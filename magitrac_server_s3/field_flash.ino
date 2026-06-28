// field_flash.ino — flash PixelPost units in the field, with no external AP.
//
// Flow:
//   1. Enter USB-MSC transfer mode (USB screen → ENTER), copy pixel_post.ino.bin
//      onto the SD card — into /firmware/ (tidy) or the root — then EJECT.
//   2. Ejecting reboots the server out of MSC mode (usb_msc.ino).  On the next
//      boot the server spots the file and shows a dedicated FLASH screen
//      (fieldFlashScreen() in magitrac_server_s3.ino).
//   3. FLASH (re)broadcasts an ESP-NOW OTA packet pointing the posts at the
//      server's OWN softAP; while that screen is up the server hosts a tiny HTTP
//      server off the SD card so the posts can pull the bin, flash, and reboot.
//   4. SKIP renames the bin to *.done and reboots the server — which both tidies
//      up everything the HTTP server touched and returns to making music.  The
//      HTTP server therefore NEVER runs during normal operation.
//
// The posts need NO firmware change — their OTA layer (LayerFirmwareUpdate)
// already takes the AP creds + URL from the broadcast packet; we just fill it
// with the server's own softAP instead of an external nginx box.
//
// Why SD + reboot rather than a live USB-serial upload: USB-CDC has no usable
// flow control for bulk binary and its tiny RX ring drops bytes under SD-write
// back-pressure.  USB-MSC block transfer is reliable, and reading the file only
// *after* the host ejects avoids dual-ownership FAT corruption.

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>           // heap_caps_malloc (PSRAM firmware cache)
#include "dhcpserver/dhcpserver.h"   // dhcps_lease_t
#include <pixelpost_proto.h>
#include "sd_mutex.h"

extern void pixelpostSendFirmwareUpdate(const char* ssid, const char* pwd, const char* url);
extern char gApSsid[];   // live softAP creds, set in setup() (magitrac_server_s3.ino)
extern char gApPsk[];

static const char* const FW_NAME = "pixel_post.ino.bin";   // URL path component
// Where the firmware may have been dropped, checked in order.  /firmware/ is
// the tidy spot; the root is accepted too so a stray drop still works.
static const char* const FW_CANDIDATES[] = {
    "/firmware/pixel_post.ino.bin",
    "/pixel_post.ino.bin",
};
static char sFwPath[48] = "";    // resolved by fieldFlashFirmwareSize()

// Resolve + size the firmware blob on SD.  Returns bytes (0 = none) and caches
// the path in sFwPath.
uint32_t fieldFlashFirmwareSize() {
    for (size_t i = 0; i < sizeof(FW_CANDIDATES) / sizeof(FW_CANDIDATES[0]); i++) {
        uint32_t sz = 0;
        { SdLock l; File f = SD.open(FW_CANDIDATES[i], FILE_READ);
          if (f) { sz = f.size(); f.close(); } }
        if (sz > 0) {
            strncpy(sFwPath, FW_CANDIDATES[i], sizeof(sFwPath));
            sFwPath[sizeof(sFwPath) - 1] = 0;
            return sz;
        }
    }
    sFwPath[0] = 0;
    return 0;
}

// ── softAP DHCP (on only while the FLASH screen is up) ──────────────────────
// setup() runs the softAP with its DHCP server STOPPED — the client uses a
// static .2, so no leases are handed out normally.  But the posts do a plain
// WiFi.begin() and need DHCP to associate, so we open a lease pool well clear
// of the server (.1) and client (.2) while flashing.  Torn down by the reboot
// on SKIP.
static void fieldFlashSetDhcp(bool on) {
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return;
    esp_netif_dhcps_stop(ap);
    if (!on) { Serial.println("[FLD] softAP DHCP off"); return; }

    dhcps_lease_t lease = {};
    lease.enable = true;
    // esp_ip4_addr_t.addr is network-order bytes — first octet in the low byte.
    lease.start_ip.addr = (uint32_t)MAGI_SERVER_IP_0 | ((uint32_t)MAGI_SERVER_IP_1 << 8) |
                          ((uint32_t)MAGI_SERVER_IP_2 << 16) | ((uint32_t)50 << 24);
    lease.end_ip.addr   = (uint32_t)MAGI_SERVER_IP_0 | ((uint32_t)MAGI_SERVER_IP_1 << 8) |
                          ((uint32_t)MAGI_SERVER_IP_2 << 16) | ((uint32_t)90 << 24);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                           &lease, sizeof(lease));
    esp_err_t err = esp_netif_dhcps_start(ap);
    Serial.printf("[FLD] softAP DHCP on (.50-.90) start=%d\n", (int)err);
}

// ── HTTP file server — non-blocking, multi-client, RAM-cached ───────────────
// The firmware is read into PSRAM once (the SD card is the shared bottleneck —
// reading it per-post is wasted work).  Posts are then served CONCURRENTLY and
// non-blocking: each screen-loop call pushes one bounded burst to every active
// client and returns immediately, so the FLASH/DONE buttons stay responsive
// mid-flash and a slow post can't head-of-line-block the others.
static WiFiServer sHttp(80);
static bool       sServeUp = false;

static uint8_t*   sFwBuf = nullptr;   // firmware cached in PSRAM
static uint32_t   sFwLen = 0;

static const int FLD_MAX_CLIENTS = 8;
struct FldClient {
    WiFiClient sock;
    uint32_t   cursor = 0;       // bytes sent so far
    bool       active = false;
};
static FldClient sClients[FLD_MAX_CLIENTS];

// Load the firmware into PSRAM, start DHCP + HTTP.  For the FLASH screen only.
void fieldFlashServeBegin() {
    if (sServeUp) return;

    sFwLen = fieldFlashFirmwareSize();        // resolves sFwPath + size
    if (sFwBuf) { free(sFwBuf); sFwBuf = nullptr; }
    if (sFwLen > 0) {
        sFwBuf = (uint8_t*)heap_caps_malloc(sFwLen, MALLOC_CAP_SPIRAM);
        if (!sFwBuf) {
            Serial.println("[FLD] PSRAM alloc FAILED — cannot serve");
        } else {
            uint32_t got = 0;
            { SdLock l; File f = SD.open(sFwPath, FILE_READ);
              if (f) {
                  while (got < sFwLen) {
                      int n = f.read(sFwBuf + got, sFwLen - got);
                      if (n <= 0) break;
                      got += (uint32_t)n;
                  }
                  f.close();
              } }
            if (got != sFwLen) {
                Serial.printf("[FLD] PSRAM load short %u/%u — freeing\n",
                              (unsigned)got, (unsigned)sFwLen);
                free(sFwBuf); sFwBuf = nullptr;
            } else {
                Serial.printf("[FLD] firmware cached in PSRAM: %u bytes\n", (unsigned)sFwLen);
            }
        }
    }

    fieldFlashSetDhcp(true);
    sHttp.begin();
    sServeUp = true;
    Serial.println("[FLD] HTTP firmware server up on :80");
}

// Drive the serve.  Accepts a new post (if any), pushes a non-blocking burst to
// every active client.  Returns how many clients FULLY finished this call.
// Call every screen-loop iteration.
int fieldFlashServePoll() {
    if (!sServeUp) return 0;
    int completed = 0;

    // Accept one new connection per call into a free slot.
    WiFiClient nc = sHttp.available();
    if (nc) {
        int slot = -1;
        for (int i = 0; i < FLD_MAX_CLIENTS; i++) if (!sClients[i].active) { slot = i; break; }
        if (slot < 0 || !sFwBuf || sFwLen == 0) {
            nc.print("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            nc.flush(); nc.stop();
        } else {
            // Drain the request headers (bounded — the GET arrives at once).
            uint32_t t0 = millis(); int nlc = 0;
            while (nc.connected() && millis() - t0 < 600) {
                int b = nc.read();
                if (b < 0) { delay(1); continue; }
                if (b == '\n') { if (++nlc >= 2) break; } else if (b != '\r') nlc = 0;
            }
            nc.printf("HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: close\r\n\r\n", (unsigned)sFwLen);
            sClients[slot].sock   = nc;
            sClients[slot].cursor = 0;
            sClients[slot].active = true;
            Serial.printf("[FLD] post connected: %s (slot %d)\n",
                          nc.remoteIP().toString().c_str(), slot);
        }
    }

    // Push a non-blocking burst to every active client from the PSRAM buffer.
    for (int i = 0; i < FLD_MAX_CLIENTS; i++) {
        if (!sClients[i].active) continue;
        WiFiClient& c = sClients[i].sock;
        if (!c.connected()) {
            Serial.printf("[FLD] slot %d dropped at %u/%u\n",
                          i, (unsigned)sClients[i].cursor, (unsigned)sFwLen);
            c.stop(); sClients[i].active = false; continue;
        }
        uint32_t rem = sFwLen - sClients[i].cursor;
        if (rem == 0) {
            c.flush(); c.stop(); sClients[i].active = false;
            completed++;
            Serial.printf("[FLD] served %u bytes (slot %d)\n", (unsigned)sFwLen, i);
            continue;
        }
        // Bounded blocking write.  WiFiClient::availableForWrite() is NOT usable
        // on this core (returns 0 → nothing ever sent), so we cap the burst
        // instead and let the write self-pace against the TCP window.  The burst
        // must comfortably exceed the ~5.7 KB TCP send buffer so it stays full
        // across loop iterations (a small burst + the loop delay left the radio
        // idle between refills).  ~16 KB keeps each write brief enough that
        // FLASH/DONE still respond within ~150 ms.
        uint32_t n = rem < 16384 ? rem : 16384;
        size_t w = c.write(sFwBuf + sClients[i].cursor, n);
        sClients[i].cursor += (uint32_t)w;
    }
    return completed;
}

// ── (Re)broadcast the OTA packet — the FLASH button ─────────────────────────
// Returns false if there's nothing to flash or the server isn't self-hosting an
// AP.  Posts that hear it switch to STA, join the softAP, and pull from the URL.
bool fieldFlashBroadcast() {
    uint32_t fsize = fieldFlashFirmwareSize();   // resolves sFwPath
    if (fsize == 0) { Serial.println("[FLD] no firmware on SD"); return false; }

    IPAddress ip = WiFi.softAPIP();
    if (ip == IPAddress(0, 0, 0, 0)) {           // only SERVER_AP can self-serve
        Serial.println("[FLD] not in SERVER_AP mode — cannot self-serve");
        return false;
    }
    if (!gApSsid[0]) { Serial.println("[FLD] live AP creds empty"); return false; }

    char url[PP_OTA_URL_LEN] = {};
    snprintf(url, sizeof(url), "http://%s/%s", ip.toString().c_str(), FW_NAME);

    // Use the LIVE softAP creds so the posts join the network that's actually
    // broadcasting — NOT srvSelfCredsGet()/magitrac_srv_self, which had drifted
    // out of sync (told posts to join a non-existent SSID).
    Serial.printf("[FLD] OTA broadcast: ssid='%s' url='%s' (%u bytes from %s)\n",
                  gApSsid, url, (unsigned)fsize, sFwPath);
    pixelpostSendFirmwareUpdate(gApSsid, gApPsk, url);
    return true;
}

// Rename the firmware to *.done so the next boot returns straight to music.
// Re-drop a fresh pixel_post.ino.bin (via USB-MSC) to arm another flash.
void fieldFlashMarkDone() {
    if (!sFwPath[0]) fieldFlashFirmwareSize();
    if (!sFwPath[0]) return;
    char done[56];
    snprintf(done, sizeof(done), "%s.done", sFwPath);
    { SdLock l; SD.remove(done); SD.rename(sFwPath, done); }
    Serial.printf("[FLD] marked done: %s -> %s\n", sFwPath, done);
}
