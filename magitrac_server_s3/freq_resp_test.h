#pragma once

// Bench diagnostic (only reachable with FREQ_RESP_TEST defined — see the .ino):
// plays a fixed test sequence out of the codec, over and over, for measuring
// the output's frequency response with an audio interface on a computer.
// The codec is used exactly as the live firmware sets it up (32 kHz, same
// output path and volume), with the WiFi radio up.
//
// One cycle (~26 s), the same on both channels:
//   1.0 s silence
//   0.5 s 1 kHz          sync marker (the analysis keys off its start)
//   0.5 s silence
//   58 tones, 20 Hz × 2^(k/6) (k = 0..57, up to ~14.5 kHz): 0.3 s each with
//         5 ms fades, then 0.1 s silence
//   1.0 s silence        noise floor
// Every tone is a sine at -12 dBFS.  The analysis (tools/freq_resp/) measures
// each tone's level relative to the 1 kHz one.
//
// With FREQ_RESP_NO_WIFI also defined the radio is switched fully off first,
// to A/B how much of the noise floor is RF pickup.
//
// Call from setup() after audioCodecInit() and the radio bring-up, before any
// task that uses the codec has been started.  NEVER returns.
void freqRespLoop();
