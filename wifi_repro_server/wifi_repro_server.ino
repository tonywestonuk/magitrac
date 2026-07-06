// wifi_repro_server.ino
// Target: M5Stack Core Basic (ESP32) — same hardware as the magitrac server.
// FQBN: m5stack:esp32:m5stack-core-esp32
//
// Role mirror of magitrac:
//   - This sketch is the STA at static IP 192.168.0.2 — matches magitrac
//     server.  Connects to wifi_repro_client.ino's AP (192.168.0.1:4242).
//   - Reads a 1-byte REQUEST then sends a 2-byte length + 4096-byte
//     payload.  Loop forever.  Payload is incrementing u8 so the
//     receiver can verify integrity.
//
// Stripped down: NO SD, NO MIDI, NO SamplePlayer, NO ESP-NOW, NO app
// layer.  Just WiFi + TCP, same framing / pacing / mode / keepalive as
// the real MagiCommsTcp backup send path.
//
// Pairs with wifi_repro_client.ino.

#include <WiFi.h>
#include <lwip/sockets.h>

// ── Parameters chosen to match the real backup flow exactly ────────────────
static const char*     AP_SSID = "wifirepro";
static const char*     AP_PSK  = "wifirepro1234";
static const IPAddress STATIC_IP(192, 168, 0, 2);
static const IPAddress GW_IP   (192, 168, 0, 1);
static const IPAddress MASK    (255, 255, 255, 0);
static const uint16_t  PORT    = 4242;          // MAGI_PORT
static const uint16_t  FRAME_PAYLOAD = 4096;    // matches REPRO_SKIP_SERVER_SD_READ
static const uint32_t  WRITE_TIMEOUT_MS = 10000;
static const uint32_t  REQ_WAIT_TIMEOUT_MS = 30000;
static const uint32_t  REPORT_PERIOD_MS = 5000;

// TCP keepalive — match MagiCommsTcp settings exactly.
static const int KEEPALIVE_IDLE_S  = 5;
static const int KEEPALIVE_INTVL_S = 2;
static const int KEEPALIVE_CNT     = 3;

static WiFiClient sClient;
static uint8_t*   sBuf            = nullptr;
static uint8_t    sPattern        = 0;
static uint64_t   sTotalBytes     = 0;
static uint32_t   sTotalFrames    = 0;
static uint32_t   sLastReportMs   = 0;

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

static bool writeAll(const uint8_t* p, size_t n) {
    size_t off = 0;
    uint32_t deadline = millis() + WRITE_TIMEOUT_MS;
    while (off < n) {
        size_t want = n - off;
        if (want > 1460) want = 1460;
        int w = sClient.write(p + off, want);
        if (w > 0) {
            off += w;
        } else {
            if (!sClient.connected()) return false;
            if ((int32_t)(millis() - deadline) > 0) return false;
            delay(1);
        }
    }
    return true;
}

static void fillFrame(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) p[i] = sPattern++;
}

static void connectWifi() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.config(STATIC_IP, GW_IP, MASK);
    WiFi.begin(AP_SSID, AP_PSK);
    Serial.printf("[REPRO-SERVER] STA connecting to %s", AP_SSID);
    uint32_t deadline = millis() + 30000;
    while (WiFi.status() != WL_CONNECTED) {
        if ((int32_t)(millis() - deadline) > 0) {
            Serial.println(" — timeout, restarting");
            delay(500);
            ESP.restart();
        }
        delay(250);
        Serial.print('.');
    }
    Serial.printf("\n[REPRO-SERVER] STA IP=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[REPRO-SERVER] boot (STA + sender, mirrors magitrac server)");

    sBuf = (uint8_t*)heap_caps_malloc(FRAME_PAYLOAD, MALLOC_CAP_8BIT);
    if (!sBuf) {
        Serial.println("[REPRO-SERVER] malloc fail");
        while (true) delay(1000);
    }
    connectWifi();
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[REPRO-SERVER] WiFi dropped — reconnecting");
        connectWifi();
        if (sClient) sClient.stop();
        return;
    }

    if (!sClient.connected()) {
        if (sClient) sClient.stop();
        Serial.printf("[REPRO-SERVER] connecting to %s:%u\n",
                      GW_IP.toString().c_str(), PORT);
        if (sClient.connect(GW_IP, PORT, 5000)) {
            sClient.setNoDelay(true);
            applyTcpKeepalive(sClient);
            Serial.printf("[REPRO-SERVER] connected (heap=%u)\n",
                          ESP.getFreeHeap());
            sTotalBytes   = 0;
            sTotalFrames  = 0;
            sPattern      = 0;
            sLastReportMs = millis();
        } else {
            Serial.println("[REPRO-SERVER] connect fail");
            delay(1000);
            return;
        }
    }

    // Wait for one REQUEST byte from the client
    uint8_t req;
    uint32_t reqDeadline = millis() + REQ_WAIT_TIMEOUT_MS;
    int n;
    while (sClient.connected() && (n = sClient.read(&req, 1)) <= 0) {
        if ((int32_t)(millis() - reqDeadline) > 0) {
            Serial.printf("[REPRO-SERVER] REQUEST wait timeout; "
                          "frames=%u bytes=%llu — dropping\n",
                          sTotalFrames, (unsigned long long)sTotalBytes);
            sClient.stop();
            return;
        }
        delay(1);
    }
    if (!sClient.connected()) {
        Serial.printf("[REPRO-SERVER] peer dropped while awaiting REQUEST; "
                      "frames=%u bytes=%llu\n",
                      sTotalFrames, (unsigned long long)sTotalBytes);
        return;
    }

    // Send one frame: 2-byte LE length + 4096-byte payload
    uint8_t hdr[2] = { (uint8_t)(FRAME_PAYLOAD & 0xff),
                       (uint8_t)((FRAME_PAYLOAD >> 8) & 0xff) };
    if (!writeAll(hdr, 2)) {
        Serial.printf("[REPRO-SERVER] header write fail after %u frames "
                      "(heap=%u, rssi=%d, wifiStatus=%d)\n",
                      sTotalFrames, ESP.getFreeHeap(),
                      WiFi.RSSI(), (int)WiFi.status());
        sClient.stop();
        return;
    }
    fillFrame(sBuf, FRAME_PAYLOAD);
    if (!writeAll(sBuf, FRAME_PAYLOAD)) {
        Serial.printf("[REPRO-SERVER] payload write fail after %u frames "
                      "(heap=%u, rssi=%d, wifiStatus=%d)\n",
                      sTotalFrames, ESP.getFreeHeap(),
                      WiFi.RSSI(), (int)WiFi.status());
        sClient.stop();
        return;
    }
    sTotalBytes += 2 + FRAME_PAYLOAD;
    sTotalFrames++;

    uint32_t now = millis();
    if (now - sLastReportMs >= REPORT_PERIOD_MS) {
        Serial.printf("[REPRO-SERVER] frames=%u bytes=%llu heap=%u rssi=%d\n",
                      sTotalFrames, (unsigned long long)sTotalBytes,
                      ESP.getFreeHeap(), WiFi.RSSI());
        sLastReportMs = now;
    }
}
