#include "audio_codec.h"
#include <Arduino.h>
#include <Wire.h>
#include <M5Module_Audio.h>

static M5ModuleAudio sDev;
static bool          sReady = false;

bool audioCodecInit() {
    if (sReady) return true;

    // Pass Wire1 (I²C_NUM_1), NOT Wire (I²C_NUM_0).  M5GFX drives the
    // internal bus that the codec, AXP, AW9523 and touch all live on via
    // I²C_NUM_1; using Arduino's Wire (I²C_NUM_0) makes a second I²C
    // peripheral fight M5GFX for the same physical pins (G12/G11), which
    // garbages the touch reads.  Both drivers on the same peripheral
    // (I²C_NUM_1) is the lesser evil — single GPIO-matrix binding.
    // M5.begin() must have run first.
    if (!sDev.begin(Wire1)) {
        Serial.println("[CODEC] M5ModuleAudio.begin FAILED — module not detected");
        return false;
    }

    sDev.setHPMode(AUDIO_HPMODE_NATIONAL);
    sDev.setMICStatus(AUDIO_MIC_OPEN);
    sDev.setMicInputLine(ADC_INPUT_LINPUT2_RINPUT2);   // on-board MEMS mic pair
    sDev.setMicGain(MIC_GAIN_24DB);
    sDev.setMicAdcVolume(96);
    sDev.setSpeakerVolume(100);
    sDev.setSpeakerOutput(DAC_OUTPUT_ALL);   // enable OUT1 + OUT2 (speaker + headphone)
    sDev.setBitsSample(ES_MODULE_ADC_DAC, BIT_LENGTH_16BITS);
    sDev.setSampleRate(SAMPLE_RATE_32K);

    sReady = true;
    Serial.println("[CODEC] ES8388 ready @ 32 kHz / 16-bit stereo");
    return true;
}

bool audioCodecReady() { return sReady; }

bool audioCodecPlay(const uint8_t* buf, size_t bytes) {
    if (!sReady) return false;
    return sDev.play(buf, (int)bytes);
}

bool audioCodecRecord(uint8_t* buf, size_t bytes) {
    if (!sReady) return false;
    return sDev.record(buf, (int)bytes);
}
