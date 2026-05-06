// MagiComms.h — Transport abstraction + reliability layer
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// ── Receive callback type ────────────────────────────────────────────────────
// First byte of data is always MagiMsgType.
using MagiRecvCb = void (*)(const uint8_t* data, int len);

// ── MagiCommsTransport — abstract raw wire layer ─────────────────────────────
//
// Subclasses implement the actual transport (ESP-NOW, UART, etc.).
// No reliability logic here — just raw send/receive.

class MagiCommsTransport {
public:
    virtual ~MagiCommsTransport() = default;

    // Initialize transport hardware (WiFi + ESP-NOW, UART pins, etc.)
    virtual bool begin() = 0;

    // Send raw bytes to the connected peer.
    virtual bool sendRaw(const void* data, size_t len) = 0;

    // Broadcast to all (pairing discovery). Returns false if unsupported.
    virtual bool sendBroadcast(const void* data, size_t len) = 0;

    // Peer management — opaque address (MAC for ESP-NOW, ignored for UART).
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

protected:
    MagiRecvCb _recvCb = nullptr;
};

// ── MagiComms — concrete reliability + session layer ─────────────────────────
//
// Wraps a MagiCommsTransport and adds:
//   - sendReliable() with ACK/retry
//   - Transparent MSG_CHUNK_ACK interception
//   - Delegates everything else to the transport
//
// One global instance per device.  Singleton back-pointer used for the
// static receive callback that the transport requires.

class MagiComms {
public:
    explicit MagiComms(MagiCommsTransport& transport);

    // Initialize transport
    void begin();

    // ── Send ──────────────────────────────────────────────────────────
    // Fire-and-forget to connected peer.
    bool send(const void* data, size_t len);

    // Reliable: sends, waits for MSG_CHUNK_ACK with matching tag, retries.
    // Blocks until ACK or maxRetries exhausted. Returns true if ACKed.
    bool sendReliable(const void* data, size_t len,
                      uint8_t tag = 0,
                      uint32_t timeoutMs = 200,
                      int maxRetries = 10);

    // Broadcast (pairing discovery).
    bool sendBroadcast(const void* data, size_t len);

    // ── Peer management (delegates to transport) ──────────────────────
    bool addPeer(const uint8_t* addr, const uint8_t* lmk = nullptr);
    void removePeer(const uint8_t* addr);
    bool hasPeer() const;
    void localAddr(uint8_t* out6) const;
    const uint8_t* lastSenderAddr() const;

    // ── Receive ───────────────────────────────────────────────────────
    // Application callback — receives all messages except MSG_CHUNK_ACK
    // (which is consumed internally by the reliability layer).
    using AppRecvCb = void (*)(const uint8_t* data, int len);
    void setOnReceive(AppRecvCb cb) { _appRecvCb = cb; }

    // Maximum single-message payload
    size_t maxPayload() const { return _transport.maxPayload(); }

    // ── Internal (public for static callback access) ──────────────────
    void _handleAck(uint8_t tag);

private:
    MagiCommsTransport& _transport;
    AppRecvCb           _appRecvCb = nullptr;

    // ACK tracking for sendReliable()
    volatile bool    _ackReceived = false;
    volatile uint8_t _ackTag      = 0;

    // Singleton for static callback routing
    static MagiComms* _instance;
    static void       _onTransportRecv(const uint8_t* data, int len);
};

// ── Inline implementations ───────────────────────────────────────────────────

inline MagiComms::MagiComms(MagiCommsTransport& transport)
    : _transport(transport)
{
    _instance = this;
}

inline void MagiComms::begin() {
    _transport.setOnReceive(_onTransportRecv);
    _transport.begin();
}

inline bool MagiComms::send(const void* data, size_t len) {
    return _transport.sendRaw(data, len);
}

inline bool MagiComms::sendBroadcast(const void* data, size_t len) {
    return _transport.sendBroadcast(data, len);
}

inline bool MagiComms::addPeer(const uint8_t* addr, const uint8_t* lmk) {
    return _transport.addPeer(addr, lmk);
}

inline void MagiComms::removePeer(const uint8_t* addr) {
    _transport.removePeer(addr);
}

inline bool MagiComms::hasPeer() const {
    return _transport.hasPeer();
}

inline void MagiComms::localAddr(uint8_t* out6) const {
    _transport.localAddr(out6);
}

inline const uint8_t* MagiComms::lastSenderAddr() const {
    return _transport.lastSenderAddr();
}

inline void MagiComms::_handleAck(uint8_t tag) {
    _ackTag      = tag;
    _ackReceived = true;
}

inline void MagiComms::_onTransportRecv(const uint8_t* data, int len) {
    if (!_instance) return;
    // Intercept MSG_CHUNK_ACK (0x27) — consume it for the reliability layer
    if (len >= 2 && data[0] == 0x27) {
        _instance->_handleAck(data[1]);
        return;
    }
    if (_instance->_appRecvCb) _instance->_appRecvCb(data, len);
}

inline bool MagiComms::sendReliable(const void* data, size_t len,
                                     uint8_t tag, uint32_t timeoutMs,
                                     int maxRetries) {
    _ackReceived = false;
    _transport.sendRaw(data, len);
    int retries = 0;
    uint32_t t0 = millis();
    while (!_ackReceived || _ackTag != tag) {
        delay(1);
        if (millis() - t0 > timeoutMs) {
            if (++retries >= maxRetries) return false;
            _transport.sendRaw(data, len);
            t0 = millis();
        }
    }
    return true;
}

// Static member definition — must appear in exactly one translation unit.
// For Arduino .ino projects (single TU) the inline here is fine.
// For .cpp projects, define MAGICOMMS_IMPL in one .cpp before including.
#ifndef MAGICOMMS_NO_STATIC_DEF
inline MagiComms* MagiComms::_instance = nullptr;
#endif
