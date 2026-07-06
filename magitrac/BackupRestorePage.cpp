#include "BackupRestorePage.h"
#include "MagiLink.h"
#include "MagiMsg.h"
#include <SD.h>
#include <string.h>
#include <stdio.h>

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

// Folder list (single-column drag-scroll)
static const int BR_FL_Y           = 70;
static const int BR_FL_ROW_H       = 70;
static const int BR_FL_ROWS        = 6;
static const int BR_FL_X           = 20;
static const int BR_FL_W           = 920;
static const int BR_FL_DRAG_THRESH = 12;

// File list — same row metrics as folder list, but one fewer visible row
// to make space for the bottom-centred RESTORE ALL action button.
static const int BR_FILE_Y         = 70;
static const int BR_FILE_ROWS      = 5;

// Header BACK button — mirrors HOME on the left.
static const int BR_HBACK_X = 0;
static const int BR_HBACK_W = 130;

// Restore-confirm dialog
static const int BR_CONF_TITLE_Y = 200;
static const int BR_CONF_BODY_Y  = 290;
static const int BR_CONF_BTN_W   = 200;
static const int BR_CONF_BTN_H   = 70;
static const int BR_CONF_BTN_Y   = 410;
static const int BR_CONF_GAP     = 80;
static const int BR_CONF_YES_X   = (960 - 2 * BR_CONF_BTN_W - BR_CONF_GAP) / 2;
static const int BR_CONF_NO_X    = BR_CONF_YES_X + BR_CONF_BTN_W + BR_CONF_GAP;

// Restore All — bottom-centred primary action button on FILE_LIST.
static const int BR_RALL_W   = 280;
static const int BR_RALL_H   = 60;
static const int BR_RALL_X   = (960 - BR_RALL_W) / 2;
static const int BR_RALL_Y   = 460;


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

static const char* const BR_MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// Parse "bk_YYMMDD_HHMMSS" → "DD Mon YYYY  HH:MM:SS".  Returns false
// (and leaves `out` empty) if the name doesn't match.
static bool formatBackupTimestamp(const char* name, char* out, size_t outSize) {
    if (!name || !out || outSize == 0) return false;
    if (strncmp(name, "bk_", 3) != 0) { out[0] = '\0'; return false; }
    if (strlen(name) < 16) { out[0] = '\0'; return false; }
    static const uint8_t digitPositions[] = {3,4,5,6,7,8,10,11,12,13,14,15};
    for (size_t k = 0; k < sizeof(digitPositions); k++) {
        char c = name[digitPositions[k]];
        if (c < '0' || c > '9') { out[0] = '\0'; return false; }
    }
    if (name[9] != '_') { out[0] = '\0'; return false; }
    int yy = (name[3]-'0')*10 + (name[4]-'0');
    int mm = (name[5]-'0')*10 + (name[6]-'0');
    int dd = (name[7]-'0')*10 + (name[8]-'0');
    int hr = (name[10]-'0')*10 + (name[11]-'0');
    int mn = (name[12]-'0')*10 + (name[13]-'0');
    int sc = (name[14]-'0')*10 + (name[15]-'0');
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31 ||
        hr > 23 || mn > 59 || sc > 59) { out[0] = '\0'; return false; }
    snprintf(out, outSize, "%02d %s 20%02d  %02d:%02d:%02d",
             dd, BR_MONTHS[mm-1], yy, hr, mn, sc);
    return true;
}

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
    , _rsFolderScroll(0)
    , _rsDragStartY(0)
    , _rsDragStartScroll(0)
    , _rsDragMoved(false)
    , _rsFileTotal(0)
    , _rsFileCurrent(0)
    , _rsRestoreCount(0)
    , _rsCancelled(false)
    , _rsConfirmIdx(-1)
    , _rsFileScroll(0)
    , _rsFileDragStartY(0)
    , _rsFileDragStartScroll(0)
    , _rsFileDragMoved(false)
    , _rsFolderScanIdx(0)
    , _rsFolderListDirty(false)
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
        case BRState::BACKUP_PROGRESS:     drawBackupProgress();  break;
        case BRState::BACKUP_DONE:         drawBackupDone();      break;
        case BRState::FOLDER_LIST:         drawFolderList();      break;
        case BRState::FILE_LIST:           drawFileList();        break;
        case BRState::RESTORE_CONFIRM:     drawRestoreConfirm();  break;
        case BRState::RESTORE_PROGRESS:    drawRestoreProgress(); break;
        case BRState::RESTORE_DONE:        drawRestoreDone();     break;
        case BRState::ERROR_SCREEN:        drawError();           break;
    }
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

void BackupRestorePage::drawHeader(const char* title, bool withBack) {
    _d.fillRect(0, 0, 960, BR_HDR_H, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    int tw = (int)strlen(title) * 18;
    _d.setCursor((960 - tw) / 2, (BR_HDR_H - 24) / 2);
    _d.print(title);
    if (withBack) {
        uiButton(_d, BR_HBACK_X, 0, BR_HBACK_W, BR_HDR_H, "BACK",
                 COL_BLACK, COL_WHITE, 3);
    }
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

    {
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
    drawHeader("SELECT BACKUP", /*withBack=*/true);

    if (_rsFolderCount == 0) {
        _d.setTextSize(3);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(280, 250);
        _d.print("No backups found");
        return;
    }

    int start = _rsFolderScroll;
    int count = _rsFolderCount - start;
    if (count > BR_FL_ROWS) count = BR_FL_ROWS;

    _d.setTextColor(COL_BLACK);
    for (int i = 0; i < count; i++) {
        int idx = start + i;
        int y   = BR_FL_Y + i * BR_FL_ROW_H;
        _d.drawRect(BR_FL_X, y, BR_FL_W, BR_FL_ROW_H - 4, COL_BLACK);

        char dateBuf[40];
        if (!formatBackupTimestamp(_rsFolders[idx], dateBuf, sizeof(dateBuf))) {
            strncpy(dateBuf, _rsFolders[idx], sizeof(dateBuf) - 1);
            dateBuf[sizeof(dateBuf) - 1] = '\0';
        }
        _d.setTextSize(3);
        _d.setCursor(BR_FL_X + 16, y + (BR_FL_ROW_H - 4 - 24) / 2);
        _d.print(dateBuf);

        int songs = _rsFolderSongCount[idx];
        if (songs >= 0) {
            char countBuf[24];
            snprintf(countBuf, sizeof(countBuf), "%d song%s",
                     songs, songs == 1 ? "" : "s");
            int tw = (int)strlen(countBuf) * 18;
            _d.setCursor(BR_FL_X + BR_FL_W - 16 - tw,
                         y + (BR_FL_ROW_H - 4 - 24) / 2);
            _d.print(countBuf);
        }
    }

    // Scroll hint at the bottom (replaces the old PREV/NEXT row).
    if (_rsFolderCount > BR_FL_ROWS) {
        char hint[40];
        int firstVis = _rsFolderScroll + 1;
        int lastVis  = _rsFolderScroll + BR_FL_ROWS;
        if (lastVis > _rsFolderCount) lastVis = _rsFolderCount;
        snprintf(hint, sizeof(hint), "%d-%d of %d  (drag to scroll)",
                 firstVis, lastVis, _rsFolderCount);
        _d.setTextSize(2);
        int tw = (int)strlen(hint) * 12;
        _d.setCursor((960 - tw) / 2, BR_FL_Y + BR_FL_ROWS * BR_FL_ROW_H + 4);
        _d.print(hint);
    }
}

void BackupRestorePage::drawFileList() {
    drawHeader("RESTORE FILES", /*withBack=*/true);

    if (_rsFileTotal == 0) {
        _d.setTextSize(3);
        _d.setTextColor(COL_BLACK);
        _d.setCursor(330, 250);
        _d.print("No files found");
    } else {
        int start = _rsFileScroll;
        int count = _rsFileTotal - start;
        if (count > BR_FILE_ROWS) count = BR_FILE_ROWS;

        _d.setTextColor(COL_BLACK);
        for (int i = 0; i < count; i++) {
            int idx = start + i;
            int y   = BR_FILE_Y + i * BR_FL_ROW_H;
            _d.drawRect(BR_FL_X, y, BR_FL_W, BR_FL_ROW_H - 4, COL_BLACK);

            // Strip ".mgt" for display; show "instruments" as a special label.
            char nameBuf[40];
            strncpy(nameBuf, _rsFileNames[idx], sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            int len = (int)strlen(nameBuf);
            if (len >= 4) {
                const char* ext = nameBuf + len - 4;
                if (ext[0] == '.' &&
                    (ext[1] == 'm' || ext[1] == 'M') &&
                    (ext[2] == 'g' || ext[2] == 'G') &&
                    (ext[3] == 't' || ext[3] == 'T')) {
                    nameBuf[len - 4] = '\0';
                }
            }

            _d.setTextSize(3);
            _d.setCursor(BR_FL_X + 16, y + (BR_FL_ROW_H - 4 - 24) / 2);
            _d.print(nameBuf);
        }

        if (_rsFileTotal > BR_FILE_ROWS) {
            char hint[40];
            int firstVis = _rsFileScroll + 1;
            int lastVis  = _rsFileScroll + BR_FILE_ROWS;
            if (lastVis > _rsFileTotal) lastVis = _rsFileTotal;
            snprintf(hint, sizeof(hint), "%d-%d of %d  (drag to scroll)",
                     firstVis, lastVis, _rsFileTotal);
            _d.setTextSize(2);
            int tw = (int)strlen(hint) * 12;
            _d.setCursor((960 - tw) / 2, BR_FILE_Y + BR_FILE_ROWS * BR_FL_ROW_H + 4);
            _d.print(hint);
        }
    }

    uiButton(_d, BR_RALL_X, BR_RALL_Y, BR_RALL_W, BR_RALL_H,
             "RESTORE ALL", COL_WHITE, COL_BLACK, 3);
}

void BackupRestorePage::drawRestoreConfirm() {
    drawHeader("CONFIRM RESTORE");

    char title[64];
    if (_rsConfirmIdx < 0) {
        snprintf(title, sizeof(title), "Restore All?");
    } else {
        char nameBuf[40];
        strncpy(nameBuf, _rsFileNames[_rsConfirmIdx], sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        int len = (int)strlen(nameBuf);
        if (len >= 4) {
            const char* ext = nameBuf + len - 4;
            if (ext[0] == '.' &&
                (ext[1] == 'm' || ext[1] == 'M') &&
                (ext[2] == 'g' || ext[2] == 'G') &&
                (ext[3] == 't' || ext[3] == 'T')) {
                nameBuf[len - 4] = '\0';
            }
        }
        snprintf(title, sizeof(title), "Restore %s?", nameBuf);
    }

    _d.setTextSize(4);
    _d.setTextColor(COL_BLACK);
    int tw = (int)strlen(title) * 24;
    _d.setCursor((960 - tw) / 2, BR_CONF_TITLE_Y);
    _d.print(title);

    const char* body = (_rsConfirmIdx < 0)
        ? "Overwrites every file on the server from this backup."
        : "Overwrites this file on the server.";
    _d.setTextSize(2);
    _d.setTextColor(COL_DKGREY);
    int bw = (int)strlen(body) * 12;
    _d.setCursor((960 - bw) / 2, BR_CONF_BODY_Y);
    _d.print(body);

    uiButton(_d, BR_CONF_YES_X, BR_CONF_BTN_Y, BR_CONF_BTN_W, BR_CONF_BTN_H,
             "YES", COL_BLACK, COL_WHITE, 4);
    uiButton(_d, BR_CONF_NO_X,  BR_CONF_BTN_Y, BR_CONF_BTN_W, BR_CONF_BTN_H,
             "NO",  COL_WHITE, COL_BLACK, 4);
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

// ── Restore logic ────────────────────────────────────────────────────────────

void BackupRestorePage::scanBackupFolders() {
    _rsFolderCount     = 0;
    _rsFolderScroll    = 0;
    _rsFolderScanIdx   = 0;
    _rsFolderListDirty = false;
    if (!SD.exists(BR_BACKUPS_DIR)) return;
    File d = SD.open(BR_BACKUPS_DIR);
    if (!d || !d.isDirectory()) return;
    // Enumerate EVERY backup folder, keeping the newest BR_MAX_FOLDERS.  Folder
    // names (bk_YYMMDD_HHMMSS) sort lexicographically == reverse-chronologically,
    // so insert each into a descending-sorted array and drop the oldest once
    // full.  (The old code stopped enumerating at BR_MAX_FOLDERS *before* sorting
    // — so once more than 16 backups existed, the newest ones, handed back last
    // by FAT, were dropped and never appeared on the restore list.)
    while (true) {
        File entry = d.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) {
            const char* name = entry.name();
            const char* slash = strrchr(name, '/');
            if (slash) name = slash + 1;

            char cand[BR_FOLDER_MAX];
            strncpy(cand, name, BR_FOLDER_MAX - 1);
            cand[BR_FOLDER_MAX - 1] = '\0';

            // When full, skip anything older than the oldest one we're keeping.
            if (!(_rsFolderCount == BR_MAX_FOLDERS &&
                  strcmp(cand, _rsFolders[BR_MAX_FOLDERS - 1]) <= 0)) {
                int pos = (_rsFolderCount < BR_MAX_FOLDERS)
                              ? _rsFolderCount : (BR_MAX_FOLDERS - 1);
                while (pos > 0 && strcmp(_rsFolders[pos - 1], cand) < 0) {
                    memcpy(_rsFolders[pos], _rsFolders[pos - 1], BR_FOLDER_MAX);
                    pos--;
                }
                memcpy(_rsFolders[pos], cand, BR_FOLDER_MAX);
                if (_rsFolderCount < BR_MAX_FOLDERS) _rsFolderCount++;
            }
        }
        entry.close();
    }
    d.close();

    // Array is already sorted newest-first by construction; counts fill in via
    // scanFolderSongsTick().
    for (int i = 0; i < _rsFolderCount; i++) _rsFolderSongCount[i] = -1;
}

void BackupRestorePage::scanFolderSongsTick() {
    if (_rsFolderScanIdx >= _rsFolderCount) return;
    int idx = _rsFolderScanIdx++;

    int songs = 0;
    char sub[80];
    snprintf(sub, sizeof(sub), "%s/%s", BR_BACKUPS_DIR, _rsFolders[idx]);
    File sd = SD.open(sub);
    if (sd && sd.isDirectory()) {
        while (true) {
            File e = sd.openNextFile();
            if (!e) break;
            if (!e.isDirectory()) {
                const char* n = e.name();
                const char* sl = strrchr(n, '/');
                if (sl) n = sl + 1;
                int len = (int)strlen(n);
                if (len >= 4) {
                    const char* ext = n + len - 4;
                    if (ext[0] == '.' &&
                        (ext[1] == 'm' || ext[1] == 'M') &&
                        (ext[2] == 'g' || ext[2] == 'G') &&
                        (ext[3] == 't' || ext[3] == 'T') &&
                        strcasecmp(n, "instruments.mgt") != 0) {
                        songs++;
                    }
                }
            }
            e.close();
        }
        sd.close();
    }
    _rsFolderSongCount[idx] = songs;

    // Only flag the list for redraw if the row we just updated is on screen.
    int start = _rsFolderScroll;
    int end   = start + BR_FL_ROWS;
    if (idx >= start && idx < end) {
        _rsFolderListDirty = true;
    }
}

void BackupRestorePage::scanFolderFiles(const char* folder) {
    _rsFileTotal     = 0;
    _rsFileScroll    = 0;
    _rsFileDragMoved = false;
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
            // Accept every backup file type so they all restore: .mgt
            // (songs + instruments), .set (setlists), .txt (drumtracks).
            // The server routes each back to the right dir by extension.
            int len = (int)strlen(name);
            if (len >= 4 && name[len - 4] == '.') {
                const char* ext = name + len - 3;
                if (strcasecmp(ext, "mgt") == 0 ||
                    strcasecmp(ext, "set") == 0 ||
                    strcasecmp(ext, "txt") == 0) {
                    strncpy(_rsFileNames[_rsFileTotal], name, SRV_FNAME_MAX - 1);
                    _rsFileNames[_rsFileTotal][SRV_FNAME_MAX - 1] = '\0';
                    _rsFileTotal++;
                }
            }
        }
        entry.close();
    }
    d.close();

    // Sort alphabetically (case-insensitive) — keeps related songs together
    // and puts "instruments" wherever it falls naturally.
    for (int i = 1; i < _rsFileTotal; i++) {
        char keyName[SRV_FNAME_MAX];
        memcpy(keyName, _rsFileNames[i], SRV_FNAME_MAX);
        int j = i - 1;
        while (j >= 0 && strcasecmp(_rsFileNames[j], keyName) > 0) {
            memcpy(_rsFileNames[j+1], _rsFileNames[j], SRV_FNAME_MAX);
            j--;
        }
        memcpy(_rsFileNames[j+1], keyName, SRV_FNAME_MAX);
    }
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
    // Backup is fully synchronous in startBackup() — no per-tick driver
    // needed.  Restore still streams file-by-file off the UI loop.
    if (_state == BRState::RESTORE_PROGRESS)
        restoreTick();

    // Background song-count scan for the folder list.  One folder per tick
    // keeps the SD work off the initial display path.  Don't redraw while
    // the user is touching (could interfere with a drag-scroll); the dirty
    // flag is consumed on the next idle tick.
    if (_state == BRState::FOLDER_LIST) {
        if (_rsFolderScanIdx < _rsFolderCount) {
            scanFolderSongsTick();
        }
        if (_rsFolderListDirty && !_touch.isTouched) {
            _rsFolderListDirty = false;
            _d.fillRect(0, BR_HDR_H, 960, 540 - BR_HDR_H, COL_WHITE);
            drawFolderList();
            _d.paintLater();
        }
    }

    if (!_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // ── Drag-scroll for the FOLDER_LIST view ────────────────────────────────
    // Rising edge: capture start position so we can decide tap-vs-drag later.
    if (_state == BRState::FOLDER_LIST) {
        if (down && !_wasDown) {
            _rsDragStartY      = sy;
            _rsDragStartScroll = _rsFolderScroll;
            _rsDragMoved       = false;
        } else if (down && _wasDown) {
            int dy = sy - _rsDragStartY;
            if (!_rsDragMoved &&
                (dy >= BR_FL_DRAG_THRESH || dy <= -BR_FL_DRAG_THRESH)) {
                _rsDragMoved = true;
            }
            if (_rsDragMoved) {
                // Drag finger DOWN → reveal earlier rows → smaller offset.
                int rowDelta = -dy / BR_FL_ROW_H;
                int newOff   = _rsDragStartScroll + rowDelta;
                int maxOff   = _rsFolderCount - BR_FL_ROWS;
                if (maxOff < 0) maxOff = 0;
                if (newOff < 0)      newOff = 0;
                if (newOff > maxOff) newOff = maxOff;
                if (newOff != _rsFolderScroll) {
                    _rsFolderScroll = newOff;
                    _d.fillRect(0, BR_HDR_H, 960, 540 - BR_HDR_H, COL_WHITE);
                    drawFolderList();
                    _d.paintLater();
                }
            }
            _wasDown = true;
            return false;
        }
    }

    // ── Drag-scroll for the FILE_LIST view ──────────────────────────────────
    // Same shape as the FOLDER_LIST handler above; the bottom RESTORE ALL
    // button keeps working because the drag-mode flip suppresses the tap.
    if (_state == BRState::FILE_LIST) {
        if (down && !_wasDown) {
            _rsFileDragStartY      = sy;
            _rsFileDragStartScroll = _rsFileScroll;
            _rsFileDragMoved       = false;
        } else if (down && _wasDown) {
            int dy = sy - _rsFileDragStartY;
            if (!_rsFileDragMoved &&
                (dy >= BR_FL_DRAG_THRESH || dy <= -BR_FL_DRAG_THRESH)) {
                _rsFileDragMoved = true;
            }
            if (_rsFileDragMoved) {
                int rowDelta = -dy / BR_FL_ROW_H;
                int newOff   = _rsFileDragStartScroll + rowDelta;
                int maxOff   = _rsFileTotal - BR_FILE_ROWS;
                if (maxOff < 0) maxOff = 0;
                if (newOff < 0)      newOff = 0;
                if (newOff > maxOff) newOff = maxOff;
                if (newOff != _rsFileScroll) {
                    _rsFileScroll = newOff;
                    _d.fillRect(0, BR_HDR_H, 960, 540 - BR_HDR_H, COL_WHITE);
                    drawFileList();
                    _d.paintLater();
                }
            }
            _wasDown = true;
            return false;
        }
    }

    // Only act on falling edge
    if (!down && _wasDown) {
        _wasDown = false;

        // If the user was drag-scrolling, swallow the release entirely so
        // it can't fire HOME / BACK / row taps under the lift-off point.
        if (_rsDragMoved)     { _rsDragMoved = false;     return false; }
        if (_rsFileDragMoved) { _rsFileDragMoved = false; return false; }

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
                if (hitHeaderBack(sx, sy)) {
                    _state = BRState::MENU;
                    draw(); _d.paint();
                    return false;
                }
                // Folder tap (single column, drag-scrolled)
                int start = _rsFolderScroll;
                int count = _rsFolderCount - start;
                if (count > BR_FL_ROWS) count = BR_FL_ROWS;
                for (int i = 0; i < count; i++) {
                    int ly = BR_FL_Y + i * BR_FL_ROW_H;
                    if (sx >= BR_FL_X && sx < BR_FL_X + BR_FL_W &&
                        sy >= ly && sy < ly + BR_FL_ROW_H - 4) {
                        scanFolderFiles(_rsFolders[start + i]);
                        _state = BRState::FILE_LIST;
                        draw(); _d.paint();
                        return false;
                    }
                }
                break;
            }

            case BRState::FILE_LIST: {
                if (hitHeaderBack(sx, sy)) {
                    _state = BRState::FOLDER_LIST;
                    draw(); _d.paint();
                    return false;
                }
                // RESTORE ALL button (bottom-centred) → confirm
                if (sx >= BR_RALL_X && sx < BR_RALL_X + BR_RALL_W &&
                    sy >= BR_RALL_Y && sy < BR_RALL_Y + BR_RALL_H) {
                    _rsConfirmIdx = -1;
                    _state = BRState::RESTORE_CONFIRM;
                    draw(); _d.paint();
                    return false;
                }
                // Individual file tap → confirm
                int start = _rsFileScroll;
                int count = _rsFileTotal - start;
                if (count > BR_FILE_ROWS) count = BR_FILE_ROWS;
                for (int i = 0; i < count; i++) {
                    int ly = BR_FILE_Y + i * BR_FL_ROW_H;
                    if (sx >= BR_FL_X && sx < BR_FL_X + BR_FL_W &&
                        sy >= ly && sy < ly + BR_FL_ROW_H - 4) {
                        _rsConfirmIdx = start + i;
                        _state = BRState::RESTORE_CONFIRM;
                        draw(); _d.paint();
                        return false;
                    }
                }
                break;
            }

            case BRState::RESTORE_CONFIRM: {
                // YES → kick off the restore
                if (sx >= BR_CONF_YES_X && sx < BR_CONF_YES_X + BR_CONF_BTN_W &&
                    sy >= BR_CONF_BTN_Y && sy < BR_CONF_BTN_Y + BR_CONF_BTN_H) {
                    startRestore(_rsConfirmIdx);
                    return false;
                }
                // NO → back to the file list
                if (sx >= BR_CONF_NO_X && sx < BR_CONF_NO_X + BR_CONF_BTN_W &&
                    sy >= BR_CONF_BTN_Y && sy < BR_CONF_BTN_Y + BR_CONF_BTN_H) {
                    _state = BRState::FILE_LIST;
                    draw(); _d.paint();
                    return false;
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

bool BackupRestorePage::hitHeaderBack(int sx, int sy) const {
    return (sx >= BR_HBACK_X && sx < BR_HBACK_X + BR_HBACK_W &&
            sy >= 0 && sy < BR_HDR_H);
}

void BackupRestorePage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}
