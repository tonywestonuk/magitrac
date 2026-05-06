// MagiCommsEspNow.cpp — ESP-NOW transport implementation

#include "MagiCommsEspNow.h"
#include "MagiMsg.h"
#include <WiFi.h>
#include <string.h>

MagiCommsEspNow* MagiCommsEspNow::_inst = nullptr;

// ═══════════════════════════════════════════════════════════════════════════════
#ifdef MAGICOMMS_ESPNOW_ARDUINO3X
// ── Server: Arduino 3.x ESP32_NOW.h (class-based peers) ─────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

#include "ESP32_NOW.h"

// Forward: route received data into the transport's callback
static void routeToTransport(const uint8_t* senderMac, const uint8_t* data, int len) {
    if (!MagiCommsEspNow::_inst) return;
    memcpy(MagiCommsEspNow::_inst->_lastSender, senderMac, 6);
    MagiCommsEspNow::_inst->invokeRecvCb(data, len);
}

// ── Broadcast peer ───────────────────────────────────────────────────────────
class McBroadcastPeer : public ESP_NOW_Peer {
public:
    McBroadcastPeer()
        : ESP_NOW_Peer(ESP_NOW.BROADCAST_ADDR, 1, WIFI_IF_STA, nullptr) {}

    bool begin()   { return add(); }
    bool sendData(const uint8_t* data, size_t len) { return send(data, (int)len) > 0; }

    void onReceive(const uint8_t* data, size_t len, bool) override {
        // Broadcast messages (MSG_PAIR_REQUEST, MSG_CONNECT) carry senderMac at offset 1.
        if (len < sizeof(MsgPairRequest)) return;
        const MsgPairRequest* req = (const MsgPairRequest*)data;
        Serial.printf("[MC] bcast rx type=0x%02X from %02X:%02X:%02X:%02X:%02X:%02X\n",
            data[0],
            req->senderMac[0], req->senderMac[1], req->senderMac[2],
            req->senderMac[3], req->senderMac[4], req->senderMac[5]);
        routeToTransport(req->senderMac, data, (int)len);
    }
    void onSent(bool success) override {
        if (!success) Serial.println("[MC] beacon FAIL");
    }
};

// ── Client peer ──────────────────────────────────────────────────────────────
class McClientPeer : public ESP_NOW_Peer {
public:
    McClientPeer(const uint8_t* mac, const uint8_t* lmk = nullptr)
        : ESP_NOW_Peer(mac, 1, WIFI_IF_STA, lmk) {}

    bool begin()   { return add(); }
    void end()     { remove(); }
    bool sendData(const uint8_t* data, size_t len) { return send(data, (int)len) > 0; }

    void onReceive(const uint8_t* data, size_t len, bool) override {
        Serial.printf("[MC] rx(peer) type=0x%02X len=%d\n",
                      len > 0 ? data[0] : 0xFF, (int)len);
        if (len < 1) return;
        routeToTransport(addr(), data, (int)len);
    }
    void onSent(bool success) override {
        if (!success) Serial.println("[MC] send FAIL");
    }
};

// onNewPeer callback — messages from unregistered peers (MSG_PAIR_REQUEST, MSG_CONNECT)
static void onNewPeerCb(const esp_now_recv_info_t* info,
                        const uint8_t* data, int len, void*) {
    Serial.printf("[MC] onNewPeer type=0x%02X from %02X:%02X:%02X:%02X:%02X:%02X\n",
        len > 0 ? data[0] : 0xFF,
        info->src_addr[0], info->src_addr[1], info->src_addr[2],
        info->src_addr[3], info->src_addr[4], info->src_addr[5]);
    if (len < 1) return;
    routeToTransport(info->src_addr, data, len);
}

// ── Transport methods (Arduino 3.x) ─────────────────────────────────────────

bool MagiCommsEspNow::begin() {
    _inst = this;
    WiFi.mode(WIFI_STA);
    WiFi.setChannel(1);
    while (!WiFi.STA.started()) delay(10);
    Serial.println("[MC] STA started");

    if (!ESP_NOW.begin()) {
        Serial.println("[MC] ESP-NOW begin FAILED");
        return false;
    }
    Serial.println("[MC] ESP-NOW began OK");

    ESP_NOW.onNewPeer(onNewPeerCb, nullptr);

    auto* bp = new McBroadcastPeer();
    bool ok = bp->begin();
    Serial.printf("[MC] broadcast peer: %s\n", ok ? "OK" : "FAIL");
    if (!ok) { delete bp; bp = nullptr; }
    _broadcastPeer = bp;

    return true;
}

bool MagiCommsEspNow::sendRaw(const void* data, size_t len) {
    auto* cp = (McClientPeer*)_clientPeer;
    if (!cp) return false;
    return cp->sendData((const uint8_t*)data, len);
}

bool MagiCommsEspNow::sendBroadcast(const void* data, size_t len) {
    auto* bp = (McBroadcastPeer*)_broadcastPeer;
    if (!bp) return false;
    return bp->sendData((const uint8_t*)data, len);
}

bool MagiCommsEspNow::addPeer(const uint8_t* mac6, const uint8_t* encryptKey) {
    // Remove existing client peer
    auto* old = (McClientPeer*)_clientPeer;
    if (old) { old->end(); delete old; _clientPeer = nullptr; }

    auto* cp = new McClientPeer(mac6, encryptKey);
    bool ok = cp->begin();
    if (!ok) { delete cp; cp = nullptr; }
    Serial.printf("[MC] addPeer: %s encrypt=%d\n", ok ? "OK" : "FAIL", encryptKey != nullptr);

    _clientPeer = cp;
    if (ok) {
        memcpy(_peerMac, mac6, 6);
        _hasPeer = true;
    } else {
        _hasPeer = false;
    }
    return ok;
}

void MagiCommsEspNow::removePeer(const uint8_t* mac6) {
    auto* cp = (McClientPeer*)_clientPeer;
    if (cp && memcmp(_peerMac, mac6, 6) == 0) {
        cp->end();
        delete cp;
        _clientPeer = nullptr;
        _hasPeer = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
#else
// ── Client: legacy esp_now.h C API ──────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

#include <esp_now.h>

static void onEspNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
    if (!MagiCommsEspNow::_inst) return;
    memcpy(MagiCommsEspNow::_inst->_lastSender, info->src_addr, 6);
    MagiCommsEspNow::_inst->invokeRecvCb(data, len);
}

bool MagiCommsEspNow::begin() {
    _inst = this;
    WiFi.mode(WIFI_STA);
    WiFi.setChannel(1);
    while (!WiFi.STA.started()) delay(10);
    Serial.println("[MC] STA started");

    if (esp_now_init() != ESP_OK) {
        Serial.println("[MC] ESP-NOW init FAILED");
        return false;
    }
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("[MC] ESP-NOW init OK");
    return true;
}

bool MagiCommsEspNow::sendRaw(const void* data, size_t len) {
    if (!_hasPeer) return false;
    return esp_now_send(_peerMac, (const uint8_t*)data, len) == ESP_OK;
}

bool MagiCommsEspNow::sendBroadcast(const void* data, size_t len) {
    static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    // Ensure broadcast peer exists
    if (!esp_now_is_peer_exist(BROADCAST)) {
        esp_now_peer_info_t bp = {};
        memcpy(bp.peer_addr, BROADCAST, 6);
        bp.channel = 1;
        bp.encrypt = false;
        esp_now_add_peer(&bp);
    }
    return esp_now_send(BROADCAST, (const uint8_t*)data, len) == ESP_OK;
}

bool MagiCommsEspNow::addPeer(const uint8_t* mac6, const uint8_t* encryptKey) {
    if (esp_now_is_peer_exist(mac6)) esp_now_del_peer(mac6);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac6, 6);
    peer.channel = 1;
    if (encryptKey) {
        peer.encrypt = true;
        memcpy(peer.lmk, encryptKey, 16);
    } else {
        peer.encrypt = false;
    }
    Serial.printf("[MC] addPeer encrypt=%d\n", peer.encrypt);
    bool ok = esp_now_add_peer(&peer) == ESP_OK;
    if (ok) {
        memcpy(_peerMac, mac6, 6);
        _hasPeer = true;
    }
    return ok;
}

void MagiCommsEspNow::removePeer(const uint8_t* mac6) {
    if (esp_now_is_peer_exist(mac6)) esp_now_del_peer(mac6);
    if (memcmp(_peerMac, mac6, 6) == 0)
        _hasPeer = false;
}

#endif  // MAGICOMMS_ESPNOW_ARDUINO3X

// ═══════════════════════════════════════════════════════════════════════════════
// ── Shared methods (both APIs) ──────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

bool MagiCommsEspNow::hasPeer() const {
    return _hasPeer;
}

void MagiCommsEspNow::localAddr(uint8_t* out6) const {
    WiFi.macAddress(out6);
}

const uint8_t* MagiCommsEspNow::lastSenderAddr() const {
    return _lastSender;
}
