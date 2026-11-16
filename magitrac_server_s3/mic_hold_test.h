#pragma once

// Bench diagnostic (only reachable with MIC_HOLD_TEST defined — see the .ino):
// hold the screen to record the microphone into PSRAM, let go to hear it back
// through the speaker / line out.  For judging what the vocoder is being fed:
// it uses the codec exactly as the live firmware sets it up (same input, same
// mic gain, 32 kHz), with the WiFi radio already up, and records the LEFT
// channel — the one the mic is wired to and the vocoder reads.
//
// After each take it shows the level figures (peak, RMS, DC offset, clipped
// samples) on the screen and prints them to Serial.
//
// Call from setup() after audioCodecInit() and the radio bring-up, before any
// task that uses the codec has been started.  This function NEVER returns:
// the device is a mic tester until normal firmware is flashed again.
void micHoldLoop();
