// Host test for claude_ai.cpp: the request it builds (headers, valid JSON,
// the song and prompt in it) and how it reads the API's answer — chunked or
// plain SSE, errors, refusals, cut-off and dropped streams — against a fake
// network (test_mocks/ai/).  ArduinoJson comes from the Arduino libraries
// folder.
//
// Run from Terminal — NOT through the Arduino IDE (it writes a test_claude_ai
// binary here — don't commit it):
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++17 -Wno-format -DUNIT_TEST -I test_mocks/ai -I test_mocks -I . \
//       -I ../magitrac_lib/src -I ~/Documents/Arduino/libraries/ArduinoJson/src \
//       test_claude_ai.cpp -o test_claude_ai && ./test_claude_ai
#ifndef ARDUINO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include "test_mocks/Arduino.h"

static uint32_t sNow = 0;
uint32_t millis() { return sNow += 1; }
HardwareSerial Serial;

std::string gMockSent, gMockReply, gMockCa, gMockKeyFile;
bool        gMockConnectOk = true, gMockKeyFileExists = true;
int         gMockWifiMode = 1, gMockWifiStatus = 3;
size_t      gMockInternalFree = 77 * 1024;
bool        gMockDnsOk = true;
int         gMockTlsAllocSwaps = 0;

#include "sd_mutex.h"
SemaphoreHandle_t sSdMutex = nullptr;
SdLock::SdLock() {}
SdLock::~SdLock() {}

const char* sampleManifestNameFor(uint8_t id) {
    if (id == 3) return "kick.wav";
    if (id == 9) return "caf\xe9 vox.wav";      // Latin-1 byte: must not break the JSON
    return nullptr;
}

#include "TrackerData.cpp"
#include "NoteGrid.cpp"
#include "song_text.cpp"
#include "claude_ai.cpp"

static int sPass = 0, sFail = 0;
static void check(bool ok, const char* what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    ok ? sPass++ : sFail++;
}

// ── A fake API ───────────────────────────────────────────────────────────────

static std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n"; else if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
static std::string ev(const char* type, const std::string& data) {
    return std::string("event: ") + type + "\ndata: " + data + "\n\n";
}
// A whole streamed answer whose text is `text`, sent in deltas of `piece`.
static std::string stream(const std::string& text, const char* stop = "end_turn",
                          bool withStop = true, size_t piece = 37) {
    std::string s;
    s += ev("message_start", "{\"type\":\"message_start\",\"message\":{\"model\":\"claude-opus-5-5\","
                             "\"usage\":{\"input_tokens\":4200,\"cache_read_input_tokens\":3000}}}");
    s += ev("content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
    s += ev("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"\"}}");
    s += ev("ping", "{\"type\":\"ping\"}");
    s += ev("content_block_stop", "{\"type\":\"content_block_stop\",\"index\":0}");
    s += ev("content_block_start", "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    for (size_t i = 0; i < text.size(); i += piece)
        s += ev("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":" +
                jstr(text.substr(i, piece)) + "}}");
    s += ev("content_block_stop", "{\"type\":\"content_block_stop\",\"index\":1}");
    s += ev("message_delta", std::string("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"") + stop +
                             "\"},\"usage\":{\"output_tokens\":1234}}");
    if (withStop) s += ev("message_stop", "{\"type\":\"message_stop\"}");
    return s;
}
static std::string chunked(const std::string& body, size_t n) {
    std::string o;
    for (size_t i = 0; i < body.size(); i += n) {
        std::string part = body.substr(i, n);
        char h[16]; snprintf(h, sizeof(h), "%zx", part.size());
        o += std::string(h) + "\r\n" + part + "\r\n";
    }
    return o + "0\r\n\r\n";
}
static std::string http200(const std::string& body, bool chunk) {
    if (chunk) return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n" + chunked(body, 113);
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n" + body;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static Song* makeSong() {
    Song* s = (Song*)calloc(1, sizeof(Song));
    initSong(s);
    strcpy(s->name, "Old song");
    s->columns[1] = ColumnSettings{ 1, 0, 0, 100, 0, 0, 0, "Piano" };
    NoteGrid g(s->notePool, &s->noteFreeHead, &s->patterns[0].noteHead);
    Note n = { makeNote(0, 4), VEL_DEFAULT, 0, 0 };
    g.set(0, 1, n);
    return s;
}

static const char* ANSWER =
    "MAGITRAC SONG 1\n"
    "SUMMARY A happy two-block song in G at 120 BPM.\n"
    "SONG name=\"Happy G\" bpm=120 minbpm=90 maxbpm=150 speed=6 start=0\n"
    "COL 1 ch=1 bank=0 prog=0 vol=100 tr=0 mute=0 tune=0 name=\"Piano\"\n"
    "BLOCK 0 len=16 name=\"Verse\" ref=G keymode=SAME end=NEXT:1\n"
    "N 0 0 0 G-3 - WAIT\n"
    "N 0 0 1 G-4 100 -\n"
    "N 0 4 1 B-4 - -\n"
    "N 0 8 1 D-5 - -\n"
    "BLOCK 1 len=16 name=\"Chorus\" ref=G keymode=SAME end=GOTO:0\n"
    "N 1 0 1 C-5 - -\n"
    "END\n";

struct Outcome { bool started; bool ok; std::string reply; std::string err; };

static Outcome run(const std::string& reply, const char* prompt = "Create a happy song in G") {
    gMockSent.clear();
    gMockReply = reply;
    Song* s = makeSong();
    Outcome o = { false, false, "", "" };
    char err[AI_TEXT_LEN] = "";
    o.started = aiBegin(s, prompt, err, sizeof(err));
    if (!o.started) { o.err = err; free(s); return o; }
    const char* r; size_t len;
    if (aiTakeReply(o.ok, r, len, err, sizeof(err))) {
        o.reply.assign(r, len);
        o.err = err;
    } else {
        o.err = "(no result)";
    }
    aiRelease();
    free(s);
    return o;
}

// ── Tests ────────────────────────────────────────────────────────────────────

static void testRequest() {
    printf("the request\n");
    gMockKeyFile = "# my key\nsk-ant-test-123\r\neffort=high\n";
    Outcome o = run(http200(stream(ANSWER), true));
    check(o.started && o.ok, "runs to the end");

    size_t split = gMockSent.find("\r\n\r\n");
    std::string head = gMockSent.substr(0, split), body = gMockSent.substr(split + 4);
    check(head.find("POST /v1/messages HTTP/1.1") == 0, "POST /v1/messages");
    check(head.find("x-api-key: sk-ant-test-123\r\n") != std::string::npos, "key from the file (comment + CR skipped)");
    check(head.find("anthropic-version: 2026-10-21") != std::string::npos, "API version header");
    check(head.find("anthropic-beta: server-side-fallback-2026-07-01") != std::string::npos, "fallback beta header");
    char cl[40]; snprintf(cl, sizeof(cl), "content-length: %zu\r\n", body.size());
    check(head.find(cl) != std::string::npos, "content-length matches the body");
    check(gMockCa.find("BEGIN CERTIFICATE") != std::string::npos, "root CAs set");

    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body);
    check(!e, "the body is valid JSON");
    if (e) { printf("    %s\n%.400s\n", e.c_str(), body.c_str()); return; }
    check(!strcmp(doc["model"] | "", "claude-opus-5-5"), "model");
    check(doc["stream"] == true && doc["max_tokens"] == 64000, "stream + max_tokens");
    check(!strcmp(doc["output_config"]["effort"] | "", "high"), "effort from the file");
    check(!strcmp(doc["fallbacks"] | "", "default"), "fallbacks: default");
    check(doc["thinking"].isNull() && doc["temperature"].isNull(), "no thinking/temperature params");
    const char* sys = doc["system"][0]["text"] | "";
    check(strstr(sys, "MAGITRAC SONG 1") && strstr(sys, "WAIT"), "system prompt describes the format");
    check(!strcmp(doc["system"][0]["cache_control"]["type"] | "", "ephemeral"), "system prompt cached");
    const char* user = doc["messages"][0]["content"] | "";
    check(!strcmp(doc["messages"][0]["role"] | "", "user"), "one user message");
    check(strstr(user, "The performer's request: Create a happy song in G") != nullptr, "prompt in the message");
    check(strstr(user, "SAMPLE 3 \"kick.wav\"") && strstr(user, "SAMPLE 9 \"caf? vox.wav\""),
          "sample list, stray byte made safe");
    check(strstr(user, "SONG name=\"Old song\"") && strstr(user, "N 0 0 1 C-4 - -") && strstr(user, "\nEND\n"),
          "the current song as text");
    check(sKey[0] == '\0', "key wiped after the request");
    check(gMockTlsAllocSwaps == 2, "TLS pointed at PSRAM for the request, then back");

    // The answer parses into the song the fake API wrote.
    Song* base = makeSong();
    Song* out = (Song*)calloc(1, sizeof(Song));
    SongTextResult r;
    check(o.reply == ANSWER, "reply text reassembled exactly from the deltas");
    check(songFromText(o.reply.c_str(), o.reply.size(), base, out, r) && r.notes == 5 && out->numPatterns == 2,
          "reply parses: 2 blocks, 5 notes");
    check(!strcmp(r.summary, "A happy two-block song in G at 120 BPM."), "summary");
    free(base); free(out);
}

static void testStreams() {
    printf("reading answers\n");
    gMockKeyFile = "sk-ant-x\n";
    Outcome o = run(http200(stream(ANSWER), false));
    check(o.ok && o.reply == ANSWER, "plain (not chunked) stream");
    o = run(http200(stream(ANSWER, "end_turn", true, 1), true));
    check(o.ok && o.reply == ANSWER, "one character per delta");
    std::string uni = "MAGITRAC SONG 1\nSUMMARY Caf\xc3\xa9 \"quoted\" \\ back\nEND\n";
    o = run(http200(stream(uni), true));
    check(o.ok && o.reply == uni, "escapes and UTF-8 in the text survive");

    o = run(http200(stream(ANSWER, "max_tokens"), true));
    check(!o.ok && o.err.find("cut off") != std::string::npos, "max_tokens -> cut off");
    o = run(http200(stream("", "refusal"), true));
    check(!o.ok && o.err.find("declined") != std::string::npos, "refusal -> declined");
    std::string cut = http200(stream(ANSWER, "end_turn", false), false);
    cut = cut.substr(0, cut.size() / 2);
    o = run(cut);
    check(!o.ok && o.err.find("dropped") != std::string::npos, "stream cut half-way -> dropped");
    printf("    (%s)\n", o.err.c_str());
    o = run(http200(ev("error", "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"), true));
    check(!o.ok && o.err == "API error: Overloaded", "an error event mid-stream");
}

static void testErrors() {
    printf("errors\n");
    gMockKeyFile = "sk-ant-x\n";
    std::string e401 = "HTTP/1.1 401 Unauthorized\r\ncontent-type: application/json\r\n\r\n"
                       "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}";
    Outcome o = run(e401);
    check(!o.ok && o.err == "The API key was refused (401)", "401");
    std::string e400 = "HTTP/1.1 400 Bad Request\r\nTransfer-Encoding: chunked\r\n\r\n" +
        chunked("{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"max_tokens: too big\"}}", 20);
    o = run(e400);
    check(!o.ok && o.err == "API error 400: max_tokens: too big", "400 with a chunked JSON body");

    gMockConnectOk = false;
    o = run(http200(stream(ANSWER), true));
    check(o.started && !o.ok && o.err == "Secure connection to the API failed (-0x7780)", "TLS connect fails: code shown");
    gMockConnectOk = true;
    gMockDnsOk = false;
    o = run(http200(stream(ANSWER), true));
    check(o.started && !o.ok && o.err.find("Can't look up") == 0, "DNS fails");
    gMockDnsOk = true;

    gMockKeyFileExists = false;
    o = run("");
    check(!o.started && o.err.find("No API key") == 0, "no key file: refused up front");
    gMockKeyFileExists = true;
    gMockKeyFile = "model=claude-opus-5-5\n";
    o = run("");
    check(!o.started && o.err.find("has no API key") != std::string::npos, "key file without a key");
    gMockKeyFile = "sk-ant-x\n";

    gMockWifiMode = 2;   // softAP only (SERVER_AP)
    o = run("");
    check(!o.started && o.err.find("external WiFi") != std::string::npos, "not on an external network");
    gMockWifiMode = 1;

    gMockInternalFree = 20 * 1024;
    o = run("");
    check(!o.started && o.err.find("Not enough memory") == 0, "too little internal RAM");
    gMockInternalFree = 77 * 1024;

    o = run("", "");
    check(!o.started, "empty prompt refused");
    check(!aiBusy(), "never left busy");
}

// The prompt's SYNTH examples must be the presets the device has, written
// exactly as song_text.cpp writes them.
static void testPromptPresets() {
    printf("Prompt presets\n");
    for (int k = 0; k < ORGAN_SYNTH_PRESET_N; k++) {
        OrganPatch op = {};
        organPatchSetVoice(op, ORGAN_VOICE_SYNTH);
        for (int i = 0; i < ORGAN_SYNTH_PARAMS; i++) organSynthParam(op, i) = ORGAN_SYNTH_PRESETS[k].p[i];
        static char buf[512];
        TextBuf t = { buf, sizeof(buf), 0, false };
        writeOrgan(op, t);
        const char* line = strstr(buf, "\nSYNTH ") + 7;
        std::string want = std::string(ORGAN_SYNTH_PRESETS[k].name);
        want.resize(8, ' ');
        want += std::string(line, strcspn(line, "\n"));
        char what[64];
        snprintf(what, sizeof(what), "%s matches the device", ORGAN_SYNTH_PRESETS[k].name);
        check(strstr(AI_SYSTEM, want.c_str()) != nullptr, what);
    }
}

int main() {
    testPromptPresets();
    testRequest();
    testStreams();
    testErrors();
    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif
