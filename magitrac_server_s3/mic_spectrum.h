// mic_spectrum.h — mic capture + FFT + MSG_SPECTRUM/MSG_BEAT broadcast.
//
// CoreS3 + Module Audio v2.2 — the on-board MEMS mic feeds the ES8388
// codec's LINPUT2/RINPUT2 ADC pair, captured via audio_codec.
//
// Lifecycle: spectrumInit() spawns the task; spectrumSetActive(true) turns
// processing on.  Gated at the SELECT_EFFECT layer in pixelpost_send.ino —
// effect 13 (SoundSpectrum) needs the mic; other effects don't.
//
// Codec is shared full-duplex with SamplePlayer — no I²S0 mutex any more.
#pragma once
#include <stdint.h>

void spectrumInit();
void spectrumSetActive(bool active);
bool spectrumIsActive();
