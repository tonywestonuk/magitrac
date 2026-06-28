// chord_detect.cpp — see chord_detect.h.

#include "chord_detect.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// Spectral peaks below this fraction of the frame's loudest bin are ignored —
// keeps broadband noise and the skirts of strong partials out of the chroma.
static const float PEAK_REL_THRESH = 0.10f;
// A peak must also stand this many times above the band's mean magnitude to
// count.  This is the real noise-floor reject: with no tonal content every bin
// sits near the mean, so nothing clears the bar and the chroma stays empty.
static const float PEAK_SNR        = 2.5f;
// Absolute loudness gate: below this peak magnitude we report silence rather
// than chasing a chord in the noise floor.  Tune against the real rig.
static const float SILENCE_GATE    = 0.02f;
// A template tone must carry at least this share of the peak chroma to count
// as "present".  Stops a barely-there 7th from upgrading a triad to a 7th
// chord, and a missing 3rd from being read as a power chord by accident.
static const float TONE_PRESENT    = 0.20f;
// A match is only accepted if at least this share of the chroma energy sits on
// the chord's own tones.  Flat (noisy) chroma spreads energy across all 12
// pitch classes, so its best chord only ever explains ~3/12 of the energy and
// is rejected; a real triad concentrates most energy on its three tones.
static const float CONF_MIN        = 0.55f;
// Only fold energy in this band into the chroma.  Low enough for guitar/bass
// roots, high enough to catch the harmonics that disambiguate the quality.
static const float CHROMA_FMIN_HZ  = 70.0f;
static const float CHROMA_FMAX_HZ  = 2000.0f;

// Chord templates as interval lists (semitones above the root).  Ordered
// simplest-first so a tie resolves toward the plainer chord.
struct Template { ChordQuality q; uint8_t n; uint8_t iv[4]; };
static const Template TEMPLATES[] = {
    { CH_MAJ,  3, {0, 4, 7, 0} },
    { CH_MIN,  3, {0, 3, 7, 0} },
    { CH_SUS4, 3, {0, 5, 7, 0} },
    { CH_SUS2, 3, {0, 2, 7, 0} },
    { CH_DIM,  3, {0, 3, 6, 0} },
    { CH_AUG,  3, {0, 4, 8, 0} },
    { CH_DOM7, 4, {0, 4, 7, 10} },
    { CH_MIN7, 4, {0, 3, 7, 10} },
    { CH_MAJ7, 4, {0, 4, 7, 11} },
    { CH_5,    2, {0, 7, 0, 0} },
};
static const int NUM_TEMPLATES = sizeof(TEMPLATES) / sizeof(TEMPLATES[0]);

static const char* ROOT_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

const char* chordRootName(int8_t pc) {
    if (pc < 0 || pc > 11) return "?";
    return ROOT_NAMES[pc];
}

const char* chordQualitySuffix(ChordQuality q) {
    switch (q) {
        case CH_MAJ:  return "";
        case CH_MIN:  return "m";
        case CH_DOM7: return "7";
        case CH_MIN7: return "m7";
        case CH_MAJ7: return "maj7";
        case CH_DIM:  return "dim";
        case CH_AUG:  return "aug";
        case CH_SUS4: return "sus4";
        case CH_SUS2: return "sus2";
        case CH_5:    return "5";
        default:      return "";
    }
}

void chordFullName(const ChordResult& r, char* out, int outLen) {
    if (!r.valid || r.root < 0) {
        strncpy(out, "--", outLen - 1);
        out[outLen - 1] = '\0';
        return;
    }
    snprintf(out, outLen, "%s%s",
             chordRootName(r.root), chordQualitySuffix(r.quality));
}

uint16_t chordToneMask(const ChordResult& r) {
    if (!r.valid || r.root < 0) return 0;
    for (int t = 0; t < NUM_TEMPLATES; ++t) {
        if (TEMPLATES[t].q != r.quality) continue;
        uint16_t mask = 0;
        for (int j = 0; j < TEMPLATES[t].n; ++j)
            mask |= (uint16_t)(1u << ((r.root + TEMPLATES[t].iv[j]) % 12));
        return mask;
    }
    return 0;
}

ChordResult chordAnalyze(const float* mag, int magLen,
                         float sampleRateHz, int fftSize) {
    ChordResult r;
    memset(&r, 0, sizeof(r));
    r.valid = false;
    r.root  = -1;

    const float binHz = sampleRateHz / (float)fftSize;
    int k0 = (int)(CHROMA_FMIN_HZ / binHz);
    int k1 = (int)(CHROMA_FMAX_HZ / binHz);
    if (k0 < 2)            k0 = 2;
    if (k1 > magLen - 2)   k1 = magLen - 2;
    if (k1 <= k0) return r;

    // Peak magnitude (for the silence gate + relative threshold) and the band
    // mean (for the noise-floor SNR test).
    float maxMag = 0.0f, sumMag = 0.0f;
    for (int k = k0; k <= k1; ++k) {
        float m = mag[k];
        if (m > maxMag) maxMag = m;
        sumMag += m;
    }
    if (maxMag < SILENCE_GATE) return r;
    float meanMag = sumMag / (float)(k1 - k0 + 1);

    r.level = maxMag > 1.0f ? 1.0f : maxMag;     // rough loudness gauge

    // Fold spectral peaks into the chroma.  Each local maximum that clears both
    // the relative threshold and the noise-floor SNR contributes its
    // (parabolically refined) frequency's pitch class.  Refining the bin
    // frequency matters most at the low end, where one 7.8 Hz bin can straddle
    // a semitone boundary.
    const float peakThresh = maxMag * PEAK_REL_THRESH;
    const float snrThresh  = meanMag * PEAK_SNR;
    for (int k = k0; k <= k1; ++k) {
        float m = mag[k];
        if (m < peakThresh)                 continue;
        if (m < snrThresh)                  continue;   // not above noise floor
        if (m < mag[k - 1] || m < mag[k + 1]) continue;   // not a local max

        float denom = mag[k - 1] - 2.0f * m + mag[k + 1];
        float delta = (denom != 0.0f)
                        ? 0.5f * (mag[k - 1] - mag[k + 1]) / denom
                        : 0.0f;
        if (delta < -0.5f) delta = -0.5f;
        if (delta >  0.5f) delta =  0.5f;
        float freq = (k + delta) * binHz;
        if (freq <= 0.0f) continue;

        float midi = 69.0f + 12.0f * log2f(freq / 440.0f);
        int   pc   = ((int)lroundf(midi)) % 12;
        if (pc < 0) pc += 12;
        r.chroma[pc] += m;
    }

    // Normalise chroma to peak = 1 for both matching and display.
    float chMax = 0.0f;
    for (int i = 0; i < 12; ++i) if (r.chroma[i] > chMax) chMax = r.chroma[i];
    if (chMax <= 0.0f) return r;
    float chSum = 0.0f;
    for (int i = 0; i < 12; ++i) { r.chroma[i] /= chMax; chSum += r.chroma[i]; }
    if (chSum <= 0.0f) return r;

    // Score every root × template.  present = chroma energy on chord tones,
    // absent = energy on the other pitch classes; (present - absent) rewards
    // tight matches and punishes stray energy.  A template only qualifies if
    // each of its tones clears TONE_PRESENT, so we never invent a tone that
    // the spectrum doesn't actually support.
    float        bestScore   = -1e9f;
    int          bestRoot    = -1;
    ChordQuality bestQuality = CH_NONE;
    float        bestPresent = 0.0f;
    for (int root = 0; root < 12; ++root) {
        for (int t = 0; t < NUM_TEMPLATES; ++t) {
            const Template& tmpl = TEMPLATES[t];
            float present = 0.0f;
            bool  qualifies = true;
            for (int j = 0; j < tmpl.n; ++j) {
                int pc = (root + tmpl.iv[j]) % 12;
                float e = r.chroma[pc];
                present += e;
                if (e < TONE_PRESENT) qualifies = false;
            }
            if (!qualifies) continue;
            float absent = chSum - present;
            float score  = present - absent;
            if (score > bestScore) {
                bestScore   = score;
                bestRoot    = root;
                bestQuality = tmpl.q;
                bestPresent = present;
            }
        }
    }

    // Accept the best match only if it's genuinely chord-shaped: a positive
    // score (more energy on chord tones than off them) AND enough of the total
    // energy concentrated on those tones.  Flat noise fails both.
    if (bestRoot >= 0) {
        float conf = bestPresent / chSum;
        if (bestScore > 0.0f && conf >= CONF_MIN) {
            r.root       = (int8_t)bestRoot;
            r.quality    = bestQuality;
            r.confidence = conf;
            r.valid      = true;
        }
    }
    return r;
}
