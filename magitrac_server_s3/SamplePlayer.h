#pragma once
#include <stdint.h>

// ── SamplePlayer — non-blocking WAV playback via ES8388 codec ─────────────────
//
// CoreS3 + Module Audio v2.2.  Runs a FreeRTOS task on Core 0 so the MIDI
// sequencer (Core 1) is never blocked.
//
// Both play() and stop() are fire-and-forget.  The SD stream is single-voice:
// a new samplePlayerPlay() interrupts the one playing.  Samples preloaded into
// PSRAM play as up to 4 polyphonic RAM voices mixed on top (see below).
//
// WAVs are 16-bit PCM, mono or stereo, 8–48 kHz; linear-interp resampled to
// the codec's fixed 32 kHz (mono is duplicated to L+R).
//
// Codec is shared full-duplex with mic_spectrum — no mutex needed, both
// directions stream concurrently on the same I²S peripheral.

struct Song;   // TrackerData.h — only needed by samplePreloadSync

void samplePlayerInit();                    // call once in setup() (after audioCodecInit)
void samplePlayerPlay(const char* path, float pitchSemitones = 0,   // 0 = native pitch (tracker C-4); ±semitones transposes (fractional = cents-accurate root tuning)
                      uint8_t volume = 127,                         // squared taper (GM CC7-ish); 127 = unity, up to 255 boosts (and can clip)
                      uint32_t startFrame = 0,                      // trim: first frame to play
                      uint32_t endFrame = 0,                        // trim: stop here (0 = end of file)
                      bool loop = false);                           // wrap start..end until stopped
void samplePlayerStop();                    // signal stop (stream + all RAM voices), returns immediately
void samplePlayerStopStream();              // stop only the SD stream; RAM voices keep ringing
bool samplePlayerIsPlaying();

// ── PSRAM sample cache + polyphonic RAM voices ────────────────────────────────
// Samples held in PSRAM play from RAM, polyphonic (8 voices, mixed with each
// other AND with a concurrently streaming sample).  Anything not in the cache
// streams from SD, single-voice.
//
// What is kept in the cache: the loaded song's SFX samples, the sample open in
// the client's editor (samplePreloadSetExtra), and — while the client's
// samples page is open (samplePreloadSetPads) — every sample on the card,
// shortest first.  One sample may take up to 2 MB of audio and the cache 6 MB
// in all; what doesn't fit streams.  Samples nobody wants any more stay until
// their room is needed.
//
// samplePreloadSync does one step of bringing the cache to that state: call
// it from the LOOP task (it reads SD, 32 KB at a time — 8 KB with `gentle`,
// for while a song is playing) until it returns true.  false = call again.
// The Set calls below only record a wish — the caller schedules the sync that
// acts on it.
bool samplePreloadSync(const Song* song, bool gentle = false);
void samplePreloadSetExtra(uint8_t sampleId);   // 0 = none
void samplePreloadSetPads(bool on);
// Empty the cache and forget which samples failed to load — after a restore
// replaced WAVs (new audio under ids already cached).  Done by the following
// samplePreloadSync calls, once the RAM voices are quiet.  Loop task.
void samplePreloadForgetAll();
// Empty the cache right now, to free its PSRAM for something bigger (the ADD
// SAMPLE take buffer).  false = a voice or the stream is still sounding; stop
// them, wait a moment and call again.  Loop task only.  The next
// samplePreloadSync reloads whatever is still wanted.
bool samplePreloadDropAllNow();
// Start a RAM voice (polyphonic, steals the oldest when full).  Returns a
// voice handle, or -1 if the sample isn't cached (caller falls back to
// samplePlayerPlay streaming).  volume as for samplePlayerPlay: 127 = unity,
// more boosts.
int  samplePlayRam(uint8_t sampleId, float pitchSemitones = 0, uint8_t volume = 127);
// A RAM voice on a caller's mono PCM buffer (ADD SAMPLE plays its take from
// the keys this way, before it is a sample).  The buffer must outlive the
// voice: stop it and wait for sampleRamVoicesActive() to go false before
// freeing.
int  samplePlayBuffer(const int16_t* data, uint32_t frames, uint32_t rate,
                      float pitchSemitones = 0, uint8_t volume = 127);
void sampleStopVoice(int voice);            // release one RAM voice (short fade)
bool sampleRamVoicesActive();

// AUTO TUNE's pitch pass: one FFT per call on the sample asked for.
// mayStart = false keeps it from starting one (ADD SAMPLE is using the pitch
// detector).  Loop task.  True when a result was just stored.
bool samplePitchTick(bool mayStart);

// AUTO TUNE (sample editor): find sampleId's pitch — from the cache, waiting
// a few seconds for it to arrive there.  Any task.  The answer comes back from
// sampleAutoTuneTake (loop task) once: result is a SampleTuneResult.
void sampleAutoTuneRequest(uint8_t sampleId);
bool sampleAutoTuneTake(uint8_t& sampleId, uint8_t& result, float& hz);
