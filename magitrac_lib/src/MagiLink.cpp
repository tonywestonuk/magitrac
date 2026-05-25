// MagiLink.cpp — see header for purpose / scope.
#include "MagiLink.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <string.h>

// ── Tuning ──────────────────────────────────────────────────────────────────
// 8 KB stack — registered callbacks run on this task and can do
// non-trivial work (e.g. SD enumeration + streaming on the server).
// 4 KB was too tight and tripped the canary during the first real backup.
static const uint32_t LINK_TASK_STACK   = 8192;
static const UBaseType_t LINK_TASK_PRIO = 5;
static const uint32_t WIFI_WAIT_POLL_MS = 250;   // wait-for-WiFi-ready spacing
static const uint32_t ACCEPT_POLL_MS    = 50;    // accept() retry on AP
static const uint32_t CONNECT_RETRY_MS  = 1000;  // connect() retry on STA
static const uint32_t LINK_POLL_MS      = 100;   // peer.connected() polling

// TCP keepalive — detects silently-dropped connections (server power-cycle,
// WiFi blackhole, etc.).  Probes only fire when the link is idle; active
// data flow proves liveness for free.  Worst-case detection of a dead
// idle connection: IDLE + INTVL * CNT seconds.
static const int KEEPALIVE_IDLE_S  = 5;
static const int KEEPALIVE_INTVL_S = 2;
static const int KEEPALIVE_CNT     = 3;

static void applyTcpKeepalive(WiFiClient& peer) {
    int sock = peer.fd();
    if (sock < 0) return;
    int on    = 1;
    int idle  = KEEPALIVE_IDLE_S;
    int intvl = KEEPALIVE_INTVL_S;
    int cnt   = KEEPALIVE_CNT;
    setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE,  &on,    sizeof(on));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
}

// ── Hidden state — keeps WiFi.h out of the public header ────────────────────
struct MagiLink::Impl {
    WiFiServer*       server     = nullptr;
    WiFiClient        peer;
    TaskHandle_t      task       = nullptr;
    SemaphoreHandle_t mutex      = nullptr;   // binary semaphore (any task can give)
    volatile TaskHandle_t txHolder = nullptr; // current owner of mutex, if any
};

// One global per binary.
MagiLink gMagiLink;

// ── Public API ──────────────────────────────────────────────────────────────

void MagiLink::beginAp(uint16_t port) {
    if (!_impl) _impl = new Impl();
    if (!_impl->mutex) {
        _impl->mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(_impl->mutex);
    }
    if (_impl->task) return;            // already started
    _role = Role::Ap;
    _port = port;
    xTaskCreate(&MagiLink::_trampoline, "magilink_ap",
                LINK_TASK_STACK, this, LINK_TASK_PRIO, &_impl->task);
}

void MagiLink::beginSta(uint16_t port, IPAddress gateway) {
    if (!_impl) _impl = new Impl();
    if (!_impl->mutex) {
        _impl->mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(_impl->mutex);
    }
    if (_impl->task) return;
    _role    = Role::Sta;
    _port    = port;
    _gateway = gateway;
    xTaskCreate(&MagiLink::_trampoline, "magilink_sta",
                LINK_TASK_STACK, this, LINK_TASK_PRIO, &_impl->task);
}

// ── Worker task ─────────────────────────────────────────────────────────────

void MagiLink::_trampoline(void* arg) {
    MagiLink* self = static_cast<MagiLink*>(arg);
    if (self->_role == Role::Ap)  self->_runApLoop();
    else                          self->_runStaLoop();
    // Loops never return; if they ever do, drop the task.
    vTaskDelete(nullptr);
}

void MagiLink::_runApLoop() {
    // Wait until softAP has an IP (handles the case where begin() is called
    // before the AP is fully up).
    while (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_WAIT_POLL_MS));
    }
    Serial.printf("[LINK-AP] listening on %s:%u\n",
                  WiFi.softAPIP().toString().c_str(), (unsigned)_port);

    _impl->server = new WiFiServer(_port);
    _impl->server->begin();
    _impl->server->setNoDelay(true);

    while (true) {
        // Wait for a client to connect.
        Serial.println("[LINK-AP] waiting for peer...");
        while (true) {
            WiFiClient c = _impl->server->accept();
            if (c) {
                _impl->peer = c;
                _impl->peer.setNoDelay(true);
                applyTcpKeepalive(_impl->peer);
                _connected = true;
                Serial.printf("[LINK-AP] connected from %s\n",
                              _impl->peer.remoteIP().toString().c_str());
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(ACCEPT_POLL_MS));
        }

        // Main loop while peer is alive: take mutex, drain one message if
        // there is one, release, yield.  Transactional senders compete on
        // the same mutex — yield gives them a chance to grab it.
        while (_impl->peer.connected()) {
            acquireMutex();
            if (_impl->peer.available() > 0) {
                // Worker drains one message and dispatches via registered
                // callback (if any).  Messages without a registered handler
                // are silently dropped here — sender tasks that need them
                // must be inside a transaction (which would have the mutex
                // already, preventing the worker from running this code).
                if (_readMessageRaw()) {
                    _dispatch(_recvBuf, /*len=*/ (size_t)
                              ((uint16_t)_recvBuf[1] | ((uint16_t)_recvBuf[2] << 8)));
                }
            }
            releaseMutex();
            vTaskDelay(pdMS_TO_TICKS(1));   // tight loop — short yield
        }

        _connected = false;
        _impl->peer.stop();
        Serial.println("[LINK-AP] peer disconnected — looping back to accept");
    }
}

void MagiLink::_runStaLoop() {
    while (true) {
        // Wait until WiFi STA is associated.
        while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_WAIT_POLL_MS));
        }

        // Try to connect (retry indefinitely).
        Serial.printf("[LINK-STA] connecting to %s:%u\n",
                      _gateway.toString().c_str(), (unsigned)_port);
        while (!_impl->peer.connect(_gateway, _port, 5000)) {
            vTaskDelay(pdMS_TO_TICKS(CONNECT_RETRY_MS));
            // If WiFi dropped while we were retrying, jump back to outer
            // wait-for-association loop.
            if (WiFi.status() != WL_CONNECTED) break;
        }
        if (!_impl->peer.connected()) continue;  // either way, try again

        _impl->peer.setNoDelay(true);
        applyTcpKeepalive(_impl->peer);
        _connected = true;
        Serial.printf("[LINK-STA] connected (local=%s)\n",
                      WiFi.localIP().toString().c_str());

        // Main loop while peer is alive: take mutex, drain one message if
        // there is one, release, yield.  Same shape as AP side.
        while (_impl->peer.connected() && WiFi.status() == WL_CONNECTED) {
            acquireMutex();
            if (_impl->peer.available() > 0) {
                // Worker drains one message and dispatches via registered
                // callback (if any).  Messages without a registered handler
                // are silently dropped here — sender tasks that need them
                // must be inside a transaction (which would have the mutex
                // already, preventing the worker from running this code).
                if (_readMessageRaw()) {
                    _dispatch(_recvBuf, /*len=*/ (size_t)
                              ((uint16_t)_recvBuf[1] | ((uint16_t)_recvBuf[2] << 8)));
                }
            }
            releaseMutex();
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        _connected = false;
        _impl->peer.stop();
        Serial.println("[LINK-STA] peer disconnected — looping back to connect");
    }
}

// ── Transactional API ──────────────────────────────────────────────────────

void MagiLink::acquireMutex() {
    if (!_impl || !_impl->mutex) return;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (_impl->txHolder == me) return;             // already ours
    xSemaphoreTake(_impl->mutex, portMAX_DELAY);
    _impl->txHolder = me;
}

void MagiLink::releaseMutex() {
    if (!_impl || !_impl->mutex) return;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (_impl->txHolder == me) {
        _impl->txHolder = nullptr;
        xSemaphoreGive(_impl->mutex);
    }
}

bool MagiLink::send(const void* msg, size_t len) {
    if (!_connected || !_impl) return false;
    if (len == 0 || len > RECV_BUF_BYTES) return false;

    acquireMutex();
    if (!_impl->peer.connected()) return false;

    // Write the struct bytes directly — no extra framing prefix.
    size_t off = 0;
    const uint8_t* p = (const uint8_t*)msg;
    while (off < len) {
        int w = _impl->peer.write(p + off, len - off);
        if (w <= 0) {
            if (!_impl->peer.connected()) return false;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        off += w;
    }
    return true;
}

const uint8_t* MagiLink::read() {
    if (!_impl) return nullptr;
    acquireMutex();
    // No dispatch here — caller is explicitly draining the response and
    // takes ownership of the message bytes via the returned pointer.
    if (!_readMessageRaw()) return nullptr;
    return _recvBuf;
}

bool MagiLink::registerCallback(uint8_t msgType, MessageCb cb, void* ctx) {
    // Replace existing registration for the same id if there is one.
    for (int i = 0; i < _registeredCount; i++) {
        if (_registered[i].msgType == msgType) {
            _registered[i] = { msgType, cb, ctx };
            return true;
        }
    }
    if (_registeredCount >= MAX_REGISTERED) {
        Serial.printf("[LINK] registerCallback: table full (%d)\n",
                      MAX_REGISTERED);
        return false;
    }
    _registered[_registeredCount++] = { msgType, cb, ctx };
    return true;
}

// ── Internal helpers ───────────────────────────────────────────────────────

bool MagiLink::_readFully(uint8_t* dst, size_t n) {
    size_t off = 0;
    while (off < n) {
        int got = _impl->peer.read(dst + off, n - off);
        if (got > 0) {
            off += got;
            continue;
        }
        if (!_impl->peer.connected()) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

bool MagiLink::_readMessageRaw() {
    // id
    if (!_readFully(_recvBuf, 1)) return false;
    // length (uint16 LE)
    if (!_readFully(_recvBuf + 1, 2)) return false;
    uint16_t length = (uint16_t)_recvBuf[1] | ((uint16_t)_recvBuf[2] << 8);
    if (length < 3 || length > RECV_BUF_BYTES) {
        Serial.printf("[LINK] bad length %u — dropping connection\n",
                      (unsigned)length);
        _impl->peer.stop();
        return false;
    }
    // remaining payload
    size_t remaining = length - 3;
    if (remaining > 0) {
        if (!_readFully(_recvBuf + 3, remaining)) return false;
    }
    return true;   // caller decides whether to dispatch
}

void MagiLink::_dispatch(const uint8_t* msg, size_t len) {
    uint8_t id = msg[0];
    for (int i = 0; i < _registeredCount; i++) {
        if (_registered[i].msgType == id) {
            _registered[i].cb(msg, len, _registered[i].ctx);
            return;
        }
    }
    // No handler registered.  read() callers will pick it up via the
    // returned _recvBuf pointer; if nobody's reading either, the message
    // is silently dropped.  Add a one-time log here if we want to spot
    // accidental misses — keeping it quiet for now to avoid spam.
}
