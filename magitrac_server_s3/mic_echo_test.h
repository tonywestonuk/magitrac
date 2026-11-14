#pragma once

// Parked bench diagnostic (only reachable with MIC_ECHO_TEST defined in the
// .ino) — records 0.5 s of mic audio into internal RAM, prints per-channel
// peak/RMS and a CSV dump of the first frames to Serial, then repeats every
// 5 s forever.  Capture only: nothing is played back.  Verifies the codec
// record path without involving the FFT, AGC, or PixelPost.  Call from
// setup() after audioCodecInit(); this function NEVER returns.
void micEchoLoop();
