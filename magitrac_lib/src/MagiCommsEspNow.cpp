// MagiCommsEspNow.cpp — ESP-NOW transport implementation

#include "MagiCommsEspNow.h"
#include "MagiMsg.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <string.h>

MagiCommsEspNow* MagiCommsEspNow::_inst = nullptr;

static void onEspNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
    if (!MagiCommsEspNow::_inst) return;
    if (len < 1) return;

    static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    bool isBcast = (memcmp(info->des_addr, BCAST, 6) == 0);

    // Spy hook — fires for every frame including broadcasts.  Lets pixel_post
    // code listen for HMAC'd pixel_post traffic regardless of the main
    // dispatcher's filtering.
    MagiCommsEspNow::_inst->invokeSpyCb(data, len);

    // Main dispatch: drop broadcasts (they're not magitrac-protocol unicasts
    // and don't carry valid MagiMsgType byte at offset 0).
    if (isBcast) return;

    memcpy(MagiCommsEspNow::_inst->_lastSender, info->src_addr, 6);
    MagiCommsEspNow::_inst->invokeRecvCb(data, len);
}

bool MagiCommsEspNow::begin() {
    _inst = this;
    WiFi.mode(WIFI_STA);
    WiFi.setChannel(1);
    while (!WiFi.STA.started()) delay(10);
    Serial.println("[MC] STA started");

    // LR-only — matches pixel_post / pixel_post_controller.  Mixed-protocol
    // mode silently drops inbound LR-rate frames on this chip, so LR-only is
    // the only configuration that lets magitrac receive pixel_post broadcasts.
    // Magitrac↔magitrac traffic is also forced to LR rate this way (~500 Kbps
    // throughput — fine for our message sizes).
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);

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
        bp.channel = 0;   // follow current WiFi channel
        bp.encrypt = false;
        esp_now_add_peer(&bp);
    }
    return esp_now_send(BROADCAST, (const uint8_t*)data, len) == ESP_OK;
}

bool MagiCommsEspNow::addPeer(const uint8_t* mac6, const uint8_t* encryptKey) {
    if (esp_now_is_peer_exist(mac6)) esp_now_del_peer(mac6);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac6, 6);
    peer.channel = 0;   // follow current WiFi channel
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

// ═══════════════════════════════════════════════════════════════════════════════
// ── Shared methods ─────────────────────────────────────────────────────────
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
