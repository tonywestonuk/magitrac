// MagiComms.h — Transport abstraction
//
// Reliability is provided by the ESP-NOW link-layer ACK (the transport's
// sendRaw() blocks until the per-frame ACK callback fires).  No app-level
// ACK, no retries, no per-frame crypto.
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// ── Receive callback type ────────────────────────────────────────────────────
// First byte of data is always MagiMsgType.
using MagiRecvCb = void (*)(const uint8_t* data, int len);

// ── Broadcast spy ────────────────────────────────────────────────────────────
// Fires for *every* raw broadcast that arrives, before any message-type
// filtering.  Used by pixelpost_send to sync its logical clock from
// pixel_post's HMAC'd MSG_TICK_SYNC broadcasts.  Keep the callback short —
// queue and process elsewhere if you need to verify HMACs.
using MagiBroadcastSpyCb = void (*)(const uint8_t* data, int len);

// ── MagiCommsTransport — abstract raw wire layer ─────────────────────────────
//
// Subclasses implement the actual transport (ESP-NOW, UART, etc.).
// sendRaw() / sendBroadcast() block the caller until the underlying
// transport has finished the send (for ESP-NOW: until the link-layer
// ACK callback fires).

class MagiCommsTransport {
public:
    virtual ~MagiCommsTransport() = default;

    // Initialize transport hardware (WiFi + ESP-NOW, UART pins, etc.)
    virtual bool begin() = 0;

    // Send raw bytes to the connected peer.  Blocks until the link-layer
    // ACK callback fires (or a short timeout).  Returns true if the frame
    // was acknowledged by the peer.
    virtual bool sendRaw(const void* data, size_t len) = 0;

    // Broadcast to all (pairing discovery).  Broadcasts have no real ACK —
    // returns true if the send was queued OK.
    virtual bool sendBroadcast(const void* data, size_t len) = 0;

    // Peer management — opaque address (MAC for ESP-NOW, ignored for UART).
    // encryptKey is accepted for source-compat but ignored by the ESP-NOW
    // backend (link-layer encryption is disabled).
    virtual bool addPeer(const uint8_t* peerAddr, const uint8_t* encryptKey = nullptr) = 0;
    virtual void removePeer(const uint8_t* peerAddr) = 0;
    virtual bool hasPeer() const = 0;

    // This device's address (6 bytes for ESP-NOW).
    virtual void localAddr(uint8_t* out6) const = 0;

    // Address of the last message sender. Pairing uses this to learn
    // the peer's address from the first message. UART returns nullptr.
    virtual const uint8_t* lastSenderAddr() const = 0;

    // Maximum payload for a single sendRaw() call.
    virtual size_t maxPayload() const = 0;

    // Register receive callback — transport calls this from its receive context.
    void setOnReceive(MagiRecvCb cb) { _recvCb = cb; }

    // Invoke the registered callback (public so static/free callbacks can use it).
    void invokeRecvCb(const uint8_t* data, int len) {
        if (_recvCb) _recvCb(data, len);
    }

    // Broadcast spy — fires for every raw broadcast that lands, pre-filter.
    void setOnBroadcastSpy(MagiBroadcastSpyCb cb) { _spyCb = cb; }
    void invokeSpyCb(const uint8_t* data, int len) {
        if (_spyCb) _spyCb(data, len);
    }

protected:
    MagiRecvCb         _recvCb = nullptr;
    MagiBroadcastSpyCb _spyCb  = nullptr;
};

// ── MagiComms — application-facing wrapper ───────────────────────────────────
//
// With link-layer ACK doing the reliability work, this is mostly a thin
// pass-through to the transport.  Its only job beyond pass-through is to
// route the receive callback through a singleton back to the app.

class MagiComms {
public:
    explicit MagiComms(MagiCommsTransport& transport);

    // Initialize transport
    void begin();

    // Switch the underlying transport (call before begin()).  Lets the
    // magitrac_server pick MagiCommsEspNow vs MagiCommsTcp at boot time
    // based on whether stored TCP creds exist in NVS.
    void setTransport(MagiCommsTransport& t) { _transport = &t; }

    // ── Send ──────────────────────────────────────────────────────────
    // Blocks until the ESP-NOW link-layer ACK fires.  Returns true if the
    // peer ACKed the frame.
    bool send(const void* data, size_t len);

    // Same as send() — kept for source compatibility with the previous
    // sendReliable()/sendRaw() split.  The tag/timeout/maxRetries params
    // are ignored; reliability is the link-layer ACK only.  Callers that
    // need to react to a failed send should check the return value.
    bool sendReliable(const void* data, size_t len,
                      uint8_t tag = 0,
                      uint32_t timeoutMs = 0,
                      int maxRetries = 0);

    // Broadcast (pairing discovery).
    bool sendBroadcast(const void* data, size_t len);

    // ── Peer management (delegates to transport) ──────────────────────
    // lmk param accepted for source-compat; ignored — no link encryption.
    bool addPeer(const uint8_t* addr, const uint8_t* lmk = nullptr);
    void removePeer(const uint8_t* addr);
    bool hasPeer() const;
    void localAddr(uint8_t* out6) const;
    const uint8_t* lastSenderAddr() const;

    // ── Receive ───────────────────────────────────────────────────────
    using AppRecvCb = void (*)(const uint8_t* data, int len);
    void setOnReceive(AppRecvCb cb) { _appRecvCb = cb; }

    // Maximum single-message payload
    size_t maxPayload() const { return _transport->maxPayload(); }

private:
    MagiCommsTransport* _transport;
    AppRecvCb           _appRecvCb = nullptr;

    // Singleton for static callback routing
    static MagiComms* _instance;
    static void       _onTransportRecv(const uint8_t* data, int len);
};

// ── Inline implementations ───────────────────────────────────────────────────

inline MagiComms::MagiComms(MagiCommsTransport& transport)
    : _transport(&transport)
{
    _instance = this;
}

inline void MagiComms::begin() {
    _transport->setOnReceive(_onTransportRecv);
    _transport->begin();
}

inline bool MagiComms::send(const void* data, size_t len) {
    return _transport->sendRaw(data, len);
}

inline bool MagiComms::sendReliable(const void* data, size_t len,
                                     uint8_t /*tag*/, uint32_t /*timeoutMs*/,
                                     int /*maxRetries*/) {
    return _transport->sendRaw(data, len);
}

inline bool MagiComms::sendBroadcast(const void* data, size_t len) {
    return _transport->sendBroadcast(data, len);
}

inline bool MagiComms::addPeer(const uint8_t* addr, const uint8_t* lmk) {
    return _transport->addPeer(addr, lmk);
}

inline void MagiComms::removePeer(const uint8_t* addr) {
    _transport->removePeer(addr);
}

inline bool MagiComms::hasPeer() const {
    return _transport->hasPeer();
}

inline void MagiComms::localAddr(uint8_t* out6) const {
    _transport->localAddr(out6);
}

inline const uint8_t* MagiComms::lastSenderAddr() const {
    return _transport->lastSenderAddr();
}

inline void MagiComms::_onTransportRecv(const uint8_t* data, int len) {
    if (!_instance) return;
    if (_instance->_appRecvCb) _instance->_appRecvCb(data, len);
}

// Static member definition — must appear in exactly one translation unit.
// For Arduino .ino projects (single TU) the inline here is fine.
#ifndef MAGICOMMS_NO_STATIC_DEF
inline MagiComms* MagiComms::_instance = nullptr;
#endif
