// usb_msc.ino — expose the SD card to a host computer as USB Mass Storage.
//
// Uses the ESP32-Arduino core's USBMSC + SdFat (SPI).  Picked over
// Adafruit_TinyUSB because the core's USBMSC works without CDC-On-Boot, which
// keeps the boot-time USB-Serial-JTAG flash port alive — USB OTG only takes
// over the port when runUsbMsc() is actually entered.
//
// Card format note: 128 GB SDXC / exFAT cards enumerate but do NOT mount on
// macOS.  Use a FAT32 card for transfer (≤32 GB).
//
// Board settings required (Tools menu):
//   USB Mode               = USB-OTG (TinyUSB)
//   USB CDC On Boot        = Disabled        ← keeps the flash port stable
//   USB Firmware MSC       = Disabled
//   USB DFU On Boot        = Disabled

#include "sdkconfig.h"

#if CONFIG_TINYUSB_MSC_ENABLED

#include <SPI.h>
#include "SdFat.h"
#include "USB.h"
#include "USBMSC.h"

// CoreS3 SD on SPI: SCK=G36, MISO=G35, MOSI=G37, CS=G4
#define MSC_SD_SCK   36
#define MSC_SD_MISO  35
#define MSC_SD_MOSI  37
#define MSC_SD_CS    4

static SdFat  sSd;
static USBMSC sMsc;

// Set true when the host issues an eject (START STOP UNIT, start=0, load_eject=1).
// Acted on from the wait loop so the reboot happens outside the USB task.
static volatile bool sEjectReq = false;

// NOTE: the CoreS3 LCD and the SD card share one SPI bus (SCK G36 / MOSI G37).
// The MSC read/write callbacks run in the TinyUSB task and drive the SD; the
// UI loop must NOT touch the LCD while a transfer is in flight or it corrupts
// the bus.  So: draw the banner ONCE before the wait loop, then leave the
// screen alone until we're rebooting.
static int32_t mscRead(uint32_t lba, uint32_t /*offset*/, void* buffer, uint32_t bufsize) {
    return sSd.card()->readSectors(lba, (uint8_t*)buffer, bufsize / 512)
               ? (int32_t)bufsize : -1;
}

static int32_t mscWrite(uint32_t lba, uint32_t /*offset*/, uint8_t* buffer, uint32_t bufsize) {
    return sSd.card()->writeSectors(lba, buffer, bufsize / 512)
               ? (int32_t)bufsize : -1;
}

// macOS eject = START STOP UNIT with start=0, load_eject=1.  Flag it (return
// success so the host completes the eject cleanly) and let the wait loop reboot
// us out of USB-storage mode.
static bool mscOnStartStop(uint8_t /*power*/, bool start, bool eject) {
    Serial.printf("[USB] StartStop start=%d eject=%d\n", start, eject);
    if (eject && !start) {
        // Report the medium as removed so the host finalises the eject and does
        // NOT re-probe + remount.  Without this macOS sees media still present
        // on its next Test Unit Ready and remounts, then the reboot yanks a
        // still-mounted disk ("Disk not ejected properly").
        sMsc.mediaPresent(false);
        sEjectReq = true;
    }
    return true;
}

static void banner(const char* line1, const char* line2) {
    lcd.fillScreen(COL_BG);
    lcd.fillRect(0, 0, 240, 40, COL_HDR);
    lcd.setTextColor(TFT_WHITE, COL_HDR);
    lcd.setTextSize(2);
    lcd.setCursor(8, 12);
    lcd.print("USB Storage");
    lcd.setTextColor(TFT_WHITE, COL_BG);
    lcd.setTextSize(2);
    if (line1) { lcd.setCursor(10, 100); lcd.print(line1); }
    if (line2) { lcd.setCursor(10, 134); lcd.print(line2); }
    lcd.setTextSize(1);
    lcd.setCursor(10, 280); lcd.print("Eject on host to reboot");
    lcd.setCursor(10, 294); lcd.print("(or tap screen to exit).");
}

// Run USB mass-storage mode.  Never returns — tap the screen (after ejecting on
// the host) to reset the device back to normal operation.
void runUsbMsc() {
    banner("Starting...", nullptr);
    Serial.println("[USB-BOOT] entering USB mass-storage mode");

    SPI.begin(MSC_SD_SCK, MSC_SD_MISO, MSC_SD_MOSI, MSC_SD_CS);
    if (!sSd.begin(SdSpiConfig(MSC_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(25)))) {
        Serial.println("[USB-BOOT] SdFat begin FAILED");
        banner("SD init FAILED", "Reset to retry");
        for (;;) delay(100);
    }
    uint32_t sectors = sSd.card()->sectorCount();
    Serial.printf("[USB-BOOT] SdFat OK: %lu sectors\n", (unsigned long)sectors);

    sMsc.vendorID("MagiTrac");
    sMsc.productID("SD Card");
    sMsc.productRevision("1.0");
    sMsc.onRead(mscRead);
    sMsc.onWrite(mscWrite);
    sMsc.onStartStop(mscOnStartStop);
    sMsc.mediaPresent(true);
    sMsc.isWritable(true);
    sMsc.begin(sectors, 512);

    // Give the device a stable identity so macOS recognises it on the fast
    // path instead of re-probing an "unknown" device on each connect.
    USB.manufacturerName("MagiTrac");
    USB.productName("MagiTrac SD");
    USB.serialNumber("MAGITRAC-SD-0001");
    USB.begin();
    Serial.println("[USB-BOOT] MSC ready");

    char cap[24];
    snprintf(cap, sizeof(cap), "%lu MB card",
             (unsigned long)((uint64_t)sectors / 2048));
    banner("Mounted on host", cap);

    // Exit USB-storage mode on either of:
    //   • the host ejecting the disk (preferred — clean unmount), or
    //   • a screen tap (fallback — wait for the boot-held touch to release first).
    // Plain ESP.restart() ONLY.  usb_persist_restart(RESTART_NO_PERSIST) was tried
    // and is worse: it does NOT bring the Serial-JTAG console back on first boot,
    // and it leaves the SD/SPI un-reinitialisable (server boots with no files
    // until a power cycle).  Cost of ESP.restart(): serial returns on the *second*
    // boot.  Reliable SD + reboot beats clean serial.
    bool released = false;
    for (;;) {
        if (sEjectReq) {
            // Do NOT draw the LCD here: on eject the TinyUSB task may still be
            // hitting the SD on the shared SPI bus, and a concurrent LCD write
            // hangs the bus — wedging this loop before it can reboot.  Just go.
            delay(800);          // let macOS finalise the unmount before USB drops
            ESP.restart();
        }

        // Do NOT draw to the LCD here — it shares the SPI bus with the SD card,
        // which the TinyUSB task is actively reading/writing.  M5.update() only
        // touches I²C (touch/buttons), so it's safe.
        M5.update();
        bool touched = M5.Touch.getDetail().isPressed() ||
                       M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed();
        if (!touched) released = true;
        if (released && touched) {
            banner("Restarting...", nullptr);
            delay(400);
            ESP.restart();
        }
        delay(10);
    }
}

#else  // !CONFIG_TINYUSB_MSC_ENABLED — USB Mode not set to TinyUSB

void runUsbMsc() {
    lcd.fillScreen(COL_BG);
    lcd.setTextColor(TFT_WHITE, COL_BG);
    lcd.setTextSize(1);
    lcd.setCursor(10, 100); lcd.print("MSC unavailable.");
    lcd.setCursor(10, 120); lcd.print("Set Tools -> USB Mode");
    lcd.setCursor(10, 134); lcd.print("to USB-OTG (TinyUSB).");
    for (;;) delay(1000);
}

#endif
