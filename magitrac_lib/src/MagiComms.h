// MagiComms.h — Abstract transport base shared by ESP-NOW and (historically)
// other backends.  MagiCommsEspNow is the only remaining concrete transport;
// reliability is the link-layer ACK (the transport's sendRaw() blocks until
// the per-frame ACK callback fires).  No app-level ACK, no retries, no
// per-frame crypto.
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

