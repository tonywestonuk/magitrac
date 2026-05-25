// MagiCommsTcp.cpp — TCP/IP transport implementation
//
// Single background task per instance handles all connection management
// and reads:
//   • AP role: poll WiFiServer::accept() for a new peer connection, then
//     drain frames off the resulting socket.
//   • STA role: poll WiFi.status(); once associated, open a persistent
//     socket to the gateway, then drain frames.
//
// Writes happen on the caller's task and are serialised behind a mutex.
// WiFiClient::write() blocks until the kernel has accepted the bytes into
// the TCP send buffer — close enough to "the link delivered it" for our
// purposes.  Failure → tear down the socket and report false back to the
// caller, who decides what to do (no retries here, matching the pure-
// ESP-NOW transport's posture).
//
// Note on protocol mode: this transport leaves WiFi in standard B/G/N
// rates (whatever esp-idf defaults to).  The old WIFI_PROTOCOL_LR setting
// from MagiCommsEspNow is incompatible with a real WiFi AP and is dropped
// here.  If both transports run simultaneously on the same chip during
// the pixelpost migration, the last one to begin() wins the protocol
// mode — pixelpost ESP-NOW broadcasts may stop reaching LR-only peers
// until those peers also migrate to TCP.

#include "MagiCommsTcp.h"
#include "MagiMsg.h"
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <string.h>

// ── Tuning ──────────────────────────────────────────────────────────────────
static const uint32_t READER_STACK     = 4096;
static const UBaseType_t READER_PRIO   = 5;

// Set to 1 to disable the legacy TCP server + reader task while keeping
// the WiFi setup (softAP / STA association).  The new MagiLink runs on a
// separate port and provides the only live socket.  Pairing, song
// save/load, MIDI, etc. that route through gComms.send / streamBegin
// will still try to use this transport but find no peer — they're
// expected to be broken until they migrate to the new transport.
#define DISABLE_LEGACY_TCP 1
// TCP keepalive — replaces the old ESP-NOW ping/pong heartbeat that
// kept getting starved by sustained TCP traffic sharing the radio.
// Probes only fire when the link is idle, so backup-time data flow
// proves liveness for free.  Worst-case detection of a true dead
// connection: KEEPALIVE_IDLE_S + KEEPALIVE_INTVL_S * KEEPALIVE_CNT.
static const int KEEPALIVE_IDLE_S  = 5;
static const int KEEPALIVE_INTVL_S = 2;
static const int KEEPALIVE_CNT     = 3;

// Sized for the current largest single-message receive (backup file ~5 KB
// plus framing).  Heap-allocated.  Bump when song / instruments transfers
// migrate to single-blob frames — but expect PSRAM placement to backfire
// because lwip/WiFi need the reader to drain TCP fast, and PSRAM reads
// are slower than internal SRAM.
static const uint32_t MAX_FRAME        = 8192;
static const uint32_t CONNECT_RETRY_MS = 1000;
static const uint32_t POLL_IDLE_MS     = 5;
static const uint32_t READ_PAYLOAD_TIMEOUT_MS = 5000;

// ── Hidden state ────────────────────────────────────────────────────────────
struct MagiCommsTcp::Impl {
    WiFiServer*       server     = nullptr;
    WiFiClient        peer;
    SemaphoreHandle_t writeMutex = nullptr;
    TaskHandle_t      readerTask = nullptr;
    volatile bool     running    = false;
    bool              started    = false;
    uint8_t*          rxBuf      = nullptr;   // heap-allocated, MAX_FRAME bytes
};

// ── Construction / destruction ──────────────────────────────────────────────

MagiCommsTcp::MagiCommsTcp() : _impl(new Impl()) {}

// Enable lwIP TCP keepalive on a connected WiFiClient socket.  Replaces
// the old app-layer ESP-NOW ping/pong heartbeat.  Returns true if all
// four setsockopts succeeded — failure is logged but doesn't abort the
// connection (we'd rather have a connection without keepalive than no
// connection at all).
static bool applyTcpKeepalive(WiFiClient& peer, const char* logPfx) {
    int sock = peer.fd();
    if (sock < 0) return false;
    int on    = 1;
    int idle  = KEEPALIVE_IDLE_S;
    int intvl = KEEPALIVE_INTVL_S;
    int cnt   = KEEPALIVE_CNT;
    bool ok =
        (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on,    sizeof(on))    == 0) &&
        (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle))  == 0) &&
        (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) == 0) &&
        (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt))   == 0);
    if (!ok) {
        Serial.printf("%s keepalive setsockopt FAILED (errno=%d)\n",
                      logPfx, errno);
    }
    return ok;
}

MagiCommsTcp::~MagiCommsTcp() {
    // Typically a process-lifetime singleton — destruction is not exercised.
    // Best-effort cleanup only; the reader task may still hold references
    // to _impl briefly after running=false.
    if (_impl) {
        _impl->running = false;
        if (_impl->server) { _impl->server->end(); delete _impl->server; }
        delete _impl;
        _impl = nullptr;
    }
}

// ── Role configuration ──────────────────────────────────────────────────────

void MagiCommsTcp::configureAp(const char* ssid, const char* psk, uint16_t port,
                                uint8_t channel) {
    _role = Role::Ap;
    _logPfx[5] = 'C';   // [MCT-C] — magitrac client (the AP)
    strncpy(_ssid, ssid, sizeof(_ssid) - 1);  _ssid[sizeof(_ssid) - 1] = 0;
    strncpy(_psk,  psk,  sizeof(_psk)  - 1);  _psk [sizeof(_psk)  - 1] = 0;
    _port    = port;
    _channel = channel;
}

void MagiCommsTcp::configureSta(const char* ssid, const char* psk,
                                 const uint8_t my_ip[4],
                                 const uint8_t gw_ip[4],
                                 uint16_t port) {
    _role = Role::Sta;
    _logPfx[5] = 'S';   // [MCT-S] — magitrac server (the STA)
    strncpy(_ssid, ssid, sizeof(_ssid) - 1);  _ssid[sizeof(_ssid) - 1] = 0;
    strncpy(_psk,  psk,  sizeof(_psk)  - 1);  _psk [sizeof(_psk)  - 1] = 0;
    memcpy(_myIp, my_ip, 4);
    memcpy(_gwIp, gw_ip, 4);
    _port = port;
}

// ── begin() — bring up WiFi and start the reader task ───────────────────────

bool MagiCommsTcp::begin() {
    if (_role == Role::Unconfigured) {
        Serial.println("[MCT-?] begin() called before configureAp/Sta");
        return false;
    }
    if (_impl->started) return true;        // idempotent
    if (!_impl->writeMutex) _impl->writeMutex = xSemaphoreCreateMutex();
    if (!_impl->rxBuf) {
        // Internal SRAM only — PSRAM is too slow for the reader to keep
        // up with TCP backpressure.  At MAX_FRAME=8192 this barely shows
        // up on either device's heap.
        _impl->rxBuf = (uint8_t*)malloc(MAX_FRAME);
        if (!_impl->rxBuf) {
            Serial.printf("%s rxBuf alloc FAILED\n", _logPfx);
            return false;
        }
        Serial.printf("%s rxBuf %u bytes allocated\n", _logPfx, (unsigned)MAX_FRAME);
    }

    if (_role == Role::Ap) {
        Serial.printf("%s softAP ch=%u\n", _logPfx, (unsigned)_channel);
        // Disable WiFi's auto-save of SSID/PSK to NVS.  Default is TRUE —
        // every softAP() call writes the creds to the WiFi system NVS
        // namespace.  If the chip browns-out mid-write the NVS partition
        // can end up half-written and require a full flash erase to
        // recover.  We persist our own config separately, so this is
        // safe to disable.
        WiFi.persistent(false);
        // Explicit AP_STA so the coexisting ESP-NOW transport (pairing)
        // can still send/receive on the STA interface alongside the AP.
        WiFi.mode(WIFI_AP_STA);
        // Pass channel explicitly — ESP-NOW (coexisting on this radio)
        // follows the AP channel.
        bool ok = WiFi.softAP(_ssid, _psk, _channel);
        if (!ok) {
            Serial.printf("%s softAP FAILED\n", _logPfx);
            return false;
        }

        // Force the AP onto 192.168.0.1/24 so the server's static
        // 192.168.0.2/192.168.0.1-gateway config can route.  Without
        // this, ESP32 defaults to 192.168.4.1/24 and the server sits
        // unreachable.  Then stop the AP's DHCP server — its default
        // pool starts at .2 which collides with the server's static .2.
        IPAddress local(192, 168, 0, 1);
        IPAddress gw   (192, 168, 0, 1);
        IPAddress mask (255, 255, 255, 0);
        WiFi.softAPConfig(local, gw, mask);
        if (esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
            esp_netif_dhcps_stop(ap_netif);
        }
        Serial.printf("%s AP IP: %s\n", _logPfx,
                      WiFi.softAPIP().toString().c_str());

#if !DISABLE_LEGACY_TCP
        _impl->server = new WiFiServer(_port);
        _impl->server->begin();
        Serial.printf("%s AP up: SSID=%s port=%u\n",
                      _logPfx, _ssid, (unsigned)_port);
#else
        Serial.printf("%s LEGACY TCP SERVER DISABLED — MagiLink owns the socket\n",
                      _logPfx);
#endif
    } else {
        // Same persistence reasoning as the AP branch — disable WiFi's
        // auto-save of SSID/PSK to NVS so a brown-out can't corrupt the
        // partition mid-write.
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        IPAddress local(_myIp[0], _myIp[1], _myIp[2], _myIp[3]);
        IPAddress gw   (_gwIp[0], _gwIp[1], _gwIp[2], _gwIp[3]);
        IPAddress mask (255, 255, 255, 0);
        WiFi.config(local, gw, mask);
        WiFi.begin(_ssid, _psk);
        Serial.printf("%s STA joining SSID=%s static IP=%s\n",
                      _logPfx, _ssid, local.toString().c_str());
    }

    _impl->running = true;
    _impl->started = true;
#if !DISABLE_LEGACY_TCP
    xTaskCreate(&MagiCommsTcp::_readerTrampoline,
                _role == Role::Ap ? "mctcp_ap" : "mctcp_sta",
                READER_STACK, this, READER_PRIO, &_impl->readerTask);
#else
    Serial.printf("%s LEGACY READER TASK NOT STARTED — MagiLink in use\n",
                  _logPfx);
#endif
    return true;
}

// ── Reader / connection-management task ─────────────────────────────────────

void MagiCommsTcp::_readerTrampoline(void* arg) {
    static_cast<MagiCommsTcp*>(arg)->_readerLoop();
}

void MagiCommsTcp::_readerLoop() {
    uint8_t* buf = _impl->rxBuf;   // heap-allocated in begin()

    while (_impl->running) {
        // ── Establish / maintain connection ─────────────────────────────────
        if (!_impl->peer || !_impl->peer.connected()) {
            if (_hasPeer) {
                Serial.printf("%s peer disconnected\n", _logPfx);
                _hasPeer = false;
            }

            if (_role == Role::Ap) {
                WiFiClient newClient = _impl->server->accept();
                if (newClient) {
                    _impl->peer = newClient;
                    _impl->peer.setNoDelay(true);   // disable Nagle — sub-ms latency for small msgs
                    applyTcpKeepalive(_impl->peer, _logPfx);
                    _hasPeer = true;
                    memcpy(_lastSender, _peerMac, 6);
                    Serial.printf("%s peer connected from %s\n",
                                  _logPfx,
                                  _impl->peer.remoteIP().toString().c_str());
                }
            } else {
                static bool sLoggedAssoc = false;
                static uint32_t sLastConnAttempt = 0;
                wl_status_t wst = WiFi.status();
                if (wst == WL_CONNECTED) {
                    if (!sLoggedAssoc) {
                        sLoggedAssoc = true;
                        Serial.printf("%s STA associated, IP=%s\n",
                                      _logPfx,
                                      WiFi.localIP().toString().c_str());
                    }
                    uint32_t now = millis();
                    if (now - sLastConnAttempt >= CONNECT_RETRY_MS) {
                        sLastConnAttempt = now;
                        IPAddress gw(_gwIp[0], _gwIp[1], _gwIp[2], _gwIp[3]);
                        if (_impl->peer.connect(gw, _port)) {
                            _impl->peer.setNoDelay(true);   // disable Nagle
                            applyTcpKeepalive(_impl->peer, _logPfx);
                            _hasPeer = true;
                            memcpy(_lastSender, _peerMac, 6);
                            Serial.printf("%s connected to gw %s\n",
                                          _logPfx, gw.toString().c_str());
                        } else {
                            Serial.printf("%s connect to %s:%u failed, retrying\n",
                                          _logPfx, gw.toString().c_str(),
                                          (unsigned)_port);
                        }
                    }
                } else {
                    // Log status changes only — not every poll
                    static wl_status_t sLastWst = (wl_status_t)0xFF;
                    if (wst != sLastWst) {
                        sLastWst = wst;
                        Serial.printf("%s STA status=%d\n", _logPfx, (int)wst);
                    }
                }
            }

            if (!_hasPeer) {
                vTaskDelay(pdMS_TO_TICKS(CONNECT_RETRY_MS));
                continue;
            }
        }

        // ── Drain frames ────────────────────────────────────────────────────
        if (_impl->peer.available() < 2) {
            vTaskDelay(pdMS_TO_TICKS(POLL_IDLE_MS));
            continue;
        }

        uint8_t hdr[2];
        int got = _impl->peer.read(hdr, 2);
        if (got != 2) { _impl->peer.stop(); continue; }
        uint16_t flen = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
        if (flen == 0) {
            Serial.printf("%s bad frame length 0\n", _logPfx);
            _impl->peer.stop();
            continue;
        }

        // Peek the type byte (always present at offset 0 of the payload)
        // so we can route streamable types to their per-type handler
        // before allocating a buffer.
        uint8_t typeByte;
        if (_impl->peer.read(&typeByte, 1) != 1) { _impl->peer.stop(); continue; }

        StreamHandler* sh = _findStreamHandler(typeByte);
        if (sh) {
            // Hand off to the streaming handler.  It must consume exactly
            // (flen - 1) bytes via streamReadRecv().
            sh->cb((size_t)flen - 1, sh->ctx);
            continue;
        }

        // Buffered path — the type byte plus rest must fit MAX_FRAME.
        if (flen > MAX_FRAME) {
            Serial.printf("%s frame too big for buffered recv %u\n",
                          _logPfx, (unsigned)flen);
            _impl->peer.stop();
            continue;
        }
        buf[0] = typeByte;
        int total = 1;
        uint32_t t0 = millis();
        while (total < (int)flen && (millis() - t0) < READ_PAYLOAD_TIMEOUT_MS) {
            int n = _impl->peer.read(buf + total, flen - total);
            if (n < 0)  { _impl->peer.stop(); break; }
            if (n == 0) { vTaskDelay(pdMS_TO_TICKS(POLL_IDLE_MS)); continue; }
            total += n;
        }
        if (total == (int)flen) {
            invokeRecvCb(buf, flen);
        } else {
            Serial.printf("%s payload read timeout — dropping connection\n",
                          _logPfx);
            _impl->peer.stop();
        }
    }

    _impl->readerTask = nullptr;
    vTaskDelete(nullptr);
}

// ── Send ────────────────────────────────────────────────────────────────────

bool MagiCommsTcp::sendRaw(const void* data, size_t len) {
    if (!_hasPeer || len == 0 || len > maxPayload()) return false;
    if (!_impl || !_impl->writeMutex) return false;

    // Compose header + payload in one buffer so the socket sees a single
    // write — one lwip op, one TCP segment.  Smallness matters here: a
    // typical control message is < 16 B, so this lives on the stack
    // without pressure.  For the bulk-transfer path use streamMore()
    // instead, which writes user-sized chunks directly.
    uint8_t buf[256];
    if (2 + len > sizeof(buf)) {
        // Fall back to two writes for messages that don't fit the stack
        // composer.  Caller of bigger payloads should use the streaming API.
        uint8_t hdr[2] = { (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };
        xSemaphoreTake(_impl->writeMutex, portMAX_DELAY);
        bool ok = false;
        if (_impl->peer.connected()) {
            size_t w1 = _impl->peer.write(hdr, 2);
            size_t w2 = (w1 == 2) ? _impl->peer.write((const uint8_t*)data, len) : 0;
            ok = (w1 == 2 && w2 == len);
        }
        xSemaphoreGive(_impl->writeMutex);
        if (!ok) _hasPeer = false;
        return ok;
    }

    buf[0] = (uint8_t)( len        & 0xFF);
    buf[1] = (uint8_t)((len >> 8)  & 0xFF);
    memcpy(buf + 2, data, len);

    xSemaphoreTake(_impl->writeMutex, portMAX_DELAY);
    bool ok = false;
    if (_impl->peer.connected()) {
        size_t w = _impl->peer.write(buf, 2 + len);
        ok = (w == 2 + len);
    }
    xSemaphoreGive(_impl->writeMutex);

    if (!ok) _hasPeer = false;
    return ok;
}

bool MagiCommsTcp::sendBroadcast(const void* /*data*/, size_t /*len*/) {
    // TCP has no broadcast.  Returning false signals callers (e.g. pairing
    // discovery) to use a different transport — pairing still runs on
    // ESP-NOW for now.
    return false;
}

// ── Streaming send ──────────────────────────────────────────────────────────
//
// Caller pattern: streamBegin → streamMore × N → streamEnd.  Mutex is
// held the entire time so other senders block — fine, file transfers
// are infrequent and the alternative (buffer the whole 35 KB Song in
// RAM just to call sendRaw) is worse.

bool MagiCommsTcp::streamBegin(size_t totalPayloadLen) {
    if (!_hasPeer) return false;
    // streamed payloads bypass maxPayload() (the buffered-recv cap).  The
    // only hard limit is the 2-byte wire-format length prefix.
    if (totalPayloadLen == 0 || totalPayloadLen > 65535) return false;
    if (!_impl || !_impl->writeMutex) return false;

    xSemaphoreTake(_impl->writeMutex, portMAX_DELAY);
    if (!_impl->peer.connected()) {
        xSemaphoreGive(_impl->writeMutex);
        return false;
    }
    uint8_t hdr[2] = {
        (uint8_t)( totalPayloadLen        & 0xFF),
        (uint8_t)((totalPayloadLen >> 8)  & 0xFF),
    };
    if (_impl->peer.write(hdr, 2) != 2) {
        _hasPeer = false;
        xSemaphoreGive(_impl->writeMutex);
        return false;
    }
    return true;
}

bool MagiCommsTcp::streamMore(const void* data, size_t len) {
    if (!_hasPeer || len == 0) return false;
    if (!_impl || !_impl->peer.connected()) {
        _hasPeer = false;
        return false;
    }
    // peer.write() blocks until the kernel send buffer accepts the bytes
    // — that's the flow-control we want.  A short return means the link
    // died mid-write; flag and bail.
    if (_impl->peer.write((const uint8_t*)data, len) != len) {
        _hasPeer = false;
        return false;
    }
    return true;
}

void MagiCommsTcp::streamEnd() {
    if (_impl && _impl->writeMutex) xSemaphoreGive(_impl->writeMutex);
}

// ── Streaming receive ───────────────────────────────────────────────────────

bool MagiCommsTcp::registerStreamRecv(uint8_t type, StreamRecvCb cb, void* ctx) {
    if (_streamHandlerCount >= MAX_STREAM_HANDLERS) return false;
    _streamHandlers[_streamHandlerCount++] = { type, cb, ctx };
    return true;
}

MagiCommsTcp::StreamHandler* MagiCommsTcp::_findStreamHandler(uint8_t type) {
    for (int i = 0; i < _streamHandlerCount; i++)
        if (_streamHandlers[i].type == type) return &_streamHandlers[i];
    return nullptr;
}

size_t MagiCommsTcp::streamReadRecv(uint8_t* buf, size_t want) {
    if (!_impl || !_impl->peer.connected()) return 0;
    size_t total = 0;
    uint32_t t0 = millis();
    while (total < want && (millis() - t0) < READ_PAYLOAD_TIMEOUT_MS) {
        int n = _impl->peer.read(buf + total, want - total);
        if (n < 0)  { _impl->peer.stop(); break; }
        if (n == 0) { vTaskDelay(pdMS_TO_TICKS(POLL_IDLE_MS)); continue; }
        total += n;
    }
    if (total != want) {
        // Short read = either the link died, or we timed out with bytes
        // still queued upstream.  Either way, the next reader-loop frame
        // would be mis-aligned (reading mid-payload as a length prefix
        // and seeing "frame too big" garbage).  Tear the socket down so
        // framing resets cleanly on the next connection.
        Serial.printf("%s streamReadRecv short: got=%u want=%u — dropping conn\n",
                      _logPfx, (unsigned)total, (unsigned)want);
        if (_impl) _impl->peer.stop();
        _hasPeer = false;
    }
    return total;
}

// ── Peer management ─────────────────────────────────────────────────────────

bool MagiCommsTcp::addPeer(const uint8_t* mac6, const uint8_t* /*unused*/) {
    memcpy(_peerMac, mac6, 6);
    // _hasPeer stays driven by the reader task — addPeer only registers
    // who the upcoming connection is "from" for lastSenderAddr purposes.
    return true;
}

void MagiCommsTcp::removePeer(const uint8_t* mac6) {
    if (memcmp(_peerMac, mac6, 6) != 0) return;
    // Drop the current socket but keep _peerMac registered.  Auto-reconnect
    // relies on _peerMac surviving so the reader task can re-identify the
    // peer when it comes back online.  Conceptually closer to "disconnect"
    // than the ESP-NOW transport's "forget peer entirely".
    if (_impl && _impl->peer.connected()) _impl->peer.stop();
    _hasPeer = false;
}

bool MagiCommsTcp::hasPeer() const {
    return _hasPeer;
}

// ── Local + sender addresses ────────────────────────────────────────────────

void MagiCommsTcp::localAddr(uint8_t* out6) const {
    if (_role == Role::Ap)  WiFi.softAPmacAddress(out6);
    else                    WiFi.macAddress(out6);
}

const uint8_t* MagiCommsTcp::lastSenderAddr() const {
    return _lastSender;
}
