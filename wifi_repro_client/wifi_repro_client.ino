// wifi_repro_client.ino
// Target: LilyGo T5 4.7" S3 (ESP32-S3) — same hardware as the magitrac client.
//
// Role mirror of magitrac:
//   - This sketch is the AP (192.168.0.1)   — matches magitrac client.
//   - It listens on TCP port 4242            — same as MAGI_PORT.
//   - For each frame, it sends a 1-byte REQUEST, then reads a
//     2-byte length + 4096-byte payload from the connected sender
//     (wifi_repro_server.ino on M5Stack).  Verifies the incrementing
//     u8 pattern.  Reports per-burst progress on serial.
//
// Stripped down: NO SD, NO EPD, NO MIDI, NO ESP-NOW, NO app layer.
// Just WiFi + TCP, with the same framing, pacing, mode, and keepalive
// settings as the real magitrac backup path.  If THIS wedges with the
// same timing as the real backup, the wedge is fundamentally in the
// WiFi / lwIP stack.  If it runs clean for hours, the wedge is being
// caused by something the real magitrac code is doing in parallel.
//
// Pairs with wifi_repro_server.ino.

#include <WiFi.h>
#include <esp_netif.h>
#include <lwip/sockets.h>

// ── Parameters chosen to match the real backup flow exactly ────────────────
static const char*     AP_SSID = "wifirepro";
static const char*     AP_PSK  = "wifirepro1234";
static const int       AP_CH   = 1;                    // matches [SP] AP channel: 1
static const uint16_t  PORT    = 4242;                 // MAGI_PORT
static const uint16_t  FRAME_PAYLOAD = 4096;           // matches REPRO_SKIP_SERVER_SD_READ
static const uint32_t  INTER_FRAME_DELAY_MS = 80;      // matches BackupRestorePage delay(80)
static const int       BURST_SIZE = 28;                // matches typical backup file count
static const uint32_t  INTER_BURST_DELAY_MS = 500;     // brief pause then repeat
static const uint32_t  WEDGE_MS = 5000;                // log wedge if no bytes for this long
static const uint32_t  READ_TIMEOUT_MS = 30000;        // drop connection after this

// TCP keepalive — match MagiCommsTcp settings exactly.
static const int KEEPALIVE_IDLE_S  = 5;
static const int KEEPALIVE_INTVL_S = 2;
static const int KEEPALIVE_CNT     = 3;

static WiFiServer sServer(PORT);
static WiFiClient sPeer;
static uint8_t*   sBuf = nullptr;

static uint32_t sFramesInBurst   = 0;
static uint32_t sBurstNumber     = 0;
static uint64_t sTotalBytes      = 0;
static uint32_t sTotalFrames     = 0;
static uint32_t sLastDataMs      = 0;
static uint8_t  sExpected        = 0;
static uint32_t sPatternErrors   = 0;
static bool     sWedgeReported   = false;

static void applyTcpKeepalive(WiFiClient& c) {
    int sock = c.fd();
    if (sock < 0) return;
    int on    = 1;
    int idle  = KEEPALIVE_IDLE_S;
    int intvl = KEEPALIVE_INTVL_S;
    int cnt   = KEEPALIVE_CNT;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on,    sizeof(on));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
}

static bool readFully(uint8_t* dst, size_t want) {
    size_t got = 0;
    uint32_t deadline = millis() + READ_TIMEOUT_MS;
    while (got < want) {
        int n = sPeer.read(dst + got, want - got);
        if (n > 0) {
            got += n;
            sLastDataMs    = millis();
            sWedgeReported = false;
        } else {
            if (!sPeer.connected()) return false;
            uint32_t now = millis();
            if ((int32_t)(now - deadline) > 0) {
                Serial.println("[REPRO-CLIENT] read deadline");
                return false;
            }
            if (!sWedgeReported && (now - sLastDataMs) > WEDGE_MS) {
                Serial.printf("[REPRO-CLIENT] *** WEDGE DETECTED *** "
                              "no data for %u ms (mid-read got %u/%u) "
                              "burst=%u frame=%u total=%u bytes=%llu "
                              "heap=%u rssi=%d wifiStatus=%d\n",
                              now - sLastDataMs, (unsigned)got, (unsigned)want,
                              sBurstNumber, sFramesInBurst, sTotalFrames,
                              (unsigned long long)sTotalBytes,
                              ESP.getFreeHeap(), WiFi.RSSI(), (int)WiFi.status());
                sWedgeReported = true;
            }
            delay(1);
        }
    }
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[REPRO-CLIENT] boot (AP + receiver, mirrors magitrac client)");

    sBuf = (uint8_t*)heap_caps_malloc(FRAME_PAYLOAD, MALLOC_CAP_8BIT);
    if (!sBuf) {
        Serial.println("[REPRO-CLIENT] malloc fail");
        while (true) delay(1000);
    }

    // Same init order as the real magitrac sketch: persistent(false) +
    // mode(AP_STA) BEFORE the SoftAP comes up.  Magitrac uses AP_STA so
    // ESP-NOW can coexist; we use it too so the wedge has the same WiFi
    // mode underneath.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 0, 1),
                      IPAddress(192, 168, 0, 1),
                      IPAddress(255, 255, 255, 0));
    bool ok = WiFi.softAP(AP_SSID, AP_PSK, AP_CH);
    Serial.printf("[REPRO-CLIENT] softAP %s ch=%d ok=%d ip=%s\n",
                  AP_SSID, AP_CH, ok ? 1 : 0,
                  WiFi.softAPIP().toString().c_str());

    // Stop DHCP — server uses static 192.168.0.2 just like the real magitrac.
    if (esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
        esp_netif_dhcps_stop(ap);
    }

    sServer.begin();
    sServer.setNoDelay(true);
    Serial.printf("[REPRO-CLIENT] listening on %u (free heap=%u)\n",
                  PORT, ESP.getFreeHeap());
}

void loop() {
    if (!sPeer || !sPeer.connected()) {
        if (sPeer) sPeer.stop();
        WiFiClient c = sServer.available();
        if (c) {
            sPeer = c;
            sPeer.setNoDelay(true);
            applyTcpKeepalive(sPeer);
            Serial.printf("[REPRO-CLIENT] peer connected from %s\n",
                          sPeer.remoteIP().toString().c_str());
            sFramesInBurst = 0;
            sBurstNumber   = 0;
            sTotalBytes    = 0;
            sTotalFrames   = 0;
            sExpected      = 0;
            sPatternErrors = 0;
            sLastDataMs    = millis();
            sWedgeReported = false;
        } else {
            delay(10);
            return;
        }
    }

    // Send 1-byte REQUEST.  This is the analogue of MSG_REQUEST_BACKUP_FILE —
    // a small client→server write that triggers the next big server→client
    // payload.  Bidirectional load matches the real backup pattern.
    uint8_t req = 0x52;
    if (sPeer.write(&req, 1) != 1) {
        Serial.printf("[REPRO-CLIENT] REQUEST write fail after %u frames\n",
                      sTotalFrames);
        sPeer.stop();
        return;
    }

    // Read 2-byte LE length prefix
    uint8_t hdr[2];
    if (!readFully(hdr, 2)) {
        Serial.printf("[REPRO-CLIENT] disconnect on header; "
                      "burst=%u frame=%u total=%u bytes=%llu\n",
                      sBurstNumber, sFramesInBurst, sTotalFrames,
                      (unsigned long long)sTotalBytes);
        sPeer.stop();
        return;
    }
    uint16_t len = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
    if (len == 0 || len > FRAME_PAYLOAD) {
        Serial.printf("[REPRO-CLIENT] bad length %u — framing broken; dropping\n",
                      len);
        sPeer.stop();
        return;
    }

    // Read payload
    if (!readFully(sBuf, len)) {
        Serial.printf("[REPRO-CLIENT] disconnect on payload (len=%u); "
                      "burst=%u frame=%u total=%u bytes=%llu\n",
                      len, sBurstNumber, sFramesInBurst, sTotalFrames,
                      (unsigned long long)sTotalBytes);
        sPeer.stop();
        return;
    }

    // Verify incrementing-u8 pattern
    for (size_t i = 0; i < len; ++i) {
        if (sBuf[i] != sExpected) {
            sPatternErrors++;
            sExpected = sBuf[i];     // resync, count one error per skip
        }
        sExpected++;
    }

    sTotalBytes += 2 + len;
    sTotalFrames++;
    sFramesInBurst++;

    // Inter-frame pacing
    delay(INTER_FRAME_DELAY_MS);

    // Burst boundary — log progress, brief pause, start next burst
    if (sFramesInBurst >= BURST_SIZE) {
        sBurstNumber++;
        Serial.printf("[REPRO-CLIENT] burst %u OK (frames=%u, "
                      "total=%u, bytes=%llu, errs=%u, heap=%u, rssi=%d)\n",
                      sBurstNumber, sFramesInBurst,
                      sTotalFrames, (unsigned long long)sTotalBytes,
                      sPatternErrors, ESP.getFreeHeap(), WiFi.RSSI());
        sFramesInBurst = 0;
        delay(INTER_BURST_DELAY_MS);
    }
}
