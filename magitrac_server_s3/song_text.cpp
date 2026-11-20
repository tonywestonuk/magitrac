#include "song_text.h"
#include "NoteGrid.h"
#include "OrganSynth.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char* const PC_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

void textPrintf(TextBuf& out, const char* fmt, ...) {
    if (out.overflow) return;
    size_t room = out.cap - out.len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out.p + out.len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= room) {
        out.overflow = true;
        if (out.cap) out.p[out.len] = '\0';
        return;
    }
    out.len += (size_t)n;
}

// ── Writing ──────────────────────────────────────────────────────────────────

static void nameOf(char* dst, size_t n, const char* src, size_t srcMax) {
    size_t j = 0;
    for (size_t i = 0; i < srcMax && src[i] && j + 1 < n; i++)
        dst[j++] = (src[i] == '"' || (uint8_t)src[i] < 32) ? '\'' : src[i];
    dst[j] = '\0';
}

static const char* chanText(uint8_t ch, char* buf) {
    if (ch == 0)                 return "OFF";
    if (ch == SFX_CHANNEL)       return "SFX";
    if (ch == ORGAN_CHANNEL)     return "ORGAN";
    if (ch == PIXELPOST_CHANNEL) return "LIGHTS";
    if (ch >= 1 && ch <= 16) { snprintf(buf, 4, "%u", (unsigned)ch); return buf; }
    return "OFF";
}

// The organ voices by OrganPatch.voice (8 = SYNTH; 7 reads as VOCODER).
static const char* const ORGAN_VOICE_TEXT[9] =
    { "DRAWBAR", "TONEWHEEL", "CLAUDE", "NEBULA", "REED", "CHOIR", "VOCODER", "VOCODER", "SYNTH" };
static const char* const ORGAN_VIB_TEXT[7]    = { "OFF", "V1", "V2", "V3", "C1", "C2", "C3" };
static const char* const ORGAN_LESLIE_TEXT[3] = { "STOP", "SLOW", "FAST" };
static const char* const ORGAN_REVERB_TEXT[3] = { "OFF", "ROOM", "HALL" };

static void writeOrgan(const OrganPatch& op, TextBuf& out) {
    if (!(op.flags & 1)) return;
    uint8_t voice = op.voice > ORGAN_VOICE_SYNTH ? ORGAN_VOICE_SYNTH : op.voice;
    textPrintf(out, "ORGAN voice=%s vib=%s leslie=%s drive=%u reverb=%s",
               ORGAN_VOICE_TEXT[voice], ORGAN_VIB_TEXT[op.vibChorus % 7],
               ORGAN_LESLIE_TEXT[op.leslie % 3], (unsigned)(op.drive ? 1 : 0),
               ORGAN_REVERB_TEXT[op.reverb % 3]);
    if (organVoiceIsSynth(voice)) {
        textPrintf(out, "\nSYNTH");
        for (int i = 0; i < ORGAN_SYNTH_PARAMS; i++) {
            int v = organSynthClamp(i, organSynthParam(op, i));
            if (i == SYN_OSC1 || i == SYN_OSC2)
                textPrintf(out, " %s=%s", ORGAN_SYNTH_KEYS[i], SYN_WAVE_NAMES[v]);
            else if (i == SYN_PITCH)
                textPrintf(out, " %s=%d", ORGAN_SYNTH_KEYS[i], v - 12);
            else
                textPrintf(out, " %s=%d", ORGAN_SYNTH_KEYS[i], v);
        }
    } else {
        char bars[ORGAN_PATCH_BARS + 1];
        for (int i = 0; i < ORGAN_PATCH_BARS; i++) bars[i] = (char)('0' + (op.bars[i] > 8 ? 8 : op.bars[i]));
        bars[ORGAN_PATCH_BARS] = '\0';
        textPrintf(out, " bars=%s sliders=", bars);
        for (int i = 0; i < ORGAN_PATCH_SLIDERS; i++)
            textPrintf(out, i ? ",%u" : "%u", (unsigned)(op.sliders[i] > 8 ? 8 : op.sliders[i]));
    }
    textPrintf(out, "\n");
}

static void noteText(uint8_t note, char* buf) {
    if (note == NOTE_OFF) { strcpy(buf, "OFF"); return; }
    if (note == NOTE_ANY) { strcpy(buf, "ANY"); return; }
    int n = note - 1;
    snprintf(buf, 5, "%s%s%d", PC_NAMES[n % 12], (n % 12 == 1 || n % 12 == 3 ||
             n % 12 == 6 || n % 12 == 8 || n % 12 == 10) ? "" : "-", n / 12);
}

static void fxText(uint8_t col, const Note& n, char* buf) {
    if (n.effect == 0 && n.param == 0) { strcpy(buf, "-"); return; }
    if (col == INPUT_COLUMN && n.param == 0) {
        switch (n.effect) {
            case EFFECT_WAIT: strcpy(buf, "WAIT"); return;
            case EFFECT_WAT1: strcpy(buf, "WAT1"); return;
            case EFFECT_WAT2: strcpy(buf, "WAT2"); return;
            case EFFECT_SYNC: strcpy(buf, "SYNC"); return;
            case EFFECT_AVRG: strcpy(buf, "AVRG"); return;
            case EFFECT_PASA: strcpy(buf, "PASA"); return;
        }
    }
    if (col != INPUT_COLUMN && n.effect == EFFECT_PAN) {
        snprintf(buf, 12, "PAN:%u", (unsigned)(n.param > 127 ? 127 : n.param)); return;
    }
    if (col != INPUT_COLUMN && n.effect == EFFECT_RTRG) {
        snprintf(buf, 12, "RTRG:%u", (unsigned)retrigHits(n.param)); return;
    }
    snprintf(buf, 12, "FX:%02X%02X", (unsigned)n.effect, (unsigned)n.param);
}

static void endText(uint8_t nav, char* buf) {
    if (nav == NAV_RNT) { strcpy(buf, "RETURN"); return; }
    uint8_t t = nav & NAV_TARGET_MASK;
    switch (nav & NAV_MODE_MASK) {
        case NAV_LOOP: strcpy(buf, "LOOP"); return;
        case NAV_FWD:  snprintf(buf, 12, "NEXT:%u", (unsigned)t); return;
        case NAV_BACK: snprintf(buf, 12, "BACK:%u", (unsigned)t); return;
        default:       snprintf(buf, 12, "GOTO:%u", (unsigned)t); return;
    }
}

struct NoteWriteCtx { TextBuf* out; int block; };

static void writeNote(uint8_t row, uint8_t col, const Note& n, void* ctx) {
    NoteWriteCtx* c = (NoteWriteCtx*)ctx;
    if (n.note == NOTE_EMPTY && n.effect == 0 && n.param == 0) return;
    char nt[6], fx[16], vel[6];
    if (n.note == NOTE_EMPTY) strcpy(nt, "---");
    else noteText(n.note, nt);
    if (n.velocity & 0x80) strcpy(vel, "-");
    else snprintf(vel, sizeof(vel), "%u", (unsigned)n.velocity);
    fxText(col, n, fx);
    textPrintf(*c->out, "N %d %u %u %s %s %s\n", c->block, (unsigned)row, (unsigned)col,
               nt, vel, fx);
}

bool songToText(const Song* s, TextBuf& out) {
    char nm[40], b1[12], b2[12];
    textPrintf(out, "MAGITRAC SONG 1\n");
    nameOf(nm, sizeof(nm), s->name, sizeof(s->name));
    textPrintf(out, "SONG name=\"%s\" bpm=%u minbpm=%u maxbpm=%u speed=%u start=%u\n",
               nm, (unsigned)s->bpm, (unsigned)s->minBPM, (unsigned)s->maxBPM,
               (unsigned)s->speed, (unsigned)s->startPattern);
    for (int c = 1; c < MAX_COLUMNS; c++) {
        const ColumnSettings& cs = s->columns[c];
        if (cs.midiChannel == 0 && !cs.name[0]) continue;
        nameOf(nm, sizeof(nm), cs.name, sizeof(cs.name));
        textPrintf(out, "COL %d ch=%s bank=%u prog=%u vol=%u tr=%d mute=%u tune=%d name=\"%s\"\n",
                   c, chanText(cs.midiChannel, b1), (unsigned)cs.bankMSB,
                   (unsigned)cs.program, (unsigned)cs.volume, (int)cs.transpose,
                   (unsigned)(cs.mute ? 1 : 0), (int)cs.sfxTuneCents, nm);
    }
    writeOrgan(s->organ, out);
    int np = s->numPatterns;
    if (np < 1) np = 1;
    if (np > MAX_PATTERNS) np = MAX_PATTERNS;
    for (int b = 0; b < np; b++) {
        const Pattern& p = s->patterns[b];
        nameOf(nm, sizeof(nm), p.name, sizeof(p.name));
        endText(p.blockEndNav, b2);
        textPrintf(out, "BLOCK %d len=%u name=\"%s\" ref=%s keymode=%s end=%s\n",
                   b, (unsigned)p.length, nm, PC_NAMES[p.referenceNote % 12],
                   p.keyChangeMode ? "TOP" : "SAME", b2);
        for (int k = 0; k < 12; k++) {
            const InputNoteEntry& e = p.inputNotes[k];
            if (e.switchMode == BlockSwitch::STAY && e.transposeAction == TransposeAction::KEEP)
                continue;
            const char* sw = e.switchMode == BlockSwitch::SAME_POS ? "SAME"
                           : e.switchMode == BlockSwitch::TOP      ? "TOP" : "STAY";
            const char* tr = e.transposeAction == TransposeAction::NOTE   ? "NOTE"
                           : e.transposeAction == TransposeAction::CUSTOM ? "CUSTOM" : "KEEP";
            textPrintf(out, "KEY %d %s switch=%s target=%u tr=%s by=%d\n", b, PC_NAMES[k],
                       sw, (unsigned)e.switchTarget, tr, (int)e.transposeValue);
        }
        NoteGrid g(s->notePool, &p.noteHead);
        NoteWriteCtx ctx = { &out, b };
        g.forAll(writeNote, &ctx);
    }
    textPrintf(out, "END\n");
    return !out.overflow;
}

// ── Parsing ──────────────────────────────────────────────────────────────────

static const int TOK_MAX = 16;
static const int TOK_LEN = 48;

struct Line {
    int  n;
    char tok[TOK_MAX][TOK_LEN];
};

// Whitespace-separated tokens; a "quoted part" may contain spaces (quotes kept).
static void tokenise(const char* s, size_t len, Line& L) {
    L.n = 0;
    size_t i = 0;
    while (i < len && L.n < TOK_MAX) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
        if (i >= len) break;
        int  j = 0;
        bool q = false;
        while (i < len && (q || (s[i] != ' ' && s[i] != '\t' && s[i] != '\r'))) {
            if (s[i] == '"') q = !q;
            if (j < TOK_LEN - 1) L.tok[L.n][j++] = s[i];
            i++;
        }
        L.tok[L.n][j] = '\0';
        L.n++;
    }
}

// key=value lookup among tokens [from..n).  Quotes around the value removed.
static const char* kv(const Line& L, int from, const char* key, char* buf, size_t n) {
    size_t kl = strlen(key);
    for (int i = from; i < L.n; i++) {
        if (strncasecmp(L.tok[i], key, kl) == 0 && L.tok[i][kl] == '=') {
            const char* v = L.tok[i] + kl + 1;
            size_t j = 0;
            for (; *v && j + 1 < n; v++) if (*v != '"') buf[j++] = *v;
            buf[j] = '\0';
            return buf;
        }
    }
    return nullptr;
}

static bool toInt(const char* s, long lo, long hi, long& v) {
    if (!s || !*s) return false;
    char* e;
    v = strtol(s, &e, 10);
    return *e == '\0' && v >= lo && v <= hi;
}

static bool kvInt(const Line& L, const char* key, long lo, long hi, long& v) {
    char b[TOK_LEN];
    return toInt(kv(L, 2, key, b, sizeof(b)), lo, hi, v);
}

static int pitchClass(const char*& s) {
    static const int BASE[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A..G
    char c = (char)toupper((unsigned char)*s);
    if (c < 'A' || c > 'G') return -1;
    int pc = BASE[c - 'A'];
    s++;
    if (*s == '#')      { pc++; s++; }
    else if (*s == 'b') { pc--; s++; }
    else if (*s == '-') { s++; }
    return (pc + 12) % 12;
}

// "C-4", "C#4", "Db4", "C4", "OFF", "ANY" → note value; false if not a note.
static bool parseNote(const char* s, uint8_t& out) {
    if (!strcasecmp(s, "OFF")) { out = NOTE_OFF; return true; }
    if (!strcasecmp(s, "ANY")) { out = NOTE_ANY; return true; }
    if (!strcmp(s, "---") || !strcmp(s, "-")) { out = NOTE_EMPTY; return true; }
    const char* p = s;
    int pc = pitchClass(p);
    if (pc < 0 || *p < '0' || *p > '7' || p[1] != '\0') return false;
    int v = (*p - '0') * 12 + pc + 1;
    if (v < 1 || v > NOTE_MAX) return false;
    out = (uint8_t)v;
    return true;
}

static bool parsePc(const char* s, uint8_t& out) {
    const char* p = s;
    int pc = pitchClass(p);
    if (pc < 0 || *p) return false;
    out = (uint8_t)pc;
    return true;
}

static bool parseFx(const char* s, uint8_t col, uint8_t& fx, uint8_t& prm) {
    fx = prm = 0;
    if (!strcmp(s, "-") || !strcmp(s, "----")) return true;
    static const struct { const char* n; uint8_t e; } CUES[] = {
        { "WAIT", EFFECT_WAIT }, { "WAT1", EFFECT_WAT1 }, { "WAT2", EFFECT_WAT2 },
        { "SYNC", EFFECT_SYNC }, { "AVRG", EFFECT_AVRG }, { "PASA", EFFECT_PASA },
        { "PASS", 0 },
    };
    for (auto& c : CUES)
        if (!strcasecmp(s, c.n)) { if (col != INPUT_COLUMN) return false; fx = c.e; return true; }
    long v;
    if (!strncasecmp(s, "PAN:", 4)) {
        if (col == INPUT_COLUMN || !toInt(s + 4, 0, 127, v)) return false;
        fx = EFFECT_PAN; prm = (uint8_t)v; return true;
    }
    if (!strncasecmp(s, "RTRG:", 5)) {
        if (col == INPUT_COLUMN || !toInt(s + 5, 2, 8, v)) return false;
        fx = EFFECT_RTRG; prm = (uint8_t)v; return true;
    }
    if (!strncasecmp(s, "FX:", 3) && strlen(s + 3) == 4) {
        char* e;
        unsigned long h = strtoul(s + 3, &e, 16);
        if (*e) return false;
        fx = (uint8_t)(h >> 8); prm = (uint8_t)h; return true;
    }
    return false;
}

static void copyName(char* dst, size_t n, const char* v) {
    size_t i = 0;
    for (; v && v[i] && i + 1 < n; i++) dst[i] = v[i];
    dst[i] = '\0';
}

static bool validLen(long v) { return v == 16 || v == 24 || v == 32 || v == 48 || v == 64; }

static void fail(SongTextResult& r, int line, const char* msg) {
    if (r.err[0]) return;
    r.line = line;
    if (line) snprintf(r.err, sizeof(r.err), "line %d: %s", line, msg);
    else      snprintf(r.err, sizeof(r.err), "%s", msg);
}

bool songFromText(const char* text, size_t len, const Song* base, Song* dst,
                  SongTextResult& r) {
    memset(&r, 0, sizeof(r));

    // Start from base for the settings the text doesn't carry, then clear
    // everything the text describes.
    memcpy(dst, base, sizeof(Song));
    for (uint16_t i = 0; i < MAX_SONG_NOTES - 1; i++) dst->notePool[i].next = i + 1;
    dst->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    dst->noteFreeHead = 0;
    memset(dst->patterns, 0, sizeof(dst->patterns));
    for (int i = 0; i < MAX_PATTERNS; i++) {
        dst->patterns[i].noteHead = NOTE_NULL;
        dst->patterns[i].length   = 16;
    }
    memset(dst->columns, 0, sizeof(dst->columns));
    for (int c = 0; c < MAX_COLUMNS; c++) dst->columns[c].volume = 100;

    bool declared[MAX_PATTERNS] = {};
    bool inSong = false, ended = false;
    int  highest = -1;
    int  lineNo = 0;
    static Line L;   // ~770 bytes: off the caller's stack
    char b[TOK_LEN];

    size_t i = 0;
    while (i < len && !ended) {
        size_t s = i;
        while (i < len && text[i] != '\n') i++;
        size_t e = i;
        if (i < len) i++;
        lineNo++;
        // Trim, and drop markdown fences / bullets the model might add.
        while (s < e && (text[s] == ' ' || text[s] == '\t')) s++;
        while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) e--;
        if (s == e || text[s] == '#') continue;

        if (e - s > 8 && strncmp(text + s, "SUMMARY ", 8) == 0) {
            size_t n = e - s - 8;
            if (n >= sizeof(r.summary)) n = sizeof(r.summary) - 1;
            memcpy(r.summary, text + s + 8, n);
            r.summary[n] = '\0';
            continue;
        }
        if (!inSong) {
            if (e - s >= 13 && strncmp(text + s, "MAGITRAC SONG", 13) == 0) inSong = true;
            continue;
        }
        tokenise(text + s, e - s, L);
        const char* kw = L.tok[0];

        if (!strcmp(kw, "END")) { ended = true; break; }

        if (!strcmp(kw, "N")) {
            long blk, row, col;
            uint8_t note, fx, prm, vel = VEL_DEFAULT;
            if (L.n < 7 || !toInt(L.tok[1], 0, MAX_PATTERNS - 1, blk) ||
                !toInt(L.tok[2], 0, MAX_ROWS - 1, row) ||
                !toInt(L.tok[3], 0, MAX_COLUMNS - 1, col) ||
                !parseNote(L.tok[4], note) || !parseFx(L.tok[6], (uint8_t)col, fx, prm)) {
                fail(r, lineNo, "bad note line"); r.skipped++; continue;
            }
            if (strcmp(L.tok[5], "-") && strcmp(L.tok[5], "--")) {
                long v;
                if (!toInt(L.tok[5], 0, 127, v)) { fail(r, lineNo, "bad velocity"); r.skipped++; continue; }
                vel = (uint8_t)v;
            }
            if (!declared[blk] || row >= dst->patterns[blk].length) {
                fail(r, lineNo, "note outside its block"); r.skipped++; continue;
            }
            if (note == NOTE_EMPTY && fx == 0 && prm == 0) continue;
            Note n = { note, vel, fx, prm };
            NoteGrid g(dst->notePool, &dst->noteFreeHead, &dst->patterns[blk].noteHead);
            if (!g.set((uint8_t)row, (uint8_t)col, n)) {
                fail(r, lineNo, "too many notes (4000 max)");
                return false;
            }
            r.notes++;
            continue;
        }

        if (!strcmp(kw, "SONG")) {
            long v;
            if (kv(L, 1, "name", b, sizeof(b))) copyName(dst->name, sizeof(dst->name), b);
            if (kvInt(L, "bpm", 20, 400, v))    dst->bpm    = (uint16_t)v;
            if (kvInt(L, "minbpm", 20, 400, v)) dst->minBPM = (uint16_t)v;
            if (kvInt(L, "maxbpm", 20, 400, v)) dst->maxBPM = (uint16_t)v;
            if (kvInt(L, "speed", 1, 31, v))    dst->speed  = (uint8_t)v;
            if (kvInt(L, "start", 0, MAX_PATTERNS - 1, v)) dst->startPattern = (uint8_t)v;
            continue;
        }

        if (!strcmp(kw, "COL")) {
            long c, v;
            if (L.n < 2 || !toInt(L.tok[1], 1, MAX_COLUMNS - 1, c)) {
                fail(r, lineNo, "bad column"); r.skipped++; continue;
            }
            ColumnSettings& cs = dst->columns[c];
            if (kv(L, 2, "ch", b, sizeof(b))) {
                if      (!strcasecmp(b, "OFF"))    cs.midiChannel = 0;
                else if (!strcasecmp(b, "SFX"))    cs.midiChannel = SFX_CHANNEL;
                else if (!strcasecmp(b, "ORGAN"))  cs.midiChannel = ORGAN_CHANNEL;
                else if (!strcasecmp(b, "LIGHTS")) cs.midiChannel = PIXELPOST_CHANNEL;
                else if (toInt(b, 1, 16, v))       cs.midiChannel = (uint8_t)v;
                else { fail(r, lineNo, "bad channel"); r.skipped++; continue; }
            }
            if (kvInt(L, "bank", 0, 127, v))   cs.bankMSB   = (uint8_t)v;
            if (kvInt(L, "prog", 0, 127, v))   cs.program   = (uint8_t)v;
            if (kvInt(L, "vol", 0, 127, v))    cs.volume    = (uint8_t)v;
            if (kvInt(L, "tr", -24, 24, v))    cs.transpose = (int8_t)v;
            if (kvInt(L, "mute", 0, 1, v))     cs.mute      = (uint8_t)v;
            if (kvInt(L, "tune", -4800, 4800, v)) cs.sfxTuneCents = (int16_t)v;
            if (kv(L, 2, "name", b, sizeof(b))) copyName(cs.name, sizeof(cs.name), b);
            continue;
        }

        if (!strcmp(kw, "ORGAN")) {
            OrganPatch& op = dst->organ;
            long v;
            int  voice = -1;
            if (kv(L, 1, "voice", b, sizeof(b))) {
                for (int k = 0; k <= ORGAN_VOICE_SYNTH; k++)
                    if (k != 7 && !strcasecmp(b, ORGAN_VOICE_TEXT[k])) voice = k;
                if (voice < 0) { fail(r, lineNo, "unknown organ voice"); r.skipped++; continue; }
            }
            if (voice >= 0) organPatchSetVoice(op, (uint8_t)voice);
            else            op.flags |= 1;
            if (kv(L, 1, "vib", b, sizeof(b)))
                for (int k = 0; k < 7; k++) if (!strcasecmp(b, ORGAN_VIB_TEXT[k])) op.vibChorus = (uint8_t)k;
            if (kv(L, 1, "leslie", b, sizeof(b)))
                for (int k = 0; k < 3; k++) if (!strcasecmp(b, ORGAN_LESLIE_TEXT[k])) op.leslie = (uint8_t)k;
            if (kv(L, 1, "reverb", b, sizeof(b)))
                for (int k = 0; k < 3; k++) if (!strcasecmp(b, ORGAN_REVERB_TEXT[k])) op.reverb = (uint8_t)k;
            if (toInt(kv(L, 1, "drive", b, sizeof(b)), 0, 1, v)) op.drive = (uint8_t)v;
            if (!organVoiceIsSynth(op.voice)) {
                if (kv(L, 1, "bars", b, sizeof(b)) && strlen(b) == ORGAN_PATCH_BARS) {
                    for (int k = 0; k < ORGAN_PATCH_BARS; k++)
                        if (b[k] >= '0' && b[k] <= '8') op.bars[k] = (uint8_t)(b[k] - '0');
                }
                if (kv(L, 1, "sliders", b, sizeof(b))) {
                    const char* q = b;
                    for (int k = 0; k < ORGAN_PATCH_SLIDERS && *q; k++) {
                        long sv = strtol(q, (char**)&q, 10);
                        if (sv >= 0 && sv <= 8) op.sliders[k] = (uint8_t)sv;
                        if (*q == ',') q++;
                    }
                }
            }
            continue;
        }

        if (!strcmp(kw, "SYNTH")) {
            OrganPatch& op = dst->organ;
            organPatchSetVoice(op, ORGAN_VOICE_SYNTH);   // a SYNTH line implies the voice
            for (int i = 0; i < ORGAN_SYNTH_PARAMS; i++) {
                if (!kv(L, 1, ORGAN_SYNTH_KEYS[i], b, sizeof(b))) continue;
                long v = -1;
                if (i == SYN_OSC1 || i == SYN_OSC2) {
                    for (int w = 0; w <= ORGAN_SYNTH_MAX[i]; w++)
                        if (!strcasecmp(b, SYN_WAVE_NAMES[w])) v = w;
                } else if (i == SYN_PITCH) {
                    if (toInt(b, -12, 12, v)) v += 12; else v = -1;
                } else if (!toInt(b, 0, ORGAN_SYNTH_MAX[i], v)) {
                    v = -1;
                }
                if (v < 0) { fail(r, lineNo, "bad synth setting"); continue; }
                organSynthParam(op, i) = (uint8_t)v;
            }
            continue;
        }

        if (!strcmp(kw, "BLOCK")) {
            long blk, v;
            if (L.n < 2 || !toInt(L.tok[1], 0, MAX_PATTERNS - 1, blk)) {
                fail(r, lineNo, "bad block number"); r.skipped++; continue;
            }
            Pattern& p = dst->patterns[blk];
            if (kvInt(L, "len", 1, 64, v)) {
                if (!validLen(v)) { fail(r, lineNo, "block length must be 16/24/32/48/64"); return false; }
                p.length = (uint8_t)v;
            }
            if (kv(L, 2, "name", b, sizeof(b))) copyName(p.name, sizeof(p.name), b);
            uint8_t pc;
            if (kv(L, 2, "ref", b, sizeof(b)) && parsePc(b, pc)) p.referenceNote = pc;
            if (kv(L, 2, "keymode", b, sizeof(b))) p.keyChangeMode = !strcasecmp(b, "TOP") ? 1 : 0;
            if (kv(L, 2, "end", b, sizeof(b))) {
                long t = 0;
                const char* colon = strchr(b, ':');
                if (colon && !toInt(colon + 1, 0, NAV_TARGET_MASK, t)) t = -1;
                if      (!strcasecmp(b, "LOOP"))      p.blockEndNav = NAV_LOOP;
                else if (!strcasecmp(b, "RETURN"))    p.blockEndNav = NAV_RNT;
                else if (t < 0)                       { fail(r, lineNo, "bad end target"); }
                else if (!strncasecmp(b, "NEXT", 4))  p.blockEndNav = NAV_FWD  | (uint8_t)(colon ? t : 1);
                else if (!strncasecmp(b, "BACK", 4))  p.blockEndNav = NAV_BACK | (uint8_t)(colon ? t : 1);
                else if (!strncasecmp(b, "GOTO", 4))  p.blockEndNav = NAV_ABS  | (uint8_t)t;
                else fail(r, lineNo, "bad end");
                // NEXT:0 / BACK:0 would collide with LOOP / the RETURN sentinel
                if ((p.blockEndNav & NAV_MODE_MASK) == NAV_FWD  && !(p.blockEndNav & NAV_TARGET_MASK)) p.blockEndNav = NAV_LOOP;
                if ((p.blockEndNav & NAV_MODE_MASK) == NAV_BACK && !(p.blockEndNav & NAV_TARGET_MASK)) p.blockEndNav = NAV_LOOP;
            }
            if (!declared[blk]) r.blocks++;
            declared[blk] = true;
            if (blk > highest) highest = (int)blk;
            continue;
        }

        if (!strcmp(kw, "KEY")) {
            long blk, v;
            uint8_t pc;
            if (L.n < 3 || !toInt(L.tok[1], 0, MAX_PATTERNS - 1, blk) || !parsePc(L.tok[2], pc)) {
                fail(r, lineNo, "bad key line"); r.skipped++; continue;
            }
            InputNoteEntry& en = dst->patterns[blk].inputNotes[pc];
            if (kv(L, 3, "switch", b, sizeof(b)))
                en.switchMode = !strcasecmp(b, "TOP")  ? BlockSwitch::TOP
                              : !strcasecmp(b, "SAME") ? BlockSwitch::SAME_POS : BlockSwitch::STAY;
            if (kvInt(L, "target", 0, MAX_PATTERNS - 1, v)) en.switchTarget = (uint8_t)v;
            if (kv(L, 3, "tr", b, sizeof(b)))
                en.transposeAction = !strcasecmp(b, "NOTE")   ? TransposeAction::NOTE
                                   : !strcasecmp(b, "CUSTOM") ? TransposeAction::CUSTOM
                                                              : TransposeAction::KEEP;
            if (kvInt(L, "by", -24, 24, v)) en.transposeValue = (int8_t)v;
            continue;
        }

        r.skipped++;   // unknown line: ignored
    }

    if (!inSong)  { fail(r, 0, "no MAGITRAC SONG in the reply"); return false; }
    if (!ended)   { fail(r, 0, "the reply stopped before END"); return false; }
    if (highest < 0) { fail(r, 0, "the song has no blocks"); return false; }
    for (int b2 = 0; b2 <= highest; b2++)
        if (!declared[b2]) { fail(r, 0, "blocks must be numbered 0, 1, 2... with no gaps"); return false; }
    dst->numPatterns = (uint8_t)(highest + 1);
    if (dst->startPattern >= dst->numPatterns) dst->startPattern = 0;
    if (dst->minBPM > dst->maxBPM) { uint16_t t = dst->minBPM; dst->minBPM = dst->maxBPM; dst->maxBPM = t; }
    if (dst->bpm < dst->minBPM) dst->bpm = dst->minBPM;
    if (dst->bpm > dst->maxBPM) dst->bpm = dst->maxBPM;
    for (int b2 = 0; b2 < dst->numPatterns; b2++)
        for (int k = 0; k < 12; k++)
            if (dst->patterns[b2].inputNotes[k].switchTarget >= dst->numPatterns)
                dst->patterns[b2].inputNotes[k].switchMode = BlockSwitch::STAY;

    // Malformed lines are skipped, but more than a few means the model lost
    // the format — don't install a song with holes in it.
    if (r.skipped > 8 && r.skipped * 10 > r.notes) {
        char m[48];
        snprintf(m, sizeof(m), "%d lines didn't parse", r.skipped);
        r.err[0] = '\0';
        fail(r, r.line, m);
        return false;
    }
    if (r.skipped == 0) r.err[0] = '\0';
    return true;
}
