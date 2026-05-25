// MagiCommsEspNow.cpp — ESP-NOW transport implementation
//
// Pure ESP-NOW: link-layer ACK is the only reliability mechanism, no
// LMK encryption, no per-frame HMAC.  Every esp_now_send() call blocks
// the caller until the send-callback fires so the return value reflects
// the actual link-layer ACK status.

#include "MagiCommsEspNow.h"
#include "MagiMsg.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_arduino_version.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// esp32-arduino 3.3+ changed the send-cb's first param from `const uint8_t*`
// (mac) to `const wifi_tx_info_t*` (struct).  Pick the right signature so
// this file builds on both magitrac (m5stack:esp32 3.2.x) and magitrac_server
// (esp32:esp32 3.3.x).
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
  #define MAGICOMMS_SEND_CB_PARAM const wifi_tx_info_t* /*info*/
#else
  #define MAGICOMMS_SEND_CB_PARAM const uint8_t* /*mac*/
#endif

MagiCommsEspNow* MagiCommsEspNow::_inst = nullptr;

// The ESP-NOW send-cb is global and gives no way to attribute a status
// to a particular esp_now_send() call.  We serialise all sends behind a
// mutex and wait for the cb signal before releasing it, so each caller
// sees its own ACK status.
static SemaphoreHandle_t s_sendMutex = nullptr;
static SemaphoreHandle_t s_sendDone  = nullptr;
static volatile esp_now_send_status_t s_lastSendStatus = ESP_NOW_SEND_SUCCESS;
static const uint32_t SEND_TIMEOUT_MS = 50;

static void onEspNowSend(MAGICOMMS_SEND_CB_PARAM, esp_now_send_status_t status) {
    s_lastSendStatus = status;
    if (s_sendDone) xSemaphoreGive(s_sendDone);
}

static void onEspNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
    if (!MagiCommsEspNow::_inst) return;
    if (len < 1) return;

    static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    bool isBcast = (memcmp(info->des_addr, BCAST, 6) == 0);

    // Spy hook — fires for every frame including broadcasts.  Lets pixel_post
    // code listen for HMAC'd pixel_post traffic regardless of the main
    // dispatcher's filtering.
    MagiCommsEspNow::_inst->invokeSpyCb(data, len);

    // Main dispatch:
    //   Unicasts → always.
    //   Broadcasts → only if a magitrac MSG_PAIR_PROBE — the magitrac
    //     server broadcasts these while scanning channels 1/6/11 in pair
    //     mode.  The MAGI_PAIR_MAGIC prefix at offset 1 disambiguates the
    //     frame from random channel chatter.  CHALLENGE and OFFER are
    //     unicast, so they don't appear in this branch.
    if (isBcast) {
        if (data[0] != MSG_PAIR_PROBE) return;
        if (len < 1 + (int)sizeof(MAGI_PAIR_MAGIC) - 1) return;
        if (memcmp(data + 1, MAGI_PAIR_MAGIC, sizeof(MAGI_PAIR_MAGIC) - 1) != 0) return;
    }

    memcpy(MagiCommsEspNow::_inst->_lastSender, info->src_addr, 6);
    MagiCommsEspNow::_inst->invokeRecvCb(data, len);
}

bool MagiCommsEspNow::begin() {
    _inst = this;

    // In coexist mode another transport (MagiCommsTcp AP role) has already
    // taken WiFi up in AP_STA mode; touching WiFi.mode / channel / protocol
    // here would clobber that setup and tear down the TCP listener.
    if (!_coexist) {
        WiFi.mode(WIFI_STA);
        WiFi.setChannel(1);
        while (!WiFi.STA.started()) delay(10);
        Serial.println("[MC] STA started");

        // WIFI_PROTOCOL_LR DELIBERATELY NOT SET.  In the TCP-transport
        // era this MagiCommsEspNow is only used during the pairing
        // ceremony (server boots ESP-NOW only when there are no TCP
        // creds; client uses coexist mode which already skipped LR).
        // LR-only mode silently dropped larger pairing frames (e.g. the
        // 106-byte MSG_PAIR_OFFER) because the client AP transmits them
        // at standard 802.11n rates the LR receiver can't decode — small
        // frames sneak through at base rate but anything past ~30 bytes
        // can fail.  Trade-off: pixelpost MSG_TICK_SYNC broadcast sync
        // may not be received by the server.  Acceptable — the server is
        // a pixelpost *sender*, not a pure receiver, and pixelpost is
        // planned to move to TCP anyway.
    } else {
        Serial.println("[MC] coexist mode — WiFi already up, skipping setup");
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("[MC] ESP-NOW init FAILED");
        return false;
    }

    // Force ESP-NOW frames to 1 Mbps DSSS — the slowest, most-reliable rate.
    // Without this, the WiFi driver does rate adaptation per-frame and tends
    // to bump larger unicasts (e.g. 106-byte MSG_PAIR_OFFER) up to rates an
    // unassociated peer can't reliably decode.  Small frames sneak through
    // at base rate; larger ones get silently dropped.  Forcing 1M is bullet-
    // proof for our pairing-traffic sizes.  Setting on both AP and STA so
    // the same code works for the coexist (client AP-STA) and pure-STA
    // (server) configs.
    esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
    esp_wifi_config_espnow_rate(WIFI_IF_AP,  WIFI_PHY_RATE_1M_L);
    Serial.println("[MC] ESP-NOW rate forced to 1 Mbps DSSS");

    if (!s_sendMutex) s_sendMutex = xSemaphoreCreateMutex();
    if (!s_sendDone)  s_sendDone  = xSemaphoreCreateBinary();

    esp_now_register_send_cb(onEspNowSend);
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("[MC] ESP-NOW init OK");
    return true;
}

bool MagiCommsEspNow::sendRaw(const void* data, size_t len) {
    if (!_hasPeer) return false;
    if (!s_sendMutex || !s_sendDone) return false;

    xSemaphoreTake(s_sendMutex, portMAX_DELAY);
    xSemaphoreTake(s_sendDone, 0);   // drain any stale signal from a prior late cb

    esp_err_t err = esp_now_send(_peerMac, (const uint8_t*)data, len);
    bool ok = false;
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_sendDone, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) == pdTRUE) {
            ok = (s_lastSendStatus == ESP_NOW_SEND_SUCCESS);
        }
    }

    xSemaphoreGive(s_sendMutex);
    return ok;
}

bool MagiCommsEspNow::sendBroadcast(const void* data, size_t len) {
    static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (!s_sendMutex || !s_sendDone) return false;

    // Ensure broadcast peer exists.  In coexist mode (AP is up alongside
    // ESP-NOW), bind the peer to the AP interface — STA is up but not
    // associated, and esp_now_send returns ESP_ERR_ESPNOW_IF if the
    // peer's ifidx doesn't match an interface able to transmit.
    if (!esp_now_is_peer_exist(BROADCAST)) {
        esp_now_peer_info_t bp = {};
        memcpy(bp.peer_addr, BROADCAST, 6);
        bp.channel = 0;
        bp.encrypt = false;
        bp.ifidx   = _coexist ? WIFI_IF_AP : WIFI_IF_STA;
        esp_now_add_peer(&bp);
    }

    xSemaphoreTake(s_sendMutex, portMAX_DELAY);
    xSemaphoreTake(s_sendDone, 0);

    esp_err_t err = esp_now_send(BROADCAST, (const uint8_t*)data, len);
    if (err == ESP_OK) xSemaphoreTake(s_sendDone, pdMS_TO_TICKS(SEND_TIMEOUT_MS));

    xSemaphoreGive(s_sendMutex);
    return err == ESP_OK;
}

bool MagiCommsEspNow::addPeer(const uint8_t* mac6, const uint8_t* /*encryptKey*/) {
    if (esp_now_is_peer_exist(mac6)) esp_now_del_peer(mac6);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac6, 6);
    peer.channel = 0;
    peer.encrypt = false;
    peer.ifidx   = _coexist ? WIFI_IF_AP : WIFI_IF_STA;
    esp_err_t err = esp_now_add_peer(&peer);
    bool ok = (err == ESP_OK);
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

bool MagiCommsEspNow::hasPeer() const {
    return _hasPeer;
}

void MagiCommsEspNow::localAddr(uint8_t* out6) const {
    WiFi.macAddress(out6);
}

const uint8_t* MagiCommsEspNow::lastSenderAddr() const {
    return _lastSender;
}
