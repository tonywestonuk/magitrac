// Minimal CoreS3 USB Mass Storage baseline — ESP32 core USBMSC + SdFat (SPI).
//
// The Adafruit_TinyUSB version (msc_sdfat) CRASHES on the CoreS3: its USB stack
// collides with the ESP32 core's USB ownership (StoreProhibited in USB init,
// before SD is even touched).  So this uses the ESP32 core's own USBMSC — which
// enumerates fine on this board — with SdFat's robust SPI driver for the card.
//
// Board settings:  Tools → USB Mode = "USB-OTG (TinyUSB)".
//
// Boots straight into mass-storage (no buttons/flags) so it's a clean baseline.
// Serial prints the card size BEFORE USB.begin() takes over the port.

#include <SPI.h>
#include "SdFat.h"
#include "USB.h"
#include "USBMSC.h"

// CoreS3 SD on SPI: SCK=G36, MISO=G35, MOSI=G37, CS=G4
#define SD_SCK   36
#define SD_MISO  35
#define SD_MOSI  37
#define SD_CS    4

SdFat  sd;
USBMSC msc;

static int32_t onRead(uint32_t lba, uint32_t /*offset*/, void* buffer, uint32_t bufsize) {
  return sd.card()->readSectors(lba, (uint8_t*)buffer, bufsize / 512) ? (int32_t)bufsize : -1;
}

static int32_t onWrite(uint32_t lba, uint32_t /*offset*/, uint8_t* buffer, uint32_t bufsize) {
  return sd.card()->writeSectors(lba, buffer, bufsize / 512) ? (int32_t)bufsize : -1;
}

static bool onStartStop(uint8_t /*power*/, bool /*start*/, bool /*eject*/) {
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nCoreS3 USBMSC + SdFat baseline");

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(25)))) {
    Serial.println("SdFat begin FAILED — lower SD_SCK_MHZ or check wiring");
    return;
  }

  uint32_t sectors = sd.card()->sectorCount();
  Serial.printf("Card OK: %lu sectors, %lu MB\n",
                (unsigned long)sectors, (unsigned long)((uint64_t)sectors / 2048));

  msc.vendorID("MagiTrac");
  msc.productID("SD Card");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(true);
  msc.isWritable(true);
  msc.begin(sectors, 512);

  USB.begin();   // takes over the USB port from here
  Serial.println("MSC started — check the host");
}

void loop() {
  delay(1000);
}
