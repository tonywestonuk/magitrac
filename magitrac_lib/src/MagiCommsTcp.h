// MagiCommsTcp.h — TCP/IP transport for MagiComms
//
// Drop-in alternative to MagiCommsEspNow.  One device runs as the access
// point (the magitrac client at 192.168.0.1); the other(s) join as STA
// peers with static IPs handed out at pairing time.  There is no DHCP —
// IP allocation is in-band during the (still ESP-NOW based) pairing
// ceremony, persisted in NVS, and reapplied statically on every boot.
//
// ── Wire framing ─────────────────────────────────────────────────────────────
// 2-byte little-endian length prefix + raw MagiMsg payload.  One persistent
// TCP socket per peer.  Reads happen on a background task and dispatch to
// the registered recv callback exactly like the ESP-NOW transport does.
//
// ── 1:N ──────────────────────────────────────────────────────────────────────
// The AP-side keeps an internal peer table indexed by MAC and accepts as
// many concurrent connections as configured peers.  Today only the magitrac
// server is paired (always at .2), so sendRaw() targets that single peer.
// When pixelposts move onto this transport later, a sendRawTo(mac, ...)
// will be added — magitrac↔server flows don't need it because they only
// ever talk to one peer.
//
// ── Coexistence with ESP-NOW ─────────────────────────────────────────────────
// SoftAP-mode and STA-mode both leave ESP-NOW usable on the same chip
// (pixelpost broadcasts still ride on MagiCommsEspNow until pixelpost is
// migrated).  Channel follows whichever interface the WiFi stack actually
// connects on; the existing MSG_SET_WIFI_CHANNEL UI keeps working.
#pragma once
#include "MagiComms.h"
#include <stdint.h>
#include <stddef.h>

// Default port for the magitrac transport — used for BOTH the TCP listener
// (reliable, framed) and the UDP listener (best-effort datagrams).  TCP
// and UDP ports are independent namespaces in the IP stack so they coexist
// on the same number without conflict.  Both client (AP listener) and
// server (STA outbound) use this — kept here so the two sides can't drift
// out of sync.
#define MAGI_PORT 4242

class MagiCommsTcp : public MagiCommsTransport {
public:
    MagiCommsTcp();
    ~MagiCommsTcp() override;

    // ── Role configuration — call exactly one of these before begin() ───────

    // AP / "magitrac client" side.
    // Brings up SoftAP on 192.168.0.1 with the supplied SSID/PSK.
    // The built-in DHCP server is disabled — peers must use the static
    // IPs they were issued during pairing.
    // `channel` must match what the server (and any ESP-NOW peers) use —
    // typically loaded from the user's persisted WiFi-channel setting
    // before calling begin().  Pass 1, 6, or 11.
    void configureAp(const char* ssid, const char* psk, uint16_t port,
                     uint8_t channel = 1);

    // STA / "magitrac_server" or "pixelpost" side.
    // Joins ssid/psk, configures the static {my_ip, gw_ip} on the STA
    // interface, then opens a persistent socket to gw_ip:port.
    void configureSta(const char*   ssid,
                      const char*   psk,
                      const uint8_t my_ip[4],
                      const uint8_t gw_ip[4],
                      uint16_t      port);

    // ── MagiCommsTransport overrides ─────────────────────────────────────────
    bool   begin() override;
    bool   sendRaw(const void* data, size_t len) override;
    bool   sendBroadcast(const void* data, size_t len) override;  // no-op (TCP has no broadcast)
    bool   addPeer(const uint8_t* mac6, const uint8_t* = nullptr) override;
    void   removePeer(const uint8_t* mac6) override;
    bool   hasPeer() const override;
    void   localAddr(uint8_t* out6) const override;
    const uint8_t* lastSenderAddr() const override;

    // 2-byte length prefix caps any single frame at 65535 bytes; the
    // reader's heap buffer (MAX_FRAME in MagiCommsTcp.cpp) limits us in
    // practice.  Currently sized for backup files; bump alongside the
    // reader buffer when we migrate song/instruments transfers off the
    // ESP-NOW-era chunked path.
    size_t maxPayload() const override { return 8192; }

    // ── Streaming send — write a single frame in pieces ─────────────────────
    // Pattern:
    //   streamBegin(totalPayloadLen);   // mutex held, length prefix written
    //   streamMore(typeByte, 1);        // first payload byte = message type
    //   streamMore(metadata, ...);
    //   while (...) streamMore(buf, n); // e.g. SD → buf → socket
    //   streamEnd();                    // mutex released
    // The bytes written across all streamMore calls MUST sum to
    // totalPayloadLen, or the receiver will mis-frame the next message.
    // streamMore blocks if the kernel's TCP send buffer is full — that's
    // the flow-control mechanism, no app-level chunking or pacing needed.
    bool streamBegin(size_t totalPayloadLen);
    bool streamMore(const void* data, size_t len);
    void streamEnd();

    // ── Streaming receive — bytes-into-handler, no app buffer ───────────────
    // For receiving payloads larger than MAX_FRAME (e.g. a 35 KB Song
    // struct).  Register a handler per message-type byte: when the reader
    // sees that type at the start of a frame, it hands off to the handler
    // and the handler reads bytes itself via streamReadRecv().
    //   • Handler runs on the reader task — keep work tight.
    //   • Handler MUST consume exactly remainingLen bytes; otherwise the
    //     next frame's framing breaks.
    //   • If a type has no streaming handler registered, the reader falls
    //     back to buffered dispatch (fits within MAX_FRAME).
    using StreamRecvCb = void (*)(size_t remainingLen, void* userCtx);
    bool   registerStreamRecv(uint8_t type, StreamRecvCb cb, void* userCtx);

    // Read `want` bytes from the current frame into `buf`.  Returns the
    // actual byte count (less than `want` only if the link dies or the
    // payload-read timeout expires).  Call only from inside a stream-recv
    // handler.
    size_t streamReadRecv(uint8_t* buf, size_t want);

    // ── New struct-driven send/recv API ──────────────────────────────────────
    //
    // Procedural transaction model — applications send and recv whole
    // MagiMsg structs.  Every struct's first byte is the message id and
    // bytes 1-2 are the length (so the receiver can sanity-check before
    // casting).
    //
    // Mutex model:
    //   - send() and recv() both take a shared transaction mutex if this
    //     task doesn't already hold it.  The mutex stays held until
    //     releaseMutex() is called.
    //   - Within a transaction, other tasks that try to send/recv block
    //     until release.
    //   - Forgetting to release blocks everyone else — keep transactions
    //     tight.  Typical pattern:
    //
    //         send(&req,  sizeof(req));            // takes mutex
    //         auto* hdr = (MsgFooHeader*)recv();   // mutex still held
    //         while (auto* b = recv(), b && b[0] == MSG_FOO_BODY) {
    //             process(b);
    //         }
    //         releaseMutex();
    //
    // Async/unsolicited messages (MIDI in, status updates) never go
    // through recv() — register a handler with registerMessageCallback
    // and the reader task fires it directly.

    using MessageCb = void (*)(const uint8_t* msg, size_t len, void* ctx);

    // Send one message.  Takes the transaction mutex if this task doesn't
    // already hold it.  Returns false if the connection is down or the
    // write fails (mutex stays held — call releaseMutex() to clean up).
    bool send(const void* msg, size_t len);

    // Block until the next message arrives that has NO registered handler.
    // Returns a pointer to an internal buffer (caller may cast to a
    // message struct).  Returns nullptr on timeout or disconnect.  Buffer
    // is valid until the next send()/recv()/releaseMutex().
    //
    // Takes the transaction mutex if this task doesn't already hold it.
    const uint8_t* recv(uint32_t timeout_ms = 0xFFFFFFFFu);

    // Release the transaction mutex (if this task holds it).  Safe to
    // call even if no transaction is active.
    void releaseMutex();

    // Register a permanent handler for messages of one type.  Replaces
    // any existing registration for the same type.  Returns false if the
    // table is full.  Handler runs on the reader task — keep it fast and
    // non-blocking.  Messages with a registered handler never reach recv().
    bool registerMessageCallback(uint8_t msgType, MessageCb cb, void* ctx = nullptr);

    // Unregister a previously-set handler.  Safe to call if none registered.
    void unregisterMessageCallback(uint8_t msgType);

private:
    enum class Role : uint8_t { Unconfigured, Ap, Sta };
    Role     _role = Role::Unconfigured;

    // Per-instance log prefix.  AP role (magitrac client) gets "[MCT-C]"
    // and STA role (server) gets "[MCT-S]" so combined serial captures
    // from both devices can be told apart by line.  Filled in by
    // configureAp/configureSta; left as "[MCT-?]" until then.
    char     _logPfx[8] = "[MCT-?]";

    // Streaming-receive registration (small fixed table — only a handful
    // of message types need streaming).
    struct StreamHandler { uint8_t type; StreamRecvCb cb; void* ctx; };
    static const int MAX_STREAM_HANDLERS = 8;
    StreamHandler    _streamHandlers[MAX_STREAM_HANDLERS];
    int              _streamHandlerCount = 0;
    StreamHandler*   _findStreamHandler(uint8_t type);

    // ── New struct-driven dispatch state ─────────────────────────────────────
    struct RegisteredHandler { uint8_t msgType; MessageCb cb; void* ctx; };
    static const int MAX_REGISTERED = 32;
    RegisteredHandler _registered[MAX_REGISTERED];
    int               _registeredCount = 0;

    // Buffer that recv() returns a pointer into.  Single-slot — safe
    // because only the current mutex-holder can call recv(), and they
    // get a fresh fill on each call.
    static const size_t RECV_BUF_BYTES = 1029;   // sizeof MsgBackupBody
    uint8_t  _recvBuf[RECV_BUF_BYTES] = {};
    size_t   _recvBufLen              = 0;

    // Try the new dispatch (registered handler → recv queue).  Returns
    // true if handled; false to fall through to legacy setOnReceive.
    bool _dispatchNew(const uint8_t* msg, size_t len);

    // Shared config
    char     _ssid[33] = {};
    char     _psk[64]  = {};
    uint16_t _port     = 0;
    uint8_t  _channel  = 1;

    // STA-only config
    uint8_t  _myIp[4]  = {};
    uint8_t  _gwIp[4]  = {};

    // Active peer state (current single-peer use; will grow to a table
    // when pixelposts join — kept here for source-compat with the existing
    // single-peer transport API).
    uint8_t  _peerMac[6]    = {};
    bool     _hasPeer       = false;
    uint8_t  _lastSender[6] = {};

    // Opaque implementation state (sockets, tasks, mutexes).  Hidden so
    // WiFi.h / lwip headers stay out of this public interface.
    struct Impl;
    Impl*    _impl = nullptr;

    // Background reader/connect task — one per transport instance.  Handles
    // accept (AP) or connect-retry (STA), then drains length-prefixed frames
    // and dispatches them to the recv callback.
    static void _readerTrampoline(void* arg);
    void        _readerLoop();
};
