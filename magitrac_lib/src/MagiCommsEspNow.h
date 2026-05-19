// MagiCommsEspNow.h — ESP-NOW transport for MagiComms
//
// Uses the legacy esp_now.h C API.  An older variant of this file once
// supported the Arduino 3.x ESP32_NOW class API behind a #ifdef, but the
// sketch-level #define never propagated to library compilation so the class
// branch never actually ran.  Removed.
#pragma once
#include "MagiComms.h"

class MagiCommsEspNow : public MagiCommsTransport {
public:
    bool   begin() override;
    bool   sendRaw(const void* data, size_t len) override;
    bool   sendBroadcast(const void* data, size_t len) override;
    bool   addPeer(const uint8_t* mac6, const uint8_t* encryptKey = nullptr) override;
    void   removePeer(const uint8_t* mac6) override;
    bool   hasPeer() const override;
    void   localAddr(uint8_t* out6) const override;
    const uint8_t* lastSenderAddr() const override;
    size_t maxPayload() const override { return 250; }

    // Public for static callback access from .cpp
    static MagiCommsEspNow* _inst;
    uint8_t _lastSender[6] = {};

private:
    uint8_t _peerMac[6] = {};
    bool    _hasPeer   = false;
};
