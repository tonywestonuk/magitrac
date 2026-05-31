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
// Upper bound on a single send().  A half-open socket (peer alive at L2 but
// not draining its window) makes write() return 0 forever while
// peer.connected() stays true; without this the worker would spin here
// holding the mutex until keepalive declares the socket dead (~10s), stalling
// the whole link.  Reset on every byte of progress, so it bounds stalls, not
// total transfer time.
static const uint32_t SEND_TIMEOUT_MS   = 3000;

// TCP keepalive — detects silently-dropped connections (server power-cycle,
// WiFi blackhole, etc.).  Probes only fire when the link is idle; active
// data flow proves liveness for free.  Worst-case detection of a dead
// idle connection: IDLE + INTVL * CNT seconds = 10s.
//
// Trade-off: a brief WiFi blip (LilyGo moved out of range mid-backup, etc.)
// has to come back inside this window or the session drops.
static const int KEEPALIVE_IDLE_S  = 4;
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

void MagiLink::beginAccept(uint16_t port) {
    if (!_impl) _impl = new Impl();
    if (!_impl->mutex) {
        _impl->mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(_impl->mutex);
    }
    if (_impl->task) return;            // already started
    _role = Role::Accept;
    _port = port;
    xTaskCreate(&MagiLink::_trampoline, "magilink_acc",
                LINK_TASK_STACK, this, LINK_TASK_PRIO, &_impl->task);
}

void MagiLink::beginConnect(uint16_t port, IPAddress peer) {
    if (!_impl) _impl = new Impl();
    if (!_impl->mutex) {
        _impl->mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(_impl->mutex);
    }
    if (_impl->task) return;
    _role = Role::Connect;
    _port = port;
    _peer = peer;
    xTaskCreate(&MagiLink::_trampoline, "magilink_con",
                LINK_TASK_STACK, this, LINK_TASK_PRIO, &_impl->task);
}

// ── Worker task ─────────────────────────────────────────────────────────────

void MagiLink::_trampoline(void* arg) {
    MagiLink* self = static_cast<MagiLink*>(arg);
    if (self->_role == Role::Accept) self->_runAcceptLoop();
    else                             self->_runConnectLoop();
    // Loops never return; if they ever do, drop the task.
    vTaskDelete(nullptr);
}

// Return the first non-zero local IP we have — either the softAP's IP
// (if the caller brought up an AP) or the STA's IP (if the caller is
// associated to an AP).  Used by the accept loop to be topology-agnostic.
static IPAddress firstLocalIp() {
    IPAddress ap  = WiFi.softAPIP();
    if ((uint32_t)ap != 0) return ap;
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP();
    return IPAddress((uint32_t)0);
}

void MagiLink::_runAcceptLoop() {
    // Wait until *any* local IP is available — softAP or STA, doesn't
    // matter to the listener.
    IPAddress local;
    while ((uint32_t)(local = firstLocalIp()) == 0) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_WAIT_POLL_MS));
    }
    Serial.printf("[LINK-ACC] listening on %s:%u\n",
                  local.toString().c_str(), (unsigned)_port);

    _impl->server = new WiFiServer(_port);
    _impl->server->begin();
    _impl->server->setNoDelay(true);

    while (true) {
        // Wait for a client to connect.
        Serial.println("[LINK-ACC] waiting for peer...");
        while (true) {
            WiFiClient c = _impl->server->accept();
            if (c) {
                _impl->peer = c;
                _impl->peer.setNoDelay(true);
                applyTcpKeepalive(_impl->peer);
                _generation++;
                _connected = true;
                Serial.printf("[LINK-ACC] connected from %s\n",
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

            // Preempt-on-new-accept: a fresh client connecting is a perfect
            // death signal for the current peer.  TCP keepalive would
            // eventually notice (~14s+, masked by WiFi association hysteresis),
            // but the new SYN tells us *now*.  Toggle _connected false→true
            // around the swap so the upper-layer session task (polling
            // isConnected() at 200ms) sees a falling edge (tears down old
            // session) followed by a rising edge (does the MSG_CONNECT/ACK
            // handshake on the new socket).
            WiFiClient incoming = _impl->server->accept();
            if (incoming) {
                Serial.printf("[LINK-ACC] preempted by new peer %s\n",
                              incoming.remoteIP().toString().c_str());
                _connected = false;
                acquireMutex();
                _impl->peer.stop();
                _impl->peer = incoming;
                _impl->peer.setNoDelay(true);
                applyTcpKeepalive(_impl->peer);
                releaseMutex();
                vTaskDelay(pdMS_TO_TICKS(300));   // let session task see edge
                _generation++;
                _connected = true;
            }

            vTaskDelay(pdMS_TO_TICKS(1));
        }

        Serial.println("[LINK-ACC] peer disconnected");
        _connected = false;
        _impl->peer.stop();
    }
}

void MagiLink::_runConnectLoop() {
    while (true) {
        // Wait until WiFi STA is associated.  Auto-reconnect alone isn't
        // enough: if the STA's first reassociation attempt fails (AP not
        // quite back yet after server reset), arduino-esp32 parks in
        // WL_CONNECT_FAILED and never tries again.  Kick it explicitly
        // every 3s while stuck.
        uint32_t waitStartMs   = millis();
        uint32_t lastReconnect = 0;
        bool     waited        = false;
        while (WiFi.status() != WL_CONNECTED) {
            waited = true;
            WiFi.setAutoReconnect(true);
            uint32_t now = millis();
            if (now - lastReconnect >= 3000) {
                lastReconnect = now;
                WiFi.reconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_WAIT_POLL_MS));
        }
        if (waited) {
            Serial.printf("[LINK-CON] WiFi associated after %ums (IP=%s)\n",
                          (unsigned)(millis() - waitStartMs),
                          WiFi.localIP().toString().c_str());
        }

        // Try to connect (retry indefinitely).
        Serial.printf("[LINK-CON] connecting to %s:%u\n",
                      _peer.toString().c_str(), (unsigned)_port);
        while (!_impl->peer.connect(_peer, _port, 5000)) {
            vTaskDelay(pdMS_TO_TICKS(CONNECT_RETRY_MS));
            // If WiFi dropped while we were retrying, jump back to outer
            // wait-for-association loop.
            if (WiFi.status() != WL_CONNECTED) break;
        }
        if (!_impl->peer.connected()) continue;  // either way, try again

        _impl->peer.setNoDelay(true);
        applyTcpKeepalive(_impl->peer);
        _generation++;
        _connected = true;
        Serial.printf("[LINK-CON] connected (local=%s)\n",
                      WiFi.localIP().toString().c_str());

        // Main loop while peer is alive: take mutex, drain one message if
        // there is one, release, yield.  Same shape as AP side.
        //
        // Exit on EITHER peer.connected() going false OR WiFi association
        // dropping.  The WiFi check is the client's analog of the server's
        // preempt-on-accept: if the AP went down (e.g. server reset), L2
        // tells us instantly, where lwIP keepalive would take 10-30s.
        while (_impl->peer.connected()) {
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[LINK-CON] WiFi disassociated");
                break;
            }
            acquireMutex();
            if (_impl->peer.available() > 0) {
                if (_readMessageRaw()) {
                    _dispatch(_recvBuf, /*len=*/ (size_t)
                              ((uint16_t)_recvBuf[1] | ((uint16_t)_recvBuf[2] << 8)));
                }
            }
            releaseMutex();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        Serial.println("[LINK-CON] peer disconnected");
        _connected = false;
        _impl->peer.stop();
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

    // If this task already holds the mutex it's running an explicit
    // transaction and owns the lock — leave it held on failure so the
    // transaction semantics are preserved.  But if send() acquired the mutex
    // itself, release it on any failure path: otherwise a caller that writes
    // `if (!send(...)) return;` (forgetting releaseMutex) would wedge the
    // worker task forever, killing the whole link until reboot.
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool weAcquired = (_impl->txHolder != me);
    acquireMutex();
    if (!_impl->peer.connected()) {
        if (weAcquired) releaseMutex();
        return false;
    }

    // Write the struct bytes directly — no extra framing prefix.
    size_t off = 0;
    const uint8_t* p = (const uint8_t*)msg;
    uint32_t lastProgressMs = millis();
    while (off < len) {
        int w = _impl->peer.write(p + off, len - off);
        if (w <= 0) {
            if (!_impl->peer.connected() ||
                (millis() - lastProgressMs) > SEND_TIMEOUT_MS) {
                if (weAcquired) releaseMutex();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        off += w;
        lastProgressMs = millis();   // forward progress resets the stall timer
    }
    return true;
}

const uint8_t* MagiLink::read() {
    if (!_impl) return nullptr;
    // Same mutex-ownership rule as send(): release on failure only if read()
    // took the lock itself, so a missing releaseMutex() at the call site
    // can't deadlock the link.
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool weAcquired = (_impl->txHolder != me);
    acquireMutex();
    // No dispatch here — caller is explicitly draining the response and
    // takes ownership of the message bytes via the returned pointer.
    if (!_readMessageRaw()) {
        if (weAcquired) releaseMutex();
        return nullptr;
    }
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
