#include "claude_ai.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include "sd_mutex.h"
#include "song_text.h"
#include "SampleManifest.h"
#include "MagiMsg.h"
#include "ai_certs.h"

static const char*    AI_HOST        = "api.anthropic.com";
static const char*    AI_KEY_PATH    = "/ai/key.txt";
static const char*    AI_DEF_MODEL   = "claude-opus-5-5";
static const char*    AI_DEF_EFFORT  = "medium";
static const uint32_t AI_MAX_TOKENS  = 64000;
static const size_t   AI_TEXT_CAP    = 160 * 1024;   // song + samples + prompt, as text
static const size_t   AI_REPLY_CAP   = 192 * 1024;   // the reply text
static const size_t   AI_LINE_CAP    = 8 * 1024;     // one SSE line
static const uint32_t AI_IDLE_MS     = 180000;       // no bytes for this long = dead
static const uint32_t AI_STACK       = 12 * 1024;
// The task's stack and lwIP's socket buffers stay internal; TLS goes to
// PSRAM (tlsCalloc below).
static const size_t   AI_MIN_FREE    = 32 * 1024;
static const size_t   AI_MIN_BLOCK   = 16 * 1024;

// This core's mbedTLS is built to allocate from internal RAM only, and a TLS
// session wants ~45 KB of it (two 16 KB records + the handshake) - more than
// the server has spare with the client connected (the connect failed at once
// with ~50 KB free).  For the length of a request, mbedTLS allocates from
// PSRAM instead (what ESP-IDF's MBEDTLS_EXTERNAL_MEM_ALLOC option does).
// tlsCallocInternal is what ESP-IDF's esp_mbedtls_mem_calloc does with this
// core's CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC (its header lacks extern "C", so
// it can't be named from C++).  Both free with heap_caps_free, so a block
// freed after the switch back is still freed correctly.
static void* tlsCalloc(size_t n, size_t size) {
    return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void* tlsCallocInternal(size_t n, size_t size) {
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void tlsFree(void* p) { heap_caps_free(p); }

// ── What the model is told ──────────────────────────────────────────────────
// Keep in step with song_text.cpp (the parser is the authority).
static const char AI_SYSTEM[] = R"SYS(You are the composing assistant built into Magitrac, a live-performance MIDI tracker used by a keyboard player in a band. The performer types a short request on the device (for example "Create a happy song in G" or "add a bass line to the chorus"). You receive the request and the current song, and you reply with the complete new song in the Magitrac text format below. The device parses your reply and loads it; the performer can undo it.

## How Magitrac plays a song
- A song is a list of blocks (patterns), numbered from 0, at most 50. Each block has 16, 24, 32, 48 or 64 rows. The sequencer plays block `start`'s rows top to bottom, then follows that block's `end` rule.
- Row length in seconds = speed * 2.5 / bpm. With the usual speed=6, one row is a 16th note: 4 rows per beat, 16 rows per 4/4 bar. A 64-row block is 4 bars. A full song therefore needs several blocks chained with end rules (e.g. intro, verse, chorus).
- There are 21 columns. Column 0 is the cue lane (described below) and makes no sound. Columns 1-20 each play one instrument. Each column is MONOPHONIC: a note keeps sounding until the next note or OFF in the same column. Put each voice of a chord in its own column (e.g. columns 4, 5, 6 for a triad pad) and use OFF to end notes, otherwise they ring until the next note.
- Notes are written like C-4, C#4, D-4 ... B-4 (octaves 0-7). C-4 is middle C (MIDI 60). Use sharps (C#, D#, F#, G#, A#); flats like Bb4 are also accepted.
- MIDI drums are on channel 10 with the General MIDI drum map: kick C-2 (36), side stick C#2, snare D-2 (38), clap D#2, closed hi-hat F#2 (42), pedal hat G#2, open hi-hat A#2 (46), low tom A-2, mid tom C-3, high tom D-3, crash C#3 (49), ride D#3 (51), tambourine F#3, cowbell G#3. A drum hit needs no OFF.

## The cue lane (column 0) - what makes Magitrac different
The song follows the performer instead of a click. Cues in column 0 react to the notes the performer plays on their keyboard (octave is ignored, only the pitch class matters):
- WAIT: the playhead halts at this row until the performer plays the cue's note, then the row fires instantly. The tempo is worked out from the time between WAITs, so the band follows the performer's tempo. If nothing is played the block returns to its top after 0.5 s (WAT1 = 1 s, WAT2 = 2 s).
- SYNC: does not halt; if the performer plays the note near here the playhead snaps to this row (re-alignment without a tempo change).
- ANY as the cue note matches whatever the performer plays.
- A cue-lane note with no cue effect (`-`, shown as PASS) absorbs the performer's matching note so it isn't treated as a cue; PASA absorbs every matching note until the playhead passes.
- AVRG sets the tempo to the average of the last four WAIT tempos.
Guidance: keep the existing cue lane unless asked to change it. For a new song, put WAITs on notes the performer would naturally play at the start of phrases (typically row 0 of each block, on the chord root they play), and at least a WAIT at row 0 of the first block so the song starts when they play. Don't put WAITs so close together that the performer has to play every beat unless asked.

## Keys table (per block)
Each block can say what happens when the performer plays each of the 12 pitch classes: switch to another block (`switch=TOP` restart it at row 0, `switch=SAME` keep the row position) and/or transpose the band (`tr=NOTE` follow the played note relative to the block's `ref` key, `tr=CUSTOM by=N` shift by N semitones). This is how a performer jumps to the chorus or changes key live. Leave keys out unless they help the request.

## Columns and instruments
- `ch=1`..`16` sends MIDI to the performer's synths on that channel with `prog` (program number), `bank` (CC0) and `vol` (CC7). prog and bank count from 0 (the device shows them +1), and the message gives the range the performer's synths accept - never go past it. Their sounds are not General MIDI: reuse the prog/bank of an existing column whose name suits the part. A channel plays one sound at a time, so columns on the same channel must have the same prog and bank. `tr` transposes the column in semitones.
- `ch=SFX` plays a WAV sample from the server's SD card; `prog` is the sample id from the sample list in the message. C-4 plays the sample at its own pitch.
- `ch=ORGAN` plays the server's built-in organ / synth - see "The organ" below.
- `ch=LIGHTS` drives the light show: note = light effect, velocity = intensity, FX:XXYY = position. Keep LIGHTS columns as they are unless asked.
- `ch=OFF` is an unused column.
- The existing columns are wired to the performer's real instruments. Keep them as they are (ch/bank/prog/name) unless asked to change them. Columns that are not in your reply are removed, so repeat every column you keep.

## The organ (ch=ORGAN)
The server has one built-in sound engine. A song has ONE organ sound, given by the ORGAN line (plus a SYNTH line when the voice is SYNTH); every ORGAN column plays it, one note at a time per column like any other column, and bank/prog are ignored on them.
- For a new part, use the organ (ORGAN columns, with a SYNTH or organ voice designed for the part) rather than a MIDI channel. Only put a new part on MIDI when the request names a channel or one of the performer's instruments (e.g. "on channel 3", "use the Nord"); then do exactly that.
- If the song's ORGAN columns already play a different sound that the request doesn't ask to change, don't change it: put the new part on MIDI instead, reusing a MIDI column's channel/bank/prog whose name suits the part.
- Changing the organ sound changes every ORGAN column. Only change it when the request is about that sound or the song has no ORGAN column yet. Leave the ORGAN/SYNTH lines out (or repeat them unchanged) to keep the current sound.
- While a song has an ORGAN column the organ takes over the audio output and ch=SFX columns go silent. Don't add ORGAN columns to a song that relies on SFX columns, or SFX columns to a song that uses the organ.
Voices:
- DRAWBAR: pure sine drawbars. With vib=OFF leslie=STOP reverb=HALL it is a pipe (flue) organ.
- TONEWHEEL: a Hammond, with key click. Classic settings: vib=C3, leslie=SLOW or FAST, drive=1 for rock.
- CLAUDE: a soft, slowly evolving sine pad (the drawbars set its voicing).
- NEBULA: a thick, detuned sci-fi pad that glides up to pitch.
- REED: harmonium / accordion. As an oboe: sliders=8,0,1,0,8.
- CHOIR: a vintage tape/disc "ahh" choir.
- VOCODER: only works with someone singing into the server's microphone - don't choose it unless asked.
- SYNTH: a subtractive synth for pads, strings, brass, basses, leads and plucks - see the SYNTH line.
`bars` is nine digits 0-8 for the footages 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1' (8 = fully out). The device's presets: 888000000 jazz, 888800000 rock, 888800088 gospel, 008806000 flutes, 888888888 full. A church organ: 808806000 for principals, 808888008 for full organ.
`sliders` is five values 0-8, the voice's own controls in this order (DRAWBAR and CLAUDE ignore them): TONEWHEEL click,-,-,-,- ; NEBULA detune,glide,bright,-,- ; REED wave(hollow..nasal),celeste,bright,stops,chamber ; CHOIR breath,ensemble,bright,attack,ahh ; VOCODER wave,pwm,sss,gate,smooth. Use 4 for anything you have no reason to change.
Effects: vib=OFF|V1|V2|V3 (vibrato)|C1|C2|C3 (chorus), leslie=STOP|SLOW|FAST, drive=0|1, reverb=OFF|ROOM|HALL.

The SYNTH line (every setting 0-16 unless shown; settings left out keep their default):
- osc1=SAW|SQUARE|TRI|SINE, osc2=SAW|SQUARE|TRI|SINE|NOISE. mix: 0 = osc1 only, 8 = even, 16 = osc2 only. pitch: osc2's tuning in semitones, -12..12 (12 = an octave up, 7 = a fifth). detune: osc2 slightly sharp for thickness (4 = 3 cents, 6 = 7, 8 = 13, 12 = 29). pw: pulse width of a SQUARE, 0 = square .. 16 = thin and nasal. sub: a square one octave below osc1.
- cutoff: the low-pass filter, 40 Hz x 2^(0.55 x cutoff): 4 = 180 Hz, 8 = 840 Hz, 10 = 1.8 kHz, 12 = 3.9 kHz, 16 = fully open; it follows the keyboard (half an octave per octave played). reso: resonance, 0 none .. 16 ringing. env: how far the envelope opens the filter, 0.4 octaves per step.
- One envelope drives both the level and the filter. attack = 1 + attack^3 ms (0 instant, 4 = 65 ms, 8 = 0.5 s, 10 = 1 s, 12 = 1.7 s). decay = 10 + 1.5 x decay^3 ms (6 = 0.33 s, 8 = 0.8 s, 12 = 2.6 s). sustain: the level held while the key is down (16 = full). release = 5 + 2 x release^3 ms (4 = 0.13 s, 6 = 0.44 s, 8 = 1 s, 12 = 3.5 s).
- Notes are velocity-sensitive. The organ's vib/leslie/drive/reverb apply to SYNTH too: chorus (C2/C3) and HALL suit pads and strings.
- Starting points (the device's presets):
  PAD     osc1=SAW osc2=SAW mix=8 pitch=0 detune=6 pw=0 sub=0 cutoff=9 reso=2 env=2 attack=9 decay=10 sustain=14 release=9
  STRINGS osc1=SAW osc2=SAW mix=8 pitch=0 detune=5 pw=0 sub=0 cutoff=11 reso=1 env=1 attack=7 decay=8 sustain=14 release=8
  BRASS   osc1=SAW osc2=SAW mix=8 pitch=0 detune=3 pw=0 sub=0 cutoff=7 reso=3 env=7 attack=4 decay=7 sustain=11 release=5
  BASS    osc1=SAW osc2=SQUARE mix=6 pitch=-12 detune=2 pw=4 sub=6 cutoff=6 reso=5 env=6 attack=0 decay=6 sustain=6 release=3
  LEAD    osc1=SQUARE osc2=SAW mix=8 pitch=0 detune=4 pw=6 sub=0 cutoff=10 reso=5 env=3 attack=0 decay=8 sustain=13 release=4
  PLUCK   osc1=SAW osc2=SQUARE mix=6 pitch=12 detune=3 pw=4 sub=0 cutoff=5 reso=6 env=9 attack=0 decay=6 sustain=0 release=6

## The text format
Your reply must contain exactly one song, written like this:

MAGITRAC SONG 1
SUMMARY <one sentence, at most 90 characters, saying what you made or changed>
SONG name="<song name>" bpm=<20-400> minbpm=<n> maxbpm=<n> speed=<1-31> start=<first block>
COL <1-20> ch=<1-16|SFX|ORGAN|LIGHTS|OFF> bank=<0-127> prog=<0-127> vol=<0-127> tr=<-24..24> mute=<0|1> tune=<cents, SFX only> name="<up to 11 chars>"
ORGAN voice=<DRAWBAR|TONEWHEEL|CLAUDE|NEBULA|REED|CHOIR|VOCODER|SYNTH> vib=<OFF|V1|V2|V3|C1|C2|C3> leslie=<STOP|SLOW|FAST> drive=<0|1> reverb=<OFF|ROOM|HALL> bars=<nine digits 0-8> sliders=<five values 0-8>
SYNTH osc1=<wave> osc2=<wave> mix=<n> pitch=<-12..12> detune=<n> pw=<n> sub=<n> cutoff=<n> reso=<n> env=<n> attack=<n> decay=<n> sustain=<n> release=<n>
BLOCK <n> len=<16|24|32|48|64> name="<up to 15 chars>" ref=<C..B> keymode=<SAME|TOP> end=<LOOP|NEXT:n|BACK:n|GOTO:n|RETURN>
KEY <block> <C..B> switch=<STAY|SAME|TOP> target=<block> tr=<KEEP|NOTE|CUSTOM> by=<-24..24>
N <block> <row> <column> <note> <velocity> <fx>
END

Rules:
- One item per line. ORGAN and SYNTH are optional (at most one of each; bars/sliders are left out when the voice is SYNTH). Blocks are numbered 0, 1, 2 ... with no gaps. Each block's BLOCK line comes before its KEY and N lines.
- `end`: LOOP repeats the block, NEXT:n / BACK:n move n blocks forward / back, GOTO:n jumps to block n, RETURN goes back to the block that jumped here. minbpm/maxbpm clamp the performer-derived tempo.
- N lines: row is 0 to len-1; note is C-0..B-7, OFF (end the note), ANY (cue lane only) or --- (no note, effect only); velocity is 0-127 or `-` for the default (100).
- fx is `-` for none; in the cue lane WAIT, WAT1, WAT2, SYNC, AVRG or PASA; in columns 1-20 PAN:<0-127> (64 = centre), RTRG:<2-8> (retrigger the note that many times within the row), or FX:XXYY to keep an effect you don't recognise exactly as it was.
- Leave out empty cells. The whole song may hold at most 4000 notes, so be economical: write a block once and reuse it with end rules rather than repeating it.
- SUMMARY and the song are all the device shows. Don't add explanations around the song, and never stop before END.
)SYS";

// ── State shared with the task ──────────────────────────────────────────────

static volatile bool     sBusy     = false;   // aiBegin .. aiRelease
static volatile bool     sFinished = false;   // the task is done (result below)
static volatile bool     sCancel   = false;
static volatile uint8_t  sState    = AI_ST_IDLE;
static volatile uint32_t sStartMs  = 0;
static volatile size_t   sReplyLen = 0;
static bool              sOk       = false;
static char*             sBody     = nullptr;  // PSRAM, the request JSON
static size_t            sBodyLen  = 0;
static char*             sReply    = nullptr;  // PSRAM, the reply text
static char              sErr[AI_TEXT_LEN];
static char              sKey[160];
static char              sModel[48];
static char              sEffort[12];

struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramAllocator sJsonAlloc;

static void setErr(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sErr, sizeof(sErr), fmt, ap);
    va_end(ap);
}

// ── Building the request (loop task) ────────────────────────────────────────

static bool readKeyFile(char* err, size_t errLen) {
    sKey[0] = '\0';
    strcpy(sModel, AI_DEF_MODEL);
    strcpy(sEffort, AI_DEF_EFFORT);
    SdLock _;
    File f = SD.open(AI_KEY_PATH, FILE_READ);
    if (!f) { snprintf(err, errLen, "No API key: put it in %s on the server's card", AI_KEY_PATH); return false; }
    char line[200];
    while (f.available()) {
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        char* e = s + strlen(s);
        while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        if (!*s || *s == '#') continue;
        if      (!strncmp(s, "model=", 6))  strlcpy(sModel, s + 6, sizeof(sModel));
        else if (!strncmp(s, "effort=", 7)) strlcpy(sEffort, s + 7, sizeof(sEffort));
        else if (!strchr(s, '=') && !sKey[0]) strlcpy(sKey, s, sizeof(sKey));
    }
    f.close();
    if (!sKey[0]) { snprintf(err, errLen, "%s has no API key in it", AI_KEY_PATH); return false; }
    return true;
}

static size_t jsonEscLen(const char* s, size_t n) {
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') out += 2;
        else if (c < 0x20) out += 6;
        else out++;
    }
    return out;
}

static void jsonEsc(TextBuf& t, const char* s, size_t n) {
    for (size_t i = 0; i < n && !t.overflow; i++) {
        uint8_t c = (uint8_t)s[i];
        if (t.len + 7 >= t.cap) { t.overflow = true; break; }
        char* o = t.p + t.len;
        switch (c) {
            case '"':  o[0] = '\\'; o[1] = '"';  t.len += 2; break;
            case '\\': o[0] = '\\'; o[1] = '\\'; t.len += 2; break;
            case '\n': o[0] = '\\'; o[1] = 'n';  t.len += 2; break;
            case '\r': o[0] = '\\'; o[1] = 'r';  t.len += 2; break;
            case '\t': o[0] = '\\'; o[1] = 't';  t.len += 2; break;
            default:
                if (c < 0x20)       { snprintf(o, 7, "\\u%04x", c); t.len += 6; }
                else if (c >= 0x80) { o[0] = '?'; t.len++; }   // a stray byte would make the JSON invalid
                else                { o[0] = (char)c; t.len++; }
        }
    }
    if (!t.overflow) t.p[t.len] = '\0';
}

static bool buildBody(const Song* song, const char* prompt, uint8_t maxBank, uint8_t maxProg,
                      char* err, size_t errLen) {
    TextBuf u = { (char*)heap_caps_malloc(AI_TEXT_CAP, MALLOC_CAP_SPIRAM), AI_TEXT_CAP, 0, false };
    if (!u.p) { snprintf(err, errLen, "Out of memory (song text)"); return false; }
    u.p[0] = '\0';
    textPrintf(u, "The performer's request: %s\n\n", prompt);
    textPrintf(u, "The performer's MIDI synths accept prog 0-%u and bank 0-%u (shown on the device as 1-%u and 1-%u). "
                  "Drum kits on channel 10 are numbered their own way.\n\n",
               (unsigned)maxProg, (unsigned)maxBank, (unsigned)maxProg + 1, (unsigned)maxBank + 1);
    int nSamples = 0;
    for (int id = 1; id <= 127; id++) {
        const char* nm = sampleManifestNameFor((uint8_t)id);
        if (!nm) continue;
        if (!nSamples++) textPrintf(u, "Samples on the server's SD card (for ch=SFX, prog = id):\n");
        textPrintf(u, "SAMPLE %d \"%s\"\n", id, nm);
    }
    if (!nSamples) textPrintf(u, "There are no samples on the server's SD card.\n");
    textPrintf(u, "\nThe current song:\n");
    if (!songToText(song, u) || u.overflow) {
        heap_caps_free(u.p);
        snprintf(err, errLen, "The song is too big to send");
        return false;
    }

    size_t sysLen = strlen(AI_SYSTEM);
    size_t cap = 512 + strlen(sModel) + strlen(sEffort) +
                 jsonEscLen(AI_SYSTEM, sysLen) + jsonEscLen(u.p, u.len);
    TextBuf b = { (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), cap, 0, false };
    if (!b.p) { heap_caps_free(u.p); snprintf(err, errLen, "Out of memory (request)"); return false; }
    textPrintf(b, "{\"model\":\"%s\",\"max_tokens\":%u,\"stream\":true,"
                  "\"output_config\":{\"effort\":\"%s\"},\"fallbacks\":\"default\","
                  "\"system\":[{\"type\":\"text\",\"text\":\"",
               sModel, (unsigned)AI_MAX_TOKENS, sEffort);
    jsonEsc(b, AI_SYSTEM, sysLen);
    textPrintf(b, "\",\"cache_control\":{\"type\":\"ephemeral\"}}],"
                  "\"messages\":[{\"role\":\"user\",\"content\":\"");
    jsonEsc(b, u.p, u.len);
    textPrintf(b, "\"}]}");
    heap_caps_free(u.p);
    if (b.overflow) { heap_caps_free(b.p); snprintf(err, errLen, "Request too big"); return false; }
    sBody    = b.p;
    sBodyLen = b.len;
    return true;
}

// ── The HTTPS call (AI task) ────────────────────────────────────────────────

// Bytes from the socket with an idle timeout and cancel; -1 = no more.
class Reader {
public:
    explicit Reader(WiFiClientSecure& c) : _c(c) { _last = millis(); }
    int get() {
        if (_pos < _len) return _buf[_pos++];
        for (;;) {
            if (sCancel) return -1;
            int a = _c.available();
            if (a > 0) {
                int n = _c.read(_buf, a < (int)sizeof(_buf) ? a : (int)sizeof(_buf));
                if (n > 0) { _len = n; _pos = 1; _last = millis(); return _buf[0]; }
            }
            if (!_c.connected()) return -1;
            if ((uint32_t)(millis() - _last) > AI_IDLE_MS) { _timedOut = true; return -1; }
            delay(5);
        }
    }
    // One line without its CR LF; false at end of stream.
    bool line(char* out, size_t cap, size_t& n) {
        n = 0;
        int ch;
        while ((ch = get()) >= 0) {
            if (ch == '\n') { if (n && out[n - 1] == '\r') n--; out[n] = '\0'; return true; }
            if (n + 1 < cap) out[n++] = (char)ch;
        }
        out[n] = '\0';
        return n > 0;
    }
    bool timedOut() const { return _timedOut; }
private:
    WiFiClientSecure& _c;
    uint8_t  _buf[1024];
    int      _pos = 0, _len = 0;
    uint32_t _last;
    bool     _timedOut = false;
};

// The body, de-chunked when the response is chunked.
class Body {
public:
    Body(Reader& r, bool chunked) : _r(r), _chunked(chunked) {}
    int get() {
        if (!_chunked) return _r.get();
        if (_left == 0) {
            if (_started) { int a = _r.get(), b = _r.get(); (void)a; (void)b; }   // CR LF after a chunk
            _started = true;
            char hex[20]; size_t n;
            if (!_r.line(hex, sizeof(hex), n)) return -1;
            _left = strtoul(hex, nullptr, 16);
            if (_left == 0) return -1;
        }
        int ch = _r.get();
        if (ch >= 0) _left--;
        return ch;
    }
    bool line(char* out, size_t cap, size_t& n) {
        n = 0;
        int ch;
        while ((ch = get()) >= 0) {
            if (ch == '\n') { if (n && out[n - 1] == '\r') n--; out[n] = '\0'; return true; }
            if (n + 1 < cap) out[n++] = (char)ch;
        }
        out[n] = '\0';
        return n > 0;
    }
private:
    Reader&  _r;
    bool     _chunked;
    bool     _started = false;
    uint32_t _left = 0;
};

static void appendReply(const char* s, size_t n) {
    size_t len = sReplyLen;
    if (len + n + 1 > AI_REPLY_CAP) { n = AI_REPLY_CAP - 1 - len; }
    memcpy(sReply + len, s, n);
    sReply[len + n] = '\0';
    sReplyLen = len + n;
}

static bool sendAll(WiFiClientSecure& c, const char* p, size_t n) {
    while (n) {
        if (sCancel) return false;
        size_t k = n > 4096 ? 4096 : n;
        size_t w = c.write((const uint8_t*)p, k);
        if (w == 0) return false;
        p += w; n -= w;
    }
    return true;
}

static void aiRun() {
    WiFiClientSecure c;
    c.setCACert(AI_ROOT_CAS);
    c.setHandshakeTimeout(30);
    Serial.printf("[AI] connecting (internal free %u, block %u)\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    IPAddress ip;
    if (!WiFi.hostByName(AI_HOST, ip)) {
        Serial.printf("[AI] DNS lookup of %s failed (DNS server %s)\n", AI_HOST,
                      WiFi.dnsIP().toString().c_str());
        setErr("Can't look up %s - is the server on the internet?", AI_HOST);
        return;
    }
    Serial.printf("[AI] %s is %s\n", AI_HOST, ip.toString().c_str());
    if (!c.connect(AI_HOST, 443, 20000)) {
        char why[100] = "";
        int code = c.lastError(why, sizeof(why));
        Serial.printf("[AI] connect failed: %d %s\n", code, why);
        if (code) setErr("Secure connection to the API failed (-0x%04X)", (unsigned)-code);
        else      setErr("Can't reach %s - is the server on the internet?", AI_HOST);
        return;
    }
    char hdr[600];
    int hn = snprintf(hdr, sizeof(hdr),
        "POST /v1/messages HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-api-key: %s\r\n"
        "anthropic-version: 2026-10-21\r\n"
        "anthropic-beta: server-side-fallback-2026-07-01\r\n"
        "content-type: application/json\r\n"
        "accept: text/event-stream\r\n"
        "content-length: %u\r\n"
        "connection: close\r\n\r\n",
        AI_HOST, sKey, (unsigned)sBodyLen);
    if (!sendAll(c, hdr, hn) || !sendAll(c, sBody, sBodyLen)) {
        setErr(sCancel ? "Cancelled" : "The connection dropped while sending");
        c.stop();
        return;
    }
    Serial.printf("[AI] sent %u bytes (model %s, effort %s)\n", (unsigned)sBodyLen, sModel, sEffort);
    sState = AI_ST_THINKING;

    Reader r(c);
    char* line = (char*)heap_caps_malloc(AI_LINE_CAP, MALLOC_CAP_SPIRAM);
    if (!line) { setErr("Out of memory"); c.stop(); return; }
    size_t n;
    int status = 0;
    bool chunked = false;
    if (r.line(line, AI_LINE_CAP, n)) sscanf(line, "HTTP/%*s %d", &status);
    while (r.line(line, AI_LINE_CAP, n) && n) {
        for (char* q = line; *q; q++) *q = (char)tolower((unsigned char)*q);
        if (!strncmp(line, "transfer-encoding:", 18) && strstr(line, "chunked")) chunked = true;
    }
    Body body(r, chunked);

    if (status != 200) {
        // An error body is one JSON object.
        size_t total = 0;
        int ch;
        while (total + 1 < AI_LINE_CAP && (ch = body.get()) >= 0) line[total++] = (char)ch;
        line[total] = '\0';
        JsonDocument doc(&sJsonAlloc);
        const char* msg = nullptr;
        if (deserializeJson(doc, line) == DeserializationError::Ok) msg = doc["error"]["message"];
        if (status == 0)        setErr(sCancel ? "Cancelled" : "No reply from the API");
        else if (status == 401) setErr("The API key was refused (401)");
        else if (status == 429) setErr("Rate limited (429) - try again in a minute");
        else if (status == 529) setErr("The API is overloaded (529) - try again");
        else                    setErr("API error %d: %s", status, msg ? msg : "");
        Serial.printf("[AI] HTTP %d: %s\n", status, line);
        heap_caps_free(line);
        c.stop();
        return;
    }

    char stop[24] = "";
    bool sawStop = false;
    JsonDocument doc(&sJsonAlloc);
    while (body.line(line, AI_LINE_CAP, n)) {
        if (n < 6 || strncmp(line, "data:", 5)) continue;
        const char* js = line + 5;
        while (*js == ' ') js++;
        if (deserializeJson(doc, js) != DeserializationError::Ok) continue;
        const char* type = doc["type"] | "";
        if (!strcmp(type, "content_block_delta")) {
            const char* dt = doc["delta"]["type"] | "";
            if (!strcmp(dt, "text_delta")) {
                const char* t = doc["delta"]["text"] | "";
                appendReply(t, strlen(t));
                sState = AI_ST_WRITING;
            }
        } else if (!strcmp(type, "message_start")) {
            Serial.printf("[AI] answering: %s, input %u tokens (%u from cache)\n",
                          (const char*)(doc["message"]["model"] | "?"),
                          (unsigned)(doc["message"]["usage"]["input_tokens"] | 0),
                          (unsigned)(doc["message"]["usage"]["cache_read_input_tokens"] | 0));
        } else if (!strcmp(type, "message_delta")) {
            const char* sr = doc["delta"]["stop_reason"] | "";
            if (*sr) strlcpy(stop, sr, sizeof(stop));
            Serial.printf("[AI] stop: %s, output %u tokens\n", stop,
                          (unsigned)(doc["usage"]["output_tokens"] | 0));
        } else if (!strcmp(type, "message_stop")) {
            sawStop = true;
            break;
        } else if (!strcmp(type, "error")) {
            setErr("API error: %s", (const char*)(doc["error"]["message"] | "?"));
            heap_caps_free(line);
            c.stop();
            return;
        }
    }
    heap_caps_free(line);
    c.stop();

    if (sCancel)                        { setErr("Cancelled"); return; }
    if (!sawStop)                       { setErr(r.timedOut() ? "The API went quiet - timed out"
                                                              : "The connection dropped mid-answer"); return; }
    if (!strcmp(stop, "refusal"))       { setErr("Claude declined that request"); return; }
    if (!strcmp(stop, "max_tokens"))    { setErr("The answer was too long and got cut off"); return; }
    if (sReplyLen + 1 >= AI_REPLY_CAP)  { setErr("The answer was too big for the server"); return; }
    sOk = true;
}

static void aiTask(void*) {
    mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
    aiRun();                               // locals (the TLS client) are gone after this
    mbedtls_platform_set_calloc_free(tlsCallocInternal, tlsFree);
    memset(sKey, 0, sizeof(sKey));
    if (sBody) { heap_caps_free(sBody); sBody = nullptr; }
    Serial.printf("[AI] finished in %lu s: %s (%u bytes)\n",
                  (unsigned long)((millis() - sStartMs) / 1000),
                  sOk ? "OK" : sErr, (unsigned)sReplyLen);
    sFinished = true;
    vTaskDelete(nullptr);
}

// ── Loop-task API ───────────────────────────────────────────────────────────

bool aiBegin(const Song* current, const char* prompt, char* err, size_t errLen,
             uint8_t maxBank, uint8_t maxProgram) {
    if (sBusy) { snprintf(err, errLen, "Already working on a request"); return false; }
    if (!prompt || !*prompt) { snprintf(err, errLen, "Type what you want first"); return false; }
    if ((WiFi.getMode() & WIFI_STA) == 0 || WiFi.status() != WL_CONNECTED) {
        snprintf(err, errLen, "The server isn't on an external WiFi network");
        return false;
    }
    size_t freeInt  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t blockInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (freeInt < AI_MIN_FREE || blockInt < AI_MIN_BLOCK) {
        snprintf(err, errLen, "Not enough memory (%u KB free) - close the organ?",
                 (unsigned)(freeInt / 1024));
        return false;
    }
    if (!readKeyFile(err, errLen)) return false;

    sReply = (char*)heap_caps_malloc(AI_REPLY_CAP, MALLOC_CAP_SPIRAM);
    if (!sReply) { snprintf(err, errLen, "Out of memory (reply)"); return false; }
    sReply[0] = '\0';
    sReplyLen = 0;
    if (!buildBody(current, prompt, maxBank, maxProgram, err, errLen)) {
        heap_caps_free(sReply); sReply = nullptr;
        memset(sKey, 0, sizeof(sKey));
        return false;
    }

    sOk       = false;
    sErr[0]   = '\0';
    sCancel   = false;
    sFinished = false;
    sState    = AI_ST_SENDING;
    sStartMs  = millis();
    sBusy     = true;
    if (xTaskCreatePinnedToCore(aiTask, "ai", AI_STACK, nullptr, 1, nullptr, 0) != pdPASS) {
        sBusy = false;
        heap_caps_free(sBody);  sBody = nullptr;
        heap_caps_free(sReply); sReply = nullptr;
        memset(sKey, 0, sizeof(sKey));
        snprintf(err, errLen, "Couldn't start the request task");
        return false;
    }
    Serial.printf("[AI] request: \"%s\"\n", prompt);
    return true;
}

void aiCancel()     { if (sBusy && !sFinished) sCancel = true; }
bool aiBusy()       { return sBusy; }
uint8_t  aiState()  { return sState; }
uint32_t aiBytes()  { return (uint32_t)sReplyLen; }
uint32_t aiSeconds(){ return sBusy ? (millis() - sStartMs) / 1000 : 0; }

bool aiTakeReply(bool& ok, const char*& reply, size_t& len, char* err, size_t errLen) {
    if (!sBusy || !sFinished) return false;
    ok    = sOk;
    reply = sReply;
    len   = sReplyLen;
    strlcpy(err, sErr, errLen);
    return true;
}

void aiRelease() {
    if (!sBusy || !sFinished) return;
    if (sReply) { heap_caps_free(sReply); sReply = nullptr; }
    sReplyLen = 0;
    sState    = AI_ST_IDLE;
    sBusy     = false;
}
