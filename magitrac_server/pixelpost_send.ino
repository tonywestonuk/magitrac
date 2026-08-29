// pixelpost_send.ino — DISABLED STUB (2026-07-24 rescue build).
//
// PixelPost support is deliberately compiled out of this fall-back server.
// The old per-event wire format (PP_MSG_SELECT_EFFECT / MOVE / SLIDER /
// TAPPED / SPECTRUM / BEAT / MAGITRAC) was replaced by the single 16-byte
// PPOST state packet on 2026-06-08, so none of those symbols exist in
// pixelpost_proto.h any more and this file no longer built.
//
// Rather than port the PPOST broadcaster back here, the lights are simply
// not driven from this server.  Two consequences, both wanted:
//
//   • Songs may still contain PIXELPOST_CHANNEL (18) columns.  The
//     sequencer already routes those away from UART MIDI and into these
//     functions, so with no-op bodies such columns are silently inert —
//     they never leak onto MIDI channel 18.
//   • The PDM mic existed only to feed the pixelpost SoundSpectrum effect.
//     With it gone, spectrumInit() is never called, so the mic never claims
//     I²S0 and SamplePlayer's mic-stop/restart dance around DAC output
//     becomes dead code.  One less thing to fight the sample player.
//
// To bring the lights back, port magitrac_server_s3/pixelpost_send.ino
// (it is hardware-agnostic — esp_now + MagiCommsEspNow only) and restore
// the spectrumInit() call in setup().

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

void pixelpostInit() {
    Serial.println("[PP-TX] disabled in this build");
}

void pixelpostEnqueue(const uint8_t*, size_t) {}
void pixelpostSendPairingBeacon() {}
void pixelpostSendSelectEffect(uint8_t) {}
void pixelpostSendTapped() {}
void pixelpostSendSlider(uint8_t, bool) {}
void pixelpostSendMove(uint8_t, uint8_t, bool) {}
