// mic_spectrum.h — PDM mic capture + FFT + MSG_SPECTRUM broadcast.
//
// Replaces the M5Paper controller's audio role.  Mic on Grove Port A
// (G22 = PDM_CLK, G21 = PDM_DATA), I2S_NUM_1 PDM RX so it doesn't fight
// SamplePlayer's I2S_NUM_0 DAC output.
//
// Lifecycle: spectrumInit() boots the task suspended; spectrumSetActive(true)
// turns the mic on and starts streaming MSG_SPECTRUM via the existing
// pixel_post send queue.  Gating is done at the SELECT_EFFECT layer in
// pixelpost_send.ino — when effect 13 (SoundSpectrum) is selected, mic on;
// any other effect, mic off.
#pragma once

void spectrumInit();
void spectrumSetActive(bool active);
