#pragma once
#include <stdint.h>

// ── SamplePlayer — non-blocking WAV playback via ES8388 codec ─────────────────
//
// CoreS3 + Module Audio v2.2.  Runs a FreeRTOS task on Core 0 so the MIDI
// sequencer (Core 1) is never blocked.
//
// Both play() and stop() are fire-and-forget: triggering a new sample while
// one is playing interrupts the first (single-voice polyphony).
//
// WAVs are mono 16-bit PCM at any sample rate; linear-interp resampled to
// the codec's 32 kHz fixed rate, mono → stereo (same value on L+R).
//
// Codec is shared full-duplex with mic_spectrum — no mutex needed, both
// directions stream concurrently on the same I²S peripheral.

struct Song;   // TrackerData.h — only needed by samplePreloadSync

void samplePlayerInit();                    // call once in setup() (after audioCodecInit)
void samplePlayerPlay(const char* path, float pitchSemitones = 0,   // 0 = native pitch (tracker C-4); ±semitones transposes (fractional = cents-accurate root tuning)
                      uint8_t volume = 127,                         // 0..127, squared taper (GM CC7-ish); 127 = unity
                      uint32_t startFrame = 0,                      // trim: first frame to play
                      uint32_t endFrame = 0,                        // trim: stop here (0 = end of file)
                      bool loop = false);                           // wrap start..end until stopped
void samplePlayerStop();                    // signal stop (stream + all RAM voices), returns immediately
void samplePlayerStopStream();              // stop only the SD stream; RAM voices keep ringing
bool samplePlayerIsPlaying();

// ── PSRAM sample cache + polyphonic RAM voices ────────────────────────────────
// SFX samples under 200 KB are preloaded into PSRAM when the song loads and
// play from RAM with polyphony (mixed with each other AND with a concurrently
// streaming big sample).  Larger files keep streaming from SD, single-voice.
//
// samplePreloadSync: diff the cache against the song's SFX columns — evict
// what's no longer referenced, load what's new.  Does SD reads: call from the
// LOOP task only (commandsTick), with the sequencer stopped and voices quiet
// (returns false = busy, call again next tick).
bool samplePreloadSync(const Song* song);
// Start a RAM voice (polyphonic, steals the oldest when full).  Returns a
// voice handle, or -1 if the sample isn't cached (caller falls back to
// samplePlayerPlay streaming).
int  samplePlayRam(uint8_t sampleId, float pitchSemitones = 0, uint8_t volume = 127);
void sampleStopVoice(int voice);            // release one RAM voice (short fade)
bool sampleRamVoicesActive();
