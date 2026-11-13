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
static const uint32_t CONNECT_RETRY_MS  = 1000;  // connect() retry on STA
// Upper bound on a single send().  A half-open socket (peer alive at L2 but
// not draining its window) makes write() return 0 forever while
// peer.connected() stays true; without this the worker would spin here
// holding the mutex until keepalive declares the socket dead, stalling
// the whole link.  Reset on every byte of progress, so it bounds stalls, not
// total transfer time.
//
// 12s, not 3s: RF noise spells (gig crowds, USB3 near the bench) corrupt
// full-size data frames for tens of seconds while small frames still pass.
// TCP's retransmission backoff has multi-second gaps, so a 3s cutoff fired
// exactly between retries and killed transfers TCP would have completed.
// The receiver is a dedicated always-draining task, so a genuinely dead
// peer is keepalive's job (~10s) — this only needs to outlast RTO gaps.
static const uint32_t SEND_TIMEOUT_MS   = 12000;
// Receive-side backstop (Connect role): a frame that has started arriving but
// made no progress for this long is abandoned by dropping the connection —
// bare id+len+payload framing cannot resync from a half-frame.  Waiting costs
// nothing (the mutex is not held while a frame is incomplete), so this sits
// well past the sender's own cutoff: SEND_TIMEOUT_MS plus WiFiClient::write's
// internal 10 x 1s retry is ~20s worst case, after which the sender closes the
// socket itself.  It must never fire on a transfer the sender would finish.
static const uint32_t RECV_STALL_MS     = 30000;
// Force a fresh WiFi association if connect() keeps failing while
// WiFi.status() still reports WL_CONNECTED — a stale association left behind
// when the server's AP power-cycled.  See _runConnectLoop.
static const uint32_t STALE_ASSOC_MS    = 8000;

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
    WiFiClient        peer;
    int               kickFd     = -1;   // loopback UDP, Accept role only
    uint16_t          kickPort   = 0;
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

IPAddress MagiLink::peerIP() const {
    if (!_connected || !_impl) return IPAddress((uint32_t)0);
    if (_role == Role::Accept) return _impl->peer.remoteIP();
    return _peer;
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

    // Raw BSD listener — WiFiServer hides its fd, and select() needs it.
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    {
        int yes = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in a = {};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port        = htons(_port);
        bind(lfd, (sockaddr*)&a, sizeof(a));
        listen(lfd, 1);
    }

    // Loopback kick socket: postToPeer() datagrams land here and get
    // forwarded to the peer between transfers.  Bound to 127.0.0.1 so it
    // is unreachable from the radio side — local tasks only, no auth
    // needed.  Non-blocking so the drain loop below can never wedge.
    int kfd = socket(AF_INET, SOCK_DGRAM, 0);
    {
        sockaddr_in a = {};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons((uint16_t)(_port + 1));
        bind(kfd, (sockaddr*)&a, sizeof(a));
        fcntl(kfd, F_SETFL, O_NONBLOCK);
        _impl->kickPort = (uint16_t)(_port + 1);
        _impl->kickFd   = kfd;
    }
    Serial.printf("[LINK-ACC] listening on %s:%u (kick :%u)\n",
                  local.toString().c_str(), (unsigned)_port,
                  (unsigned)_impl->kickPort);

    // Single select loop: listener + kick + (peer when connected).  One
    // task owns all socket I/O; everything else reaches the wire either
    // through the mutexed send() (direct command responses) or through
    // the kick socket (server-initiated tiny events).  While the loop is
    // busy with one transfer, further commands queue in the TCP receive
    // buffer and further kicks queue in the UDP buffer — serialisation
    // falls out of the single-threadedness, no extra locking.
    while (true) {
        bool peerUp = _connected && _impl->peer.connected();
        if (_connected && !peerUp) {
            Serial.println("[LINK-ACC] peer disconnected");
            _connected = false;
            acquireMutex();
            _impl->peer.stop();
            releaseMutex();
            continue;
        }

        // WiFiClient buffers reads in user space; select() only sees the
        // socket.  Service buffered frames before blocking.
        if (peerUp && _impl->peer.available() > 0) {
            acquireMutex();
            if (_impl->peer.available() > 0 && _readMessageRaw()) {
                _dispatch(_recvBuf, /*len=*/ (size_t)
                          ((uint16_t)_recvBuf[1] | ((uint16_t)_recvBuf[2] << 8)));
            }
            releaseMutex();
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        FD_SET(kfd, &rfds);
        int maxfd = lfd > kfd ? lfd : kfd;
        int pfd = -1;
        if (peerUp) {
            pfd = _impl->peer.fd();
            if (pfd >= 0) {
                FD_SET(pfd, &rfds);
                if (pfd > maxfd) maxfd = pfd;
            }
        }

        // 1s ceiling so keepalive-detected peer death and any missed edge
        // get noticed even in total silence.
        timeval tv = { 1, 0 };
        int n = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (n == 0) continue;

        // New TCP connection.  Accepted even while a peer is up: a fresh
        // SYN is a perfect death signal for the current peer (the client
        // only reconnects when its side is broken) — same preempt
        // semantics as before, now event-driven instead of polled.
        if (FD_ISSET(lfd, &rfds)) {
            sockaddr_in ra;
            socklen_t   rl  = sizeof(ra);
            int         nfd = accept(lfd, (sockaddr*)&ra, &rl);
            if (nfd >= 0) {
                bool preempted = _connected;
                if (preempted) {
                    Serial.println("[LINK-ACC] preempted by new peer");
                    _connected = false;   // falling edge for the session task
                }
                acquireMutex();
                _impl->peer.stop();
                _impl->peer = WiFiClient(nfd);
                _impl->peer.setNoDelay(true);
                applyTcpKeepalive(_impl->peer);
                releaseMutex();
                if (preempted) {
                    vTaskDelay(pdMS_TO_TICKS(300));   // let session task see edge
                }
                _generation++;
                _connected = true;
                Serial.printf("[LINK-ACC] connected from %s\n",
                              _impl->peer.remoteIP().toString().c_str());
                // pfd (and any readiness flags) belong to the old peer —
                // restart the loop so the new peer gets a fresh select().
                continue;
            }
        }

        // Kick datagrams: forward each one to the peer as-is.  Drain fully;
        // with no peer connected they are read and dropped (events are
        // idempotent — a client that connects later pulls current state).
        if (FD_ISSET(kfd, &rfds)) {
            uint8_t kb[KICK_MAX_BYTES];
            while (true) {
                int g = recvfrom(kfd, kb, sizeof(kb), 0, nullptr, nullptr);
                if (g <= 0) break;
                if (_connected && g >= 3) {
                    // send() keeps the mutex on success (transactional
                    // contract) — release explicitly or this task parks in
                    // select() still holding it, starving every other sender.
                    acquireMutex();
                    send(kb, (size_t)g);
                    releaseMutex();
                }
            }
        }

        if (pfd >= 0 && FD_ISSET(pfd, &rfds)) {
            acquireMutex();
            if (_impl->peer.available() > 0 && _readMessageRaw()) {
                _dispatch(_recvBuf, /*len=*/ (size_t)
                          ((uint16_t)_recvBuf[1] | ((uint16_t)_recvBuf[2] << 8)));
            }
            releaseMutex();
        }
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
        uint32_t connectStartMs = millis();
        while (!_impl->peer.connect(_peer, _port, 5000)) {
            vTaskDelay(pdMS_TO_TICKS(CONNECT_RETRY_MS));
            // If WiFi dropped while we were retrying, jump back to outer
            // wait-for-association loop.
            if (WiFi.status() != WL_CONNECTED) break;
            // Stale-association guard: after the server's AP vanishes and
            // returns (power-cycle / crash-reboot), WiFi.status() can keep
            // reporting WL_CONNECTED against a dead association.  connect()
            // then fails forever and the outer re-association loop is never
            // re-entered — the link wedges until a manual client reboot
            // (exactly the "client wouldn't reconnect" gig failure).  Force a
            // fresh association if connects keep failing despite "connected".
            if (millis() - connectStartMs > STALE_ASSOC_MS) {
                Serial.println("[LINK-CON] connect stuck w/ WL_CONNECTED — forcing reassoc");
                WiFi.disconnect();
                vTaskDelay(pdMS_TO_TICKS(200));
                WiFi.reconnect();
                break;   // re-enter outer wait-for-association loop
            }
        }
        if (!_impl->peer.connected()) continue;  // either way, try again

        _impl->peer.setNoDelay(true);
        applyTcpKeepalive(_impl->peer);
        _generation++;
        _connected = true;
        Serial.printf("[LINK-CON] connected (local=%s)\n",
                      WiFi.localIP().toString().c_str());

        // Main loop while peer is alive: take mutex, pull in whatever bytes
        // have arrived, dispatch if that completed a frame, release, yield.
        //
        // The mutex is held only while bytes are actually being copied — never
        // across a wait for the rest of a frame.  Push frames (517B) do not
        // align with the sender's TCP send buffer, so on a slow link a frame
        // arrives in two pieces with a retransmission gap between them.  A
        // blocking read here used to sit on the mutex for that whole gap and
        // freeze the UI task on its next send()/copySong().
        //
        // Exit on EITHER peer.connected() going false OR WiFi association
        // dropping.  The WiFi check is the client's analog of the server's
        // preempt-on-accept: if the AP went down (e.g. server reset), L2
        // tells us instantly, where lwIP keepalive would take 10-30s.
        uint32_t lastMsgMs  = millis();
        uint32_t lastDiagMs = 0;
        while (_impl->peer.connected()) {
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[LINK-CON] WiFi disassociated");
                break;
            }
            acquireMutex();
            if (_pumpMessage()) {
                _dispatch(_recvBuf, (size_t)_rxLen);
                lastMsgMs = millis();
            }
            size_t rxPartial = _rxHave;
            bool   rxStalled = rxPartial > 0 &&
                               (millis() - _rxProgressMs) > RECV_STALL_MS;
            releaseMutex();
            if (rxStalled) {
                Serial.printf("[LINK-CON] frame stalled at %u bytes for %us — dropping connection\n",
                              (unsigned)rxPartial, (unsigned)(RECV_STALL_MS / 1000));
                break;
            }
            // Stall diagnostic: quiet link + full server window has been seen
            // in the field.  avail vs FIONREAD splits the suspects — data
            // stuck below WiFiClient (avail=0 sock>0), or never reaching lwIP
            // at all (both 0 while the server reports a stalled send).
            uint32_t now = millis();
            if (now - lastMsgMs > 1000 && now - lastDiagMs > 2000) {
                lastDiagMs = now;
                int av = _impl->peer.available();
                int sockPend = -1;
                int fd = _impl->peer.fd();
                if (fd >= 0) lwip_ioctl(fd, FIONREAD, &sockPend);
                Serial.printf("[LINK-CON] quiet %lums avail=%d sock=%d heap=%u\n",
                              (unsigned long)(now - lastMsgMs), av, sockPend,
                              (unsigned)ESP.getFreeHeap());
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        Serial.println("[LINK-CON] peer disconnected");
        _connected = false;
        _impl->peer.stop();
        _rxHave = 0;   // a half-frame from the dead socket must not prefix the next one
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

bool MagiLink::postToPeer(const void* msg, size_t len) {
    if (!_impl || _impl->kickFd < 0) return false;
    if (len < 3 || len > KICK_MAX_BYTES) return false;
    sockaddr_in a = {};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(_impl->kickPort);
    return sendto(_impl->kickFd, msg, len, 0,
                  (sockaddr*)&a, sizeof(a)) == (int)len;
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
                // A stalled send means the peer stopped draining its socket —
                // and if any bytes of this message already went out, the wire
                // now carries a half-frame the receiver cannot resync from
                // (framing is bare id+len+payload).  Either way the socket is
                // in a bad state: kill it so both ends tear down and the
                // normal reconnect path re-establishes a clean stream.  No
                // app-level retry — the connect edge is the recovery path.
                Serial.printf("[LINK] send stalled/failed (%u/%u bytes) — dropping connection\n",
                              (unsigned)off, (unsigned)len);
                _impl->peer.stop();
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

// Pull whatever has arrived into _recvBuf without blocking.  Returns true
// once a whole frame (id + uint16 LE length + payload) is in; false means
// "not yet" and the progress is kept in _rxHave for the next call.  On a bad
// length the socket is stopped (framing is unrecoverable) and false returned.
bool MagiLink::_pumpMessage() {
    for (;;) {
        bool   inHeader = _rxHave < 3;
        size_t want     = inHeader ? 3 : _rxLen;
        if (_rxHave < want) {
            int got = _impl->peer.read(_recvBuf + _rxHave, want - _rxHave);
            if (got <= 0) return false;            // nothing more for now
            _rxHave       += (size_t)got;
            _rxProgressMs  = millis();
            if (_rxHave < want) continue;          // short read — try once more
        }
        if (inHeader) {
            uint16_t length = (uint16_t)_recvBuf[1] | ((uint16_t)_recvBuf[2] << 8);
            if (length < 3 || length > RECV_BUF_BYTES) {
                Serial.printf("[LINK] bad length %u — dropping connection\n",
                              (unsigned)length);
                _rxHave = 0;
                _impl->peer.stop();
                return false;
            }
            _rxLen = length;
            if (length > 3) continue;              // now the payload
        }
        _rxHave = 0;                               // next call starts a new frame
        return true;                               // caller decides whether to dispatch
    }
}

// Blocking read of one whole frame.  Resumes a frame the worker had already
// started (a transactional read() must not treat the tail of a half-received
// frame as a new one).
bool MagiLink::_readMessageRaw() {
    while (!_pumpMessage()) {
        if (!_impl->peer.connected()) {
            _rxHave = 0;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
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
