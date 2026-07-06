#pragma once

// Diagnostic — records 2 s of mic audio into PSRAM, plays it back through
// the codec, loops forever.  Verifies the full-duplex codec path end to
// end without involving the FFT, AGC, or PixelPost.  Call from setup()
// after audioCodecInit(); this function NEVER returns.
void micEchoLoop();
