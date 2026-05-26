#include "BackupRestorePage.h"
#include "MagiLink.h"
#include "MagiMsg.h"
#include <SD.h>
#include <string.h>
#include <stdio.h>

// ── REPRO: set to 1 to discard incoming backup-file bytes instead of writing
// them to SD.  Lets us test whether the magitrac wedge is caused by SD-write
// contention with the WiFi/TCP task during sustained inbound streaming.
// All other code paths (WiFi rx, _songBuf fill, BackupState progression,
// EPD repaint, request next file, 80 ms pacing) are unchanged.
// Revert to 0 when done.
#define REPRO_SKIP_SD_WRITE 1

// ── REPRO: set to 1 to suppress the per-file EPD progress repaint during
// backup.  Testing the hypothesis that TPS65185 rail-cycle EMI / current
// spikes during the panel paint are corrupting nearby WiFi packets and
// causing TCP teardowns mid-backup.  The final BACKUP_DONE paint is
// unaffected.  Revert to 0 when done.
#define REPRO_SKIP_EPD_PAINT 1

extern ServerPairing gServerPairing;

// ── Layout constants ─────────────────────────────────────────────────────────
static const int BR_HDR_H    = 50;
static const int BR_HOME_X   = 830;
static const int BR_HOME_W   = 130;

// Menu buttons
static const int BR_BTN_W    = 300;
static const int BR_BTN_H    = 120;
static const int BR_BTN_Y    = 180;
static const int BR_BTN_GAP  = 60;
static const int BR_BTN1_X   = (960 - 2 * BR_BTN_W - BR_BTN_GAP) / 2;       // = 150
static const int BR_BTN2_X   = BR_BTN1_X + BR_BTN_W + BR_BTN_GAP;           // = 510

// Back button (used in several states)
static const int BR_BACK_W   = 200;
static const int BR_BACK_H   = 60;
static const int BR_BACK_X   = (960 - BR_BACK_W) / 2;
static const int BR_BACK_Y   = 460;

// List items
static const int BR_LIST_Y   = 100;
static const int BR_LIST_H   = 50;
static const int BR_LIST_W   = 440;
static const int BR_LIST_X0  = 20;
static const int BR_LIST_X1  = 500;

// Restore All button
static const int BR_RALL_X   = 20;
static const int BR_RALL_Y   = 60;
static const int BR_RALL_W   = 200;
static const int BR_RALL_H   = 35;

// Nav buttons
static const int BR_NAV_Y    = 60;
static const int BR_NAV_W    = 100;
static const int BR_NAV_H    = 35;
static const int BR_PREV_X   = 700;
static const int BR_NEXT_X   = 820;

// Progress bar
static const int BR_PB_X     = 130;
static const int BR_PB_Y     = 300;
static const int BR_PB_W     = 700;
static const int BR_PB_H     = 40;

// Cancel button — shown only during BACKUP_PROGRESS.
static const int BR_CANCEL_W = 200;
static const int BR_CANCEL_H = 60;
static const int BR_CANCEL_X = (960 - BR_CANCEL_W) / 2;
static const int BR_CANCEL_Y = 400;

// ── Constructor ───────────────────────────────────────────────────────────────

BackupRestorePage::BackupRestorePage(EPD_PainterAdafruit& display,
                                     GT911_Lite& touch,
                                     I2C_BM8563* rtc)
    : _d(display)
    , _touch(touch)
    , _rtc(rtc)
    , _state(BRState::MENU)
    , _wasDown(false)
    , _bkFileTotal(0)
    , _bkFileCurrent(0)
    , _bkCancelled(false)
    , _rsFolderCount(0)
    , _rsFolderPage(0)
    , _rsFileTotal(0)
    , _rsFileCurrent(0)
    , _rsRestoreCount(0)
    , _rsCancelled(false)
{
    _bkFolder[0] = '\0';
    _rsSelectedFolder[0] = '\0';
    _errMsg[0] = '\0';
}

// ── open / draw ───────────────────────────────────────────────────────────────

void BackupRestorePage::open() {
    _state   = BRState::MENU;
    _wasDown = _touch.isTouched;
    _bkCancelled = false;
    _rsCancelled = false;
}

void BackupRestorePage::draw() {
    _d.fillScreen(COL_WHITE);
    switch (_state) {
        case BRState::MENU:                drawMenu();            break;
        case BRState::BACKUP_WAITING_LIST: drawBackupProgress();  break;
        case BRState::BACKUP_PROGRESS:     drawBackupProgress();  break;
        case BRState::BACKUP_DONE:         drawBackupDone();      break;
        case BRState::FOLDER_LIST:         drawFolderList();      break;
        case BRState::FILE_LIST:           drawFileList();        break;
        case BRState::RESTORE_PROGRESS:    drawRestoreProgress(); break;
        case BRState::RESTORE_DONE:        drawRestoreDone();     break;
        case BRState::ERROR_SCREEN:        drawError();           break;
    }
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

void BackupRestorePage::drawHeader(const char* title) {
    _d.fillRect(0, 0, 960, BR_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    int tw = (int)strlen(title) * 18;
    _d.setCursor((960 - tw) / 2, (BR_HDR_H - 24) / 2);
    _d.print(title);
    uiButton(_d, BR_HOME_X, 0, BR_HOME_W, BR_HDR_H, "HOME", COL_BLACK, COL_WHITE, 3);
}

void BackupRestorePage::drawMenu() {
    drawHeader("BACKUP / RESTORE");
    uiButton(_d, BR_BTN1_X, BR_BTN_Y, BR_BTN_W, BR_BTN_H, "BACKUP",  COL_WHITE, COL_BLACK, 3);
    uiButton(_d, BR_BTN2_X, BR_BTN_Y, BR_BTN_W, BR_BTN_H, "RESTORE", COL_WHITE, COL_BLACK, 3);
}

void BackupRestorePage::drawProgressBar(int x, int y, int w, int h,
                                         int current, int total) {
    _d.drawRect(x, y, w, h, COL_BLACK);
    if (total > 0) {
        int filled = (w - 4) * current / total;
        if (filled > 0)
            _d.fillRect(x + 2, y + 2, filled, h - 4, COL_BLACK);
    }
}

void BackupRestorePage::drawBackupProgress() {
    drawHeader("BACKUP");

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);

    if (_state == BRState::BACKUP_WAITING_LIST) {
        _d.setCursor(250, 200);
        _d.print("Requesting file list...");
    } else {
        // File name
        _d.setCursor(100, 180);
        _d.print("Backing up: ");
        if (_bkFileCurrent < _bkFileTotal)
            _d.print(_bkFileNames[_bkFileCurrent]);

        // Counter
        char buf[32];
        snprintf(buf, sizeof(buf), "File %d of %d", _bkFileCurrent + 1, _bkFileTotal);
        _d.setCursor(350, 240);
        _d.print(buf);

        // Progress bar
        drawProgressBar(BR_PB_X, BR_PB_Y, BR_PB_W, BR_PB_H,
                        _bkFileCurrent, _bkFileTotal);

        // Cancel button — visible during BACKUP_PROGRESS only.  Greyed
        // out once the user has tapped it (we still drain the wire).
        uiButton(_d, BR_CANCEL_X, BR_CANCEL_Y, BR_CANCEL_W, BR_CANCEL_H,
                 _bkCancelled ? "CANCELLING..." : "CANCEL",
                 COL_WHITE, COL_BLACK, 3);
    }
}

void BackupRestorePage::drawBackupDone() {
    drawHeader("BACKUP");

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(300, 180);
    _d.print(_bkCancelled ? "Backup cancelled" : "Backup complete!");

    char buf[64];
    if (_bkCancelled) {
        snprintf(buf, sizeof(buf), "%d of %d files saved",
                 _bkFilesSaved, _bkFileTotal);
    } else {
        snprintf(buf, sizeof(buf), "%d files saved", _bkFilesSaved);
    }
    _d.setCursor(330, 240);
    _d.print(buf);

    uiButton(_d, BR_BACK_X, BR_BACK_Y, BR_BACK_W, BR_BACK_H, "OK", COL_WHITE, COL_BLACK, 3);
}

void BackupRestorePage::drawFolderList() {
    drawHeader("SELECT BACKUP");

    // Nav
    uiButton(_d, 20, BR_NAV_Y, 100, BR_NAV_H, "BACK", COL_WHITE, COL_BLACK, 2);
    if (_rsFolderCount > BR_LIST_PER_PAGE) {
        uiButton(_d, BR_PREV_X, BR_NAV_Y, BR_NAV_W, BR_NAV_H, "PREV", COL_WHITE, COL_BLACK, 2);
        uiButton(_d, BR_NEXT_X, BR_NAV_Y, BR_NAV_W, BR_NAV_H, "NEXT", COL_WHITE, COL_BLACK, 2);
    }

    // Folder list
    int start = _rsFolderPage * BR_LIST_PER_PAGE;
    int count = _rsFolderCount - start;
    if (count > BR_LIST_PER_PAGE) count = BR_LIST_PER_PAGE;

    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    for (int i = 0; i < count; i++) {
        int col = i / BR_LIST_ROWS;
        int row = i % BR_LIST_ROWS;
        int x = (col == 0) ? BR_LIST_X0 : BR_LIST_X1;
        int y = BR_LIST_Y + row * BR_LIST_H;
        _d.drawRect(x, y, BR_LIST_W, BR_LIST_H - 2, COL_BLACK);
        _d.setCursor(x + 10, y + (BR_LIST_H - 18) / 2);
        _d.print(_rsFolders[start + i]);
    }

    if (_rsFolderCount == 0) {
        _d.setTextSize(3);
        _d.setCursor(280, 250);
        _d.print("No backups found");
    }
}

void BackupRestorePage::drawFileList() {
    drawHeader("RESTORE FILES");

    // Buttons
    uiButton(_d, 20, BR_NAV_Y, 100, BR_NAV_H, "BACK", COL_WHITE, COL_BLACK, 2);
    uiButton(_d, BR_RALL_X + 120, BR_RALL_Y, BR_RALL_W, BR_RALL_H,
             "RESTORE ALL", COL_WHITE, COL_BLACK, 2);

    // File list
    _d.setTextSize(2);
    _d.setTextColor(COL_BLACK);
    int count = _rsFileTotal;
    if (count > BR_LIST_PER_PAGE) count = BR_LIST_PER_PAGE;

    for (int i = 0; i < count; i++) {
        int col = i / BR_LIST_ROWS;
        int row = i % BR_LIST_ROWS;
        int x = (col == 0) ? BR_LIST_X0 : BR_LIST_X1;
        int y = BR_LIST_Y + row * BR_LIST_H;
        _d.drawRect(x, y, BR_LIST_W, BR_LIST_H - 2, COL_BLACK);
        _d.setCursor(x + 10, y + (BR_LIST_H - 18) / 2);
        _d.print(_rsFileNames[i]);
    }
}

void BackupRestorePage::drawRestoreProgress() {
    drawHeader("RESTORE");

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);

    _d.setCursor(100, 180);
    _d.print("Restoring: ");
    if (_rsFileCurrent < _rsRestoreCount)
        _d.print(_rsFileNames[_rsRestoreList[_rsFileCurrent]]);

    char buf[32];
    snprintf(buf, sizeof(buf), "File %d of %d", _rsFileCurrent + 1, _rsRestoreCount);
    _d.setCursor(350, 240);
    _d.print(buf);

    drawProgressBar(BR_PB_X, BR_PB_Y, BR_PB_W, BR_PB_H,
                    _rsFileCurrent, _rsRestoreCount);
}

void BackupRestorePage::drawRestoreDone() {
    drawHeader("RESTORE");

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(280, 200);
    _d.print("Restore complete!");

    uiButton(_d, BR_BACK_X, BR_BACK_Y, BR_BACK_W, BR_BACK_H, "OK", COL_WHITE, COL_BLACK, 3);
}

void BackupRestorePage::drawError() {
    drawHeader("ERROR");

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    _d.setCursor(100, 200);
    _d.print(_errMsg);

    uiButton(_d, BR_BACK_X, BR_BACK_Y, BR_BACK_W, BR_BACK_H, "OK", COL_WHITE, COL_BLACK, 3);
}

// ── Backup logic ─────────────────────────────────────────────────────────────

void BackupRestorePage::startBackup() {
    // Real backup over MagiLink.  Synchronous — UI freezes for the
    // duration (~10-20 s for a typical 28-file backup).  Pattern:
    //   acquire mutex
    //   send MSG_START_BACKUP
    //   loop read(): header → bodies (write each to SD) → ... → end
    //   release mutex

    if (!gMagiLink.isConnected()) {
        strncpy(_errMsg, "Not connected (link)", sizeof(_errMsg));
        _state = BRState::ERROR_SCREEN;
        draw(); _d.paintLater();
        return;
    }

    // Create the destination folder with a timestamp.
    if (_rtc) {
        I2C_BM8563_TimeTypeDef t; I2C_BM8563_DateTypeDef d;
        _rtc->getTime(&t); _rtc->getDate(&d);
        int yr = d.year; if (yr > 2000) yr -= 2000;
        snprintf(_bkFolder, sizeof(_bkFolder),
                 "%s/bk_%02d%02d%02d_%02d%02d%02d",
                 BR_BACKUPS_DIR, yr, d.month, d.date,
                 t.hours, t.minutes, t.seconds);
    } else {
        snprintf(_bkFolder, sizeof(_bkFolder),
                 "%s/bk_%lu", BR_BACKUPS_DIR, millis());
    }
    if (!SD.exists(BR_BACKUPS_DIR)) SD.mkdir(BR_BACKUPS_DIR);
    SD.mkdir(_bkFolder);
    Serial.printf("[BK] folder=%s\n", _bkFolder);

    _bkFileTotal   = 0;
    _bkFileCurrent = 0;
    _bkCancelled   = false;
    _bkFilesSaved  = 0;

    // Show "backup in progress" — paintLater so the EPD work happens on
    // its own task and we proceed to the transaction immediately.
    _state = BRState::BACKUP_PROGRESS;
    draw(); _d.paintLater();

    gMagiLink.acquireMutex();
    MsgStartBackup req;
    if (!gMagiLink.send(&req, sizeof(req))) {
        Serial.println("[BK] send MSG_START_BACKUP failed");
        gMagiLink.releaseMutex();
        strncpy(_errMsg, "Backup send failed", sizeof(_errMsg));
        _state = BRState::ERROR_SCREEN;
        draw(); _d.paintLater();
        return;
    }

    // Receive loop: header → bodies → header → bodies → ... → end_of_data.
    File     curFile;
    uint32_t curBytesWritten = 0;
    uint32_t curFileSize     = 0;
    bool     done            = false;
    bool     readOk          = true;
    bool     touchWasDown    = _touch.isTouched;
    bool     cancelRepainted = false;

    while (!done) {
        // Poll touch for the Cancel button.  Edge-detect (down on this
        // tick + up previously) so a held finger only fires once.  Even
        // if cancelled, we keep looping until END_OF_DATA — otherwise
        // remaining body bytes desync the wire.
        if (!_bkCancelled && _touch.read()) {
            bool down = _touch.isTouched;
            if (down && !touchWasDown) {
                int sx, sy;
                rawToScreen(_touch.x, _touch.y, sx, sy);
                if (sx >= BR_CANCEL_X && sx < BR_CANCEL_X + BR_CANCEL_W &&
                    sy >= BR_CANCEL_Y && sy < BR_CANCEL_Y + BR_CANCEL_H) {
                    _bkCancelled = true;
                    Serial.println("[BK] CANCEL tapped — draining remaining stream");
                }
            }
            touchWasDown = down;
        }

        // Repaint once on the cancel transition so the button label
        // changes to "CANCELLING..." — confirmation to the user.
        if (_bkCancelled && !cancelRepainted) {
            cancelRepainted = true;
            draw(); _d.paintLater();
        }

        const uint8_t* m = gMagiLink.read();
        if (!m) {
            Serial.println("[BK] read returned nullptr (disconnect)");
            readOk = false;
            break;
        }

        uint8_t id = m[0];

        // IMPORTANT: when _bkCancelled is set we still drain every
        // message until END_OF_DATA so the wire doesn't desync.  We just
        // skip the SD writes / UI updates.
        if (id == MSG_BACKUP_HEADER) {
            const MsgBackupHeader* h = (const MsgBackupHeader*)m;

            // drawBackupProgress + the existing UI expect _bkFileCurrent
            // as a 0-based index; h->file_index is 1-based.  Translate.
            _bkFileCurrent  = (int)h->file_index - 1;
            _bkFileTotal    = h->file_total;
            curBytesWritten = 0;
            curFileSize     = h->file_size;

            char nameStr[25] = {};
            strncpy(nameStr, h->filename, 24);

            Serial.printf("[BK] header file=%u/%u name='%s' size=%u%s\n",
                          (unsigned)h->file_index, (unsigned)h->file_total,
                          nameStr, (unsigned)h->file_size,
                          _bkCancelled ? "  (skipped, cancelled)" : "");

            if (!_bkCancelled) {
                if (curFile) curFile.close();

                // Populate _bkFileNames so drawBackupProgress can display
                // the currently-streaming file's name.
                if (_bkFileCurrent >= 0 && _bkFileCurrent < BR_MAX_FILES) {
                    memset(_bkFileNames[_bkFileCurrent], 0, SRV_FNAME_MAX);
                    strncpy(_bkFileNames[_bkFileCurrent], nameStr,
                            SRV_FNAME_MAX - 1);
                }

                char path[80];
                snprintf(path, sizeof(path), "%s/%s", _bkFolder, nameStr);
                if (SD.exists(path)) SD.remove(path);
                curFile = SD.open(path, FILE_WRITE);
                if (!curFile) {
                    Serial.printf("[BK] SD open failed '%s'\n", path);
                }

                // Per-file UI update — paintLater so the EPD repaint
                // runs on its own task and doesn't block the body stream.
                draw(); _d.paintLater();
            }
        }
        else if (id == MSG_BACKUP_BODY) {
            const MsgBackupBody* b = (const MsgBackupBody*)m;
            uint16_t n = b->data_len;
            if (n > sizeof(b->data)) n = sizeof(b->data);
            if (!_bkCancelled) {
                if (curFile) curFile.write(b->data, n);
                curBytesWritten += n;
                if (curBytesWritten >= curFileSize) {
                    _bkFilesSaved++;
                    Serial.printf("[BK] file %u/%u complete (%u bytes) saved=%d\n",
                                  (unsigned)_bkFileCurrent, (unsigned)_bkFileTotal,
                                  (unsigned)curBytesWritten, _bkFilesSaved);
                }
            }
            // else: bytes already off the wire (gMagiLink.read), dropped here
        }
        else if (id == MSG_END_OF_DATA) {
            Serial.printf("[BK] END_OF_DATA — backup %s\n",
                          _bkCancelled ? "cancelled (drained)" : "complete");
            done = true;
        }
        else {
            Serial.printf("[BK] unexpected id=0x%02X — ignoring\n", id);
        }
    }

    if (curFile) curFile.close();
    gMagiLink.releaseMutex();

    if (readOk) {
        _state = BRState::BACKUP_DONE;
    } else {
        strncpy(_errMsg, "Connection lost mid-backup", sizeof(_errMsg));
        _state = BRState::ERROR_SCREEN;
    }
    draw(); _d.paintLater();
}

void BackupRestorePage::backupTick() {
    BackupState bs = gServerPairing.backupState();

    if (_state == BRState::BACKUP_WAITING_LIST) {
        if (bs == BackupState::FILE_LIST_READY) {
            // Single-blob list — all entries arrived in one frame.
            int count = gServerPairing.backupFileCount();
            for (int i = 0; i < count && _bkFileTotal < BR_MAX_FILES; i++) {
                strncpy(_bkFileNames[_bkFileTotal],
                        gServerPairing.backupFileName(i),
                        SRV_FNAME_MAX - 1);
                _bkFileNames[_bkFileTotal][SRV_FNAME_MAX - 1] = '\0';
                _bkFileTotal++;
            }

            if (_bkFileTotal == 0) {
                strncpy(_errMsg, "No files on server", sizeof(_errMsg));
                _state = BRState::ERROR_SCREEN;
                draw(); _d.paint();
                return;
            }

            // Create backup folder with timestamp
            if (_rtc) {
                I2C_BM8563_TimeTypeDef t;
                I2C_BM8563_DateTypeDef d;
                _rtc->getTime(&t);
                _rtc->getDate(&d);
                int yr = d.year;
                if (yr > 2000) yr -= 2000;
                snprintf(_bkFolder, sizeof(_bkFolder),
                         "%s/bk_%02d%02d%02d_%02d%02d%02d",
                         BR_BACKUPS_DIR, yr, d.month, d.date,
                         t.hours, t.minutes, t.seconds);
            } else {
                snprintf(_bkFolder, sizeof(_bkFolder),
                         "%s/bk_%lu", BR_BACKUPS_DIR, millis());
            }
            if (!SD.exists(BR_BACKUPS_DIR)) SD.mkdir(BR_BACKUPS_DIR);
            SD.mkdir(_bkFolder);

            // Start downloading files
            _bkFileCurrent = 0;
            _state = BRState::BACKUP_PROGRESS;
            gServerPairing.resetBackup();
            gServerPairing.requestBackupFile(_bkFileNames[0]);

#if REPRO_SKIP_EPD_PAINT
            Serial.println("[BK][REPRO] skip-paint after file-list received");
#else
            draw(); _d.paint();
#endif
        }
        return;
    }

    if (_state == BRState::BACKUP_PROGRESS) {
        if (bs == BackupState::FILE_RECEIVED) {
            // Write received file to SD
            char path[80];
            snprintf(path, sizeof(path), "%s/%s",
                     _bkFolder, _bkFileNames[_bkFileCurrent]);
#if REPRO_SKIP_SD_WRITE
            Serial.printf("[BK][REPRO] skip-SD '%s' %u bytes  heap=%u  psram=%u\n",
                          path,
                          gServerPairing.receivedFileLen(),
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)ESP.getFreePsram());
#else
            if (SD.exists(path)) SD.remove(path);
            File f = SD.open(path, FILE_WRITE);
            if (f) {
                f.write(gServerPairing.receivedFileData(),
                        gServerPairing.receivedFileLen());
                f.close();
                Serial.printf("[BK] saved '%s' %u bytes  heap=%u  psram=%u\n",
                              path,
                              gServerPairing.receivedFileLen(),
                              (unsigned)ESP.getFreeHeap(),
                              (unsigned)ESP.getFreePsram());
            }
#endif

            _bkFileCurrent++;
            if (_bkFileCurrent >= _bkFileTotal || _bkCancelled) {
                _state = BRState::BACKUP_DONE;
                gServerPairing.resetBackup();
                draw(); _d.paint();
                return;
            }

            // Brief pacing — spreads the WiFi-TX + EPD-repaint + SD-write peak
            // current draw across files so we don't brown-out the LilyGo.
            delay(80);

            // Request next file
            gServerPairing.resetBackup();
            gServerPairing.requestBackupFile(_bkFileNames[_bkFileCurrent]);

#if REPRO_SKIP_EPD_PAINT
            // Per-file progress paint suppressed for EMI/wedge investigation.
            // BACKUP_DONE paint above still runs.
            Serial.printf("[BK][REPRO] skip-paint after file %u/%u\n",
                          _bkFileCurrent, _bkFileTotal);
#else
            // Update progress display (partial)
            _d.fillRect(0, BR_HDR_H, 960, 540 - BR_HDR_H, COL_WHITE);
            drawBackupProgress();
            _d.paint();
#endif
        }
    }
}

// ── Restore logic ────────────────────────────────────────────────────────────

void BackupRestorePage::scanBackupFolders() {
    _rsFolderCount = 0;
    _rsFolderPage  = 0;
    if (!SD.exists(BR_BACKUPS_DIR)) return;
    File d = SD.open(BR_BACKUPS_DIR);
    if (!d || !d.isDirectory()) return;
    while (_rsFolderCount < BR_MAX_FOLDERS) {
        File entry = d.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) {
            const char* name = entry.name();
            const char* slash = strrchr(name, '/');
            if (slash) name = slash + 1;
            strncpy(_rsFolders[_rsFolderCount], name, BR_FOLDER_MAX - 1);
            _rsFolders[_rsFolderCount][BR_FOLDER_MAX - 1] = '\0';
            _rsFolderCount++;
        }
        entry.close();
    }
    d.close();
}

void BackupRestorePage::scanFolderFiles(const char* folder) {
    _rsFileTotal = 0;
    snprintf(_rsSelectedFolder, sizeof(_rsSelectedFolder),
             "%s/%s", BR_BACKUPS_DIR, folder);

    File d = SD.open(_rsSelectedFolder);
    if (!d || !d.isDirectory()) return;
    while (_rsFileTotal < BR_MAX_FILES) {
        File entry = d.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            const char* slash = strrchr(name, '/');
            if (slash) name = slash + 1;
            // Accept .mgt files
            int len = (int)strlen(name);
            if (len >= 4) {
                const char* ext = name + len - 4;
                if (ext[0] == '.' &&
                    (ext[1] == 'm' || ext[1] == 'M') &&
                    (ext[2] == 'g' || ext[2] == 'G') &&
                    (ext[3] == 't' || ext[3] == 'T')) {
                    strncpy(_rsFileNames[_rsFileTotal], name, SRV_FNAME_MAX - 1);
                    _rsFileNames[_rsFileTotal][SRV_FNAME_MAX - 1] = '\0';
                    _rsFileTotal++;
                }
            }
        }
        entry.close();
    }
    d.close();
}

void BackupRestorePage::startRestore(int fileIdx) {
    if (!gServerPairing.isPaired()) {
        strncpy(_errMsg, "Not connected to server", sizeof(_errMsg));
        _state = BRState::ERROR_SCREEN;
        draw(); _d.paint();
        return;
    }
    _rsRestoreCount = 0;
    _rsFileCurrent  = 0;
    _rsCancelled    = false;

    if (fileIdx < 0) {
        // Restore all
        for (int i = 0; i < _rsFileTotal && i < BR_MAX_FILES; i++)
            _rsRestoreList[_rsRestoreCount++] = i;
    } else {
        _rsRestoreList[0] = fileIdx;
        _rsRestoreCount = 1;
    }

    _state = BRState::RESTORE_PROGRESS;
    draw(); _d.paint();
    // restoreTick() is called from poll() each loop iteration
}

void BackupRestorePage::restoreTick() {
    if (_rsFileCurrent >= _rsRestoreCount || _rsCancelled) {
        _state = BRState::RESTORE_DONE;
        draw(); _d.paint();
        return;
    }

    int idx = _rsRestoreList[_rsFileCurrent];
    const char* name = _rsFileNames[idx];

    // Read file from client SD
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", _rsSelectedFolder, name);
    File f = SD.open(path);
    if (!f) {
        Serial.printf("[RS] open failed '%s'\n", path);
        _rsFileCurrent++;
        restoreTick();
        return;
    }
    uint32_t fileSize = (uint32_t)f.size();
    static uint8_t restoreBuf[SONG_TRANSFER_MAX];
    if (fileSize > SONG_TRANSFER_MAX) {
        f.close();
        _rsFileCurrent++;
        restoreTick();
        return;
    }
    uint32_t got = (uint32_t)f.read(restoreBuf, fileSize);
    f.close();
    if (got != fileSize) {
        _rsFileCurrent++;
        restoreTick();
        return;
    }

    bool isInstr = (strcmp(name, "instruments.mgt") == 0);
    gServerPairing.sendRestoreFile(name, isInstr, restoreBuf, fileSize);
    Serial.printf("[RS] sent '%s' %u bytes instr=%d\n", name, fileSize, isInstr);

    // Give server time to finalize (commandsTick writes to SD) before next file
    delay(100);

    _rsFileCurrent++;

    // Update display
    if (_rsFileCurrent >= _rsRestoreCount) {
        _state = BRState::RESTORE_DONE;
    }
    _d.fillRect(0, BR_HDR_H, 960, 540 - BR_HDR_H, COL_WHITE);
    if (_state == BRState::RESTORE_DONE)
        drawRestoreDone();
    else
        drawRestoreProgress();
    _d.paint();
}

// ── Poll ──────────────────────────────────────────────────────────────────────

bool BackupRestorePage::poll() {
    // Drive backup/restore state machines
    if (_state == BRState::BACKUP_WAITING_LIST || _state == BRState::BACKUP_PROGRESS)
        backupTick();
    if (_state == BRState::RESTORE_PROGRESS)
        restoreTick();

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // Only act on falling edge
    if (!down && _wasDown) {
        _wasDown = false;

        // HOME always closes
        if (hitHome(sx, sy)) return true;

        switch (_state) {
            case BRState::MENU:
                // BACKUP button
                if (sx >= BR_BTN1_X && sx < BR_BTN1_X + BR_BTN_W &&
                    sy >= BR_BTN_Y  && sy < BR_BTN_Y  + BR_BTN_H) {
                    startBackup();
                    return false;
                }
                // RESTORE button
                if (sx >= BR_BTN2_X && sx < BR_BTN2_X + BR_BTN_W &&
                    sy >= BR_BTN_Y  && sy < BR_BTN_Y  + BR_BTN_H) {
                    scanBackupFolders();
                    _state = BRState::FOLDER_LIST;
                    draw(); _d.paint();
                    return false;
                }
                break;

            case BRState::BACKUP_DONE:
            case BRState::RESTORE_DONE:
            case BRState::ERROR_SCREEN:
                // OK button
                if (sx >= BR_BACK_X && sx < BR_BACK_X + BR_BACK_W &&
                    sy >= BR_BACK_Y && sy < BR_BACK_Y + BR_BACK_H) {
                    _state = BRState::MENU;
                    draw(); _d.paint();
                }
                break;

            case BRState::FOLDER_LIST: {
                // BACK button
                if (sx >= 20 && sx < 120 && sy >= BR_NAV_Y && sy < BR_NAV_Y + BR_NAV_H) {
                    _state = BRState::MENU;
                    draw(); _d.paint();
                    return false;
                }
                // PREV/NEXT
                if (_rsFolderCount > BR_LIST_PER_PAGE) {
                    if (sx >= BR_PREV_X && sx < BR_PREV_X + BR_NAV_W &&
                        sy >= BR_NAV_Y && sy < BR_NAV_Y + BR_NAV_H) {
                        if (_rsFolderPage > 0) { _rsFolderPage--; draw(); _d.paint(); }
                        return false;
                    }
                    if (sx >= BR_NEXT_X && sx < BR_NEXT_X + BR_NAV_W &&
                        sy >= BR_NAV_Y && sy < BR_NAV_Y + BR_NAV_H) {
                        int maxPage = (_rsFolderCount - 1) / BR_LIST_PER_PAGE;
                        if (_rsFolderPage < maxPage) { _rsFolderPage++; draw(); _d.paint(); }
                        return false;
                    }
                }
                // Folder tap
                int start = _rsFolderPage * BR_LIST_PER_PAGE;
                int count = _rsFolderCount - start;
                if (count > BR_LIST_PER_PAGE) count = BR_LIST_PER_PAGE;
                for (int i = 0; i < count; i++) {
                    int col = i / BR_LIST_ROWS;
                    int row = i % BR_LIST_ROWS;
                    int lx = (col == 0) ? BR_LIST_X0 : BR_LIST_X1;
                    int ly = BR_LIST_Y + row * BR_LIST_H;
                    if (sx >= lx && sx < lx + BR_LIST_W &&
                        sy >= ly && sy < ly + BR_LIST_H) {
                        scanFolderFiles(_rsFolders[start + i]);
                        _state = BRState::FILE_LIST;
                        draw(); _d.paint();
                        return false;
                    }
                }
                break;
            }

            case BRState::FILE_LIST: {
                // BACK button
                if (sx >= 20 && sx < 120 && sy >= BR_NAV_Y && sy < BR_NAV_Y + BR_NAV_H) {
                    _state = BRState::FOLDER_LIST;
                    draw(); _d.paint();
                    return false;
                }
                // RESTORE ALL button
                if (sx >= BR_RALL_X + 120 && sx < BR_RALL_X + 120 + BR_RALL_W &&
                    sy >= BR_RALL_Y && sy < BR_RALL_Y + BR_RALL_H) {
                    startRestore(-1);
                    return false;
                }
                // Individual file tap
                int count = _rsFileTotal;
                if (count > BR_LIST_PER_PAGE) count = BR_LIST_PER_PAGE;
                for (int i = 0; i < count; i++) {
                    int col = i / BR_LIST_ROWS;
                    int row = i % BR_LIST_ROWS;
                    int lx = (col == 0) ? BR_LIST_X0 : BR_LIST_X1;
                    int ly = BR_LIST_Y + row * BR_LIST_H;
                    if (sx >= lx && sx < lx + BR_LIST_W &&
                        sy >= ly && sy < ly + BR_LIST_H) {
                        startRestore(i);
                        return false;
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    if (down && !_wasDown) _wasDown = true;
    return false;
}

// ── Hit tests ─────────────────────────────────────────────────────────────────

bool BackupRestorePage::hitHome(int sx, int sy) const {
    return (sx >= BR_HOME_X && sx < BR_HOME_X + BR_HOME_W &&
            sy >= 0 && sy < BR_HDR_H);
}

void BackupRestorePage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}
