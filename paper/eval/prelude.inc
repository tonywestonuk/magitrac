// Host stand-ins for the hardware the sequencer touches (same as the head of
// magitrac_server_s3/test_seq.cpp).

// ── Mock runtime (must come before any Arduino header) ────────────────────────
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <assert.h>

static uint32_t _mockMs = 0;
uint32_t millis() { return _mockMs; }

#include "test_mocks/Arduino.h"
HardwareSerial Serial;
HardwareSerial midi;

// Sketch-side functions that midi_player.cpp references but the test doesn't
// exercise — stub them out so the link succeeds.
void debugPrintf(const char* /*fmt*/, ...) {}
void samplePlayerPlay(const char* /*path*/, float /*pitch*/, uint8_t /*vol*/,
                      uint32_t /*startFrame*/, uint32_t /*endFrame*/, bool /*loop*/) {}
void samplePlayerStop() {}
void samplePlayerStopStream() {}
int  samplePlayRam(uint8_t /*id*/, float /*pitch*/, uint8_t /*vol*/) { return -1; }
void sampleStopVoice(int /*voice*/) {}
const char* sampleManifestNameFor(uint8_t /*id*/) { return nullptr; }
#include "SampleManifest.h"   // SampleTrim type for the stub below
const SampleTrim* sampleTrimFor(uint8_t /*id*/) { return nullptr; }
void pixelpostSetTouchpad(uint8_t, uint8_t, bool) {}
void pixelpostSetSlider(uint8_t) {}
void pixelpostSetEffect(uint8_t) {}
bool pixelpostManualActive() { return false; }

// Organ synth stubs (drawbar_organ.cpp is hardware-bound).  Only the active
// flag is modelled — it starts "inactive" and follows organSetActive(), so the
// sequencer's grab / release of the organ can be observed.
static bool _mockOrganActive   = false;
static int  _mockOrganOffCalls = 0;     // active → inactive transitions
bool organActive() { return _mockOrganActive; }
void organSetActive(bool on) {
    if (!on && _mockOrganActive) _mockOrganOffCalls++;
    _mockOrganActive = on;
}
void organNoteOn(uint8_t, uint8_t) {}
void organNoteOff(uint8_t) {}
void organSetSustain(bool) {}
int  organGetType() { return 0; }
void organSetType(int) {}
int  organGetProcSound() { return 0; }
void organSetProcSound(int) {}
int  organGetDrawbar(int) { return 0; }
void organSetDrawbar(int, int) {}
int  organGetParam(int) { return 0; }
void organSetParam(int, int) {}
int  organGetVibChorus() { return 0; }
void organSetVibChorus(int) {}
int  organGetLeslie() { return 0; }
void organSetLeslie(int) {}
int  organGetDrive() { return 0; }
void organSetDrive(int) {}
int  organGetReverb() { return 0; }
void organSetReverb(int) {}
int  organGetVocoder() { return 0; }
void organSetVocoder(int) {}

// ── External state required by midi_player.cpp ───────────────────────────────
#include "TrackerData.h"

static uint8_t _srvBufStore[sizeof(SongFileHeader) + sizeof(Song)];
uint8_t*  srvActiveBuf    = _srvBufStore;   // firmware: PSRAM pointer
bool      srvHasActive    = true;

// ── Pull in the implementation under test ────────────────────────────────────
// The server has no TrackerData.cpp — midi_player.cpp only needs the .h types.
#include "NoteGrid.cpp"
#include MIDI_PLAYER_CPP
