// psram_test.ino — standalone SPI RAM (PSRAM) health check for M5Stack CoreS3.
//
// Written 2026-07-24 to answer one question before a gig: is the CoreS3's
// SPI RAM actually working, or is it why magitrac_server_s3 red-screens the
// moment a song is loaded?  The server allocates its ~37 KB active-song
// buffer from PSRAM and writes a Song straight through that pointer, so a
// failure there faults on the first load while menus and lists still work.
//
// Flash this INSTEAD of the server (same board settings), then read the
// verdict off the LCD — no laptop needed at the venue.  Serial at 115200
// carries the detail.
//
//   Tools > Board            : M5CoreS3
//   Tools > PSRAM            : OPI PSRAM   <-- CoreS3 is ESP32-S3-WROOM-1 N16R8,
//                                             8 MB *octal* SPI RAM.  QSPI and
//                                             Disabled both read as "absent".
//   Tools > Partition Scheme : (anything)
//
// ── What it tests, and why each one earns its place ─────────────────────────
//
//   0. Discovery      IDF's view: total / free / largest block.  Total of 0
//                     means the chip never initialised — a config or solder
//                     problem, not a data problem.  Nothing else can run.
//
//   1. Word patterns  32-bit access over every allocatable chunk.  The ESP32-S3
//                     reaches PSRAM through a cache, and octal PSRAM moves whole
//                     cache lines — byte-only tests can pass while word or
//                     burst access fails.  Patterns cover stuck bits (0x00000000
//                     / 0xFFFFFFFF), adjacent-bit coupling (0xAAAA5555 and its
//                     inverse) and data-dependent faults (LCG pseudo-random).
//
//   2. Walking bits   One bit set, then one bit clear, marched through all 32
//                     positions of a word.  Isolates a single stuck or shorted
//                     data line, which the bulk patterns can mask.
//
//   3. Aliasing       Each word gets a value derived from its own address, and
//                     the whole span is verified only after every write is done.
//                     This is the test that matters most here: an address-decode
//                     fault makes two addresses share one cell, so writing B
//                     silently corrupts A.  That produces exactly the reported
//                     symptom — some things work, others crash — and every
//                     write-then-immediately-read test in the world will pass.
//
//   4. Server repro   The exact 40 KB allocation magitrac_server_s3 makes, so
//                     pass/fail maps directly onto "will it load a song".
//
//   5. Soak (loop)    Repeats the aliasing test forever, reporting cumulative
//                     errors.  A chip that is marginal rather than dead often
//                     passes cold and fails once it warms up — which is also
//                     what "worked at home, died at the gig" looks like.
//
// Coverage note: PSRAM is exercised through as many chunks as the allocator
// will give out, so the test covers essentially all *free* PSRAM rather than
// just the single largest block.  Whatever the firmware has already claimed at
// boot is not testable from here without disturbing it.

#include <M5Unified.h>
#include <esp_heap_caps.h>

// Must match SRV_SONG_XFER_MAX in magitrac_server_s3/commands_server.ino
// (sizeof(SongFileHeader) + sizeof(Song) + 64).  Rounded up and hard-coded so
// this sketch stays standalone — it must not pull in magitrac_lib.
static const size_t SERVER_SONG_BUF = 40 * 1024;

// Up to this many chunks when carving up free PSRAM.
#define MAX_CHUNKS 16

static uint32_t sErrors  = 0;   // this run
static uint32_t sSoakErr = 0;   // cumulative across soak passes
static uint32_t sSoakRun = 0;
static bool     sHavePsram = false;

// Chunks we hold for the whole run, so the soak test has something to hammer.
static uint32_t* sChunk[MAX_CHUNKS];
static size_t    sChunkWords[MAX_CHUNKS];
static int       sNumChunks = 0;

// ── Screen ──────────────────────────────────────────────────────────────────
static int sLineY = 0;
static void line(uint16_t colour, const char* fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.println(buf);
    if (sLineY > 300) {                      // ran off the panel — recycle
        M5.Display.fillScreen(TFT_BLACK);
        sLineY = 0;
    }
    M5.Display.setTextColor(colour, TFT_BLACK);
    M5.Display.setCursor(4, sLineY);
    M5.Display.print(buf);
    sLineY += 11;
}

// Report at most this many bad addresses per pass — a dead chip would
// otherwise spool millions of lines out of the serial port.
static const int MAX_REPORT = 6;

// ── Pattern generators ──────────────────────────────────────────────────────
static uint32_t genZero (size_t)   { return 0x00000000u; }
static uint32_t genOnes (size_t)   { return 0xFFFFFFFFu; }
static uint32_t genAlt  (size_t i) { return (i & 1) ? 0x5555AAAAu : 0xAAAA5555u; }
static uint32_t genRand (size_t i) {
    uint32_t x = (uint32_t)i * 1664525u + 1013904223u;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12;
    return x;
}

// ── 1/2. Write-then-verify one pattern over one chunk ───────────────────────
static uint32_t passPattern(const char* name, uint32_t* p, size_t words,
                            uint32_t (*gen)(size_t)) {
    for (size_t i = 0; i < words; i++) p[i] = gen(i);
    uint32_t bad = 0;
    for (size_t i = 0; i < words; i++) {
        uint32_t want = gen(i), got = p[i];
        if (got != want) {
            if (bad < MAX_REPORT)
                Serial.printf("  [FAIL] %s @ %p (+0x%06x): wrote %08X read %08X "
                              "(diff %08X)\n", name, (void*)(p + i),
                              (unsigned)(i * 4), want, got, want ^ got);
            bad++;
        }
    }
    return bad;
}

// Walking-ones then walking-zeros across all 32 bit positions.
static uint32_t passWalking(uint32_t* p, size_t words) {
    uint32_t bad = 0;
    size_t n = words < 4096 ? words : 4096;   // a window is enough to find a
                                              // stuck data line
    for (int bit = 0; bit < 32; bit++) {
        uint32_t one = 1u << bit, zero = ~one;
        for (size_t i = 0; i < n; i++) p[i] = (i & 1) ? zero : one;
        for (size_t i = 0; i < n; i++) {
            uint32_t want = (i & 1) ? zero : one;
            if (p[i] != want) {
                if (bad < MAX_REPORT)
                    Serial.printf("  [FAIL] walk bit%d @ %p: wrote %08X read %08X\n",
                                  bit, (void*)(p + i), want, p[i]);
                bad++;
            }
        }
    }
    return bad;
}

// ── 3. Address aliasing across the WHOLE span ───────────────────────────────
// Write every word first, verify only afterwards.  If two addresses decode to
// the same cell, the later write clobbers the earlier one and the mismatch
// shows up here — where a write/read-back-immediately test sees nothing wrong.
// `salt` varies the mapping per soak pass so a fault can't hide behind a value
// that happens to match.
static uint32_t passAliasing(uint32_t salt) {
    for (int c = 0; c < sNumChunks; c++) {
        uint32_t* p = sChunk[c];
        for (size_t i = 0; i < sChunkWords[c]; i++)
            p[i] = ((uint32_t)(uintptr_t)(p + i)) ^ salt;
    }
    uint32_t bad = 0;
    for (int c = 0; c < sNumChunks; c++) {
        uint32_t* p = sChunk[c];
        for (size_t i = 0; i < sChunkWords[c]; i++) {
            uint32_t want = ((uint32_t)(uintptr_t)(p + i)) ^ salt;
            if (p[i] != want) {
                if (bad < MAX_REPORT)
                    Serial.printf("  [FAIL] alias @ %p: wrote %08X read %08X "
                                  "(looks like addr %08X)\n",
                                  (void*)(p + i), want, p[i], p[i] ^ salt);
                bad++;
            }
        }
    }
    return bad;
}

// ── Throughput ──────────────────────────────────────────────────────────────
// A chip that passes every pattern but runs far below spec is still a problem:
// the sample player streams out of PSRAM in real time.  Healthy octal PSRAM on
// the S3 reads in the tens of MB/s; single-MB/s figures suggest it fell back to
// a slower mode.
static void reportSpeed(uint32_t* p, size_t words) {
    volatile uint32_t sink = 0;
    uint32_t t0 = micros();
    for (size_t i = 0; i < words; i++) sink += p[i];
    uint32_t rd = micros() - t0;
    (void)sink;

    t0 = micros();
    for (size_t i = 0; i < words; i++) p[i] = (uint32_t)i;
    uint32_t wr = micros() - t0;

    float mb = (float)(words * 4) / (1024.0f * 1024.0f);
    if (rd) line(TFT_CYAN, "read  %.1f MB/s", mb / ((float)rd / 1e6f));
    if (wr) line(TFT_CYAN, "write %.1f MB/s", mb / ((float)wr / 1e6f));
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    delay(600);

    M5.Display.setRotation(0);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(1);

    line(TFT_WHITE, "SPI RAM TEST  CoreS3");
    sLineY += 4;

    // ── 0. Discovery ────────────────────────────────────────────────────────
    size_t psTotal   = ESP.getPsramSize();
    size_t psFree    = ESP.getFreePsram();
    size_t psLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    Serial.printf("[PSRAM] total=%u free=%u largest=%u\n",
                  (unsigned)psTotal, (unsigned)psFree, (unsigned)psLargest);
    Serial.printf("[HEAP ] internal free=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    line(TFT_WHITE, "total   %u KB", (unsigned)(psTotal / 1024));
    line(TFT_WHITE, "free    %u KB", (unsigned)(psFree / 1024));
    line(TFT_WHITE, "largest %u KB", (unsigned)(psLargest / 1024));
    sLineY += 4;

    if (psTotal == 0) {
        line(TFT_RED,    "NO SPI RAM DETECTED");
        line(TFT_YELLOW, "Tools>PSRAM must be");
        line(TFT_YELLOW, "'OPI PSRAM'  (CoreS3 is");
        line(TFT_YELLOW, "N16R8 = octal). QSPI or");
        line(TFT_YELLOW, "Disabled read as absent.");
        line(TFT_YELLOW, "If OPI is already set,");
        line(TFT_YELLOW, "the chip/joints are bad.");
        Serial.println("\n=== VERDICT: NO PSRAM — check Tools>PSRAM = OPI first ===");
        return;
    }
    sHavePsram = true;

    // ── Carve up free PSRAM ─────────────────────────────────────────────────
    // Repeatedly take the largest remaining block, so we end up covering
    // essentially all free PSRAM rather than one block's worth.
    size_t totalWords = 0;
    while (sNumChunks < MAX_CHUNKS) {
        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        if (big < 64 * 1024) break;                  // leave the crumbs
        size_t take = big - 16 * 1024;               // headroom for bookkeeping
        uint32_t* p = (uint32_t*)heap_caps_malloc(take, MALLOC_CAP_SPIRAM);
        if (!p) break;
        sChunk[sNumChunks]      = p;
        sChunkWords[sNumChunks] = take / 4;
        totalWords += take / 4;
        sNumChunks++;
    }

    if (sNumChunks == 0) {
        line(TFT_RED, "could not allocate ANY");
        line(TFT_RED, "PSRAM to test");
        Serial.println("\n=== VERDICT: PSRAM present but unallocatable ===");
        sErrors++;
    } else {
        line(TFT_WHITE, "testing %u KB in %d blk",
             (unsigned)(totalWords * 4 / 1024), sNumChunks);
        Serial.printf("[PSRAM] testing %u bytes across %d chunks\n",
                      (unsigned)(totalWords * 4), sNumChunks);

        // ── 1. Word patterns over every chunk ───────────────────────────────
        struct { const char* n; uint32_t (*g)(size_t); } pats[] = {
            { "zeros",  genZero }, { "ones",   genOnes },
            { "alt",    genAlt  }, { "random", genRand },
        };
        for (auto& pt : pats) {
            uint32_t bad = 0;
            for (int c = 0; c < sNumChunks; c++)
                bad += passPattern(pt.n, sChunk[c], sChunkWords[c], pt.g);
            sErrors += bad;
            if (bad) line(TFT_RED,   "  %-7s FAIL %u", pt.n, (unsigned)bad);
            else     line(TFT_GREEN, "  %-7s ok", pt.n);
        }

        // ── 2. Walking bits ─────────────────────────────────────────────────
        {
            uint32_t bad = passWalking(sChunk[0], sChunkWords[0]);
            sErrors += bad;
            if (bad) line(TFT_RED,   "  walking FAIL %u", (unsigned)bad);
            else     line(TFT_GREEN, "  walking ok");
        }

        // ── 3. Aliasing over the whole span ─────────────────────────────────
        {
            uint32_t bad = passAliasing(0xA5A5A5A5u);
            sErrors += bad;
            if (bad) line(TFT_RED,   "  alias   FAIL %u", (unsigned)bad);
            else     line(TFT_GREEN, "  alias   ok");
        }

        sLineY += 2;
        reportSpeed(sChunk[0], sChunkWords[0]);
    }

    // ── 4. Reproduce the server's own allocation ────────────────────────────
    sLineY += 4;
    line(TFT_WHITE, "server buf %u KB:", (unsigned)(SERVER_SONG_BUF / 1024));
    uint32_t* songBuf = (uint32_t*)heap_caps_malloc(SERVER_SONG_BUF, MALLOC_CAP_SPIRAM);
    if (!songBuf) {
        line(TFT_RED, "  PSRAM alloc FAILED");
        sErrors++;
        songBuf = (uint32_t*)malloc(SERVER_SONG_BUF);
        line(songBuf ? TFT_YELLOW : TFT_RED,
             songBuf ? "  internal fallback ok" : "  internal ALSO FAILED");
        if (!songBuf) sErrors++;
    } else {
        uint32_t bad = passPattern("songbuf", songBuf, SERVER_SONG_BUF / 4, genRand);
        sErrors += bad;
        if (bad) line(TFT_RED,   "  songbuf FAIL %u", (unsigned)bad);
        else     line(TFT_GREEN, "  songbuf ok");
    }
    free(songBuf);

    // ── Verdict ─────────────────────────────────────────────────────────────
    sLineY += 6;
    if (sErrors == 0) {
        line(TFT_GREEN, "SPI RAM OK");
        line(TFT_WHITE, "soak running...");
        Serial.println("\n=== VERDICT: PSRAM OK — look elsewhere for the crash ===");
        Serial.println("Leaving the soak test running; a marginal chip often");
        Serial.println("passes cold and fails once warm.  Watch for [SOAK] errors.");
    } else {
        line(TFT_RED, "SPI RAM BAD (%u)", (unsigned)sErrors);
        Serial.printf("\n=== VERDICT: PSRAM FAULTY — %u failures ===\n",
                      (unsigned)sErrors);
    }
}

void loop() {
    // ── 5. Soak ─────────────────────────────────────────────────────────────
    // Keep hammering the aliasing test — the one that catches decode faults —
    // with a different salt each pass, so thermal/marginal failures surface.
    if (!sHavePsram || sNumChunks == 0) { delay(1000); return; }

    sSoakRun++;
    uint32_t bad = passAliasing(0x5A5A0000u ^ (sSoakRun * 2654435761u));
    sSoakErr += bad;

    Serial.printf("[SOAK] pass %u: %u errors this pass, %u total\n",
                  (unsigned)sSoakRun, (unsigned)bad, (unsigned)sSoakErr);

    // Keep the running total on screen without redrawing the whole report.
    M5.Display.setTextColor(sSoakErr ? TFT_RED : TFT_GREEN, TFT_BLACK);
    M5.Display.setCursor(4, 300);
    M5.Display.printf("soak %u pass / %u err   ",
                      (unsigned)sSoakRun, (unsigned)sSoakErr);
}
