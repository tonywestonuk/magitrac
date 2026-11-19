#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "UIHelpers.h"
#include "MagiMsg.h"
#include "ServerPairing.h"
#include <I2C_BM8563.h>

// ── BackupRestorePage — full-screen backup and restore UI ─────────────────────
//
//  y=  0  ┌─ Header: "BACKUP / RESTORE"  [HOME]  (50px)
//  y= 50  └─ Content area (490px) — varies by state

// Files one backup folder can list for restore: songs, instruments,
// drumtracks, setlists and samples.  ~17 KB with the restore list — the page
// is new'd, so it lands in PSRAM.
#define BR_MAX_FILES    256
#define BR_MAX_FOLDERS  128  // list scrolls; keep ~every backup rather than silently dropping (~4KB heap)
#define BR_FOLDER_MAX   32  // max folder name length
#define BR_BACKUPS_DIR  "/backups"

enum class BRState : uint8_t {
    MENU,
    BACKUP_ASK,          // include the sample WAVs?
    BACKUP_PROGRESS,
    BACKUP_DONE,
    FOLDER_LIST,
    FILE_LIST,
    RESTORE_CONFIRM,
    RESTORE_PROGRESS,
    RESTORE_DONE,
    ERROR_SCREEN,
};

class BackupRestorePage {
public:
    BackupRestorePage(EPD_PainterAdafruit& display, GT911_Lite& touch,
                      I2C_BM8563* rtc);

    void open();
    void draw();
    bool poll();   // returns true when page should close

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    I2C_BM8563*          _rtc;
    BRState              _state;
    bool                 _wasDown;

    // Backup state
    char     _bkFolder[48];                        // full path e.g. "/backups/bk_260425_103000" (bk_YYMMDD_HHMMSS)
    char     _bkCurName[BK_FNAME_MAX];             // name of the file currently streaming (display only)
    int      _bkFileTotal;
    int      _bkFileCurrent;
    // Set by a Cancel tap.  The backup runs synchronously inside
    // startBackup() on the UI task, which polls touch itself between
    // messages; once set, the rest of the stream is drained, not written.
    volatile bool _bkCancelled;

    // Number of files fully written to SD this backup run.  Used by
    // drawBackupDone to show "N of M saved" (especially after a cancel).
    int _bkFilesSaved = 0;
    // Files that did NOT make it onto the card (open failed, short write, or
    // the stream ended early).  Non-zero turns the done screen into a warning.
    int _bkFilesFailed = 0;
    // Restore: files handed to the link, and files that could not be read
    // from the card or sent.
    int _rsFilesSent   = 0;
    int _rsFilesFailed = 0;

    // Restore state
    char     _rsFolders[BR_MAX_FOLDERS][BR_FOLDER_MAX];
    int      _rsFolderSongCount[BR_MAX_FOLDERS];   // .mgt count minus instruments.mgt
    int      _rsFolderCount;
    int      _rsFolderScroll;                      // row offset for drag-scroll
    int      _rsDragStartY;
    int      _rsDragStartScroll;
    bool     _rsDragMoved;
    char     _rsSelectedFolder[48];
    char     _rsFileNames[BR_MAX_FILES][BK_FNAME_MAX];
    int      _rsFileTotal;
    int      _rsFileCurrent;
    int      _rsRestoreCount;
    int      _rsRestoreList[BR_MAX_FILES];  // indices of files to restore
    int      _rsConfirmIdx;                 // -1 = restore all; else file index

    // FILE_LIST drag-scroll state — mirrors the folder list.
    int      _rsFileScroll;
    int      _rsFileDragStartY;
    int      _rsFileDragStartScroll;
    bool     _rsFileDragMoved;

    // Deferred song-counting for the folder list.  scanBackupFolders only
    // collects names; song counts are filled in over subsequent poll()
    // ticks so the list appears immediately.  -1 in the count means "not
    // yet scanned".
    int      _rsFolderScanIdx;
    bool     _rsFolderListDirty;

    // Error message
    char     _errMsg[48];

    // Drawing
    void drawHeader(const char* title, bool withBack = false);
    void drawMenu();
    void drawBackupAsk();
    void drawBackupProgress();
    void drawBackupDone();
    void drawFolderList();
    void drawFileList();
    void drawRestoreConfirm();
    void drawRestoreProgress();
    void drawRestoreDone();
    void drawError();
    void drawProgressBar(int x, int y, int w, int h, int current, int total);

    // Touch
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    bool hitHome(int sx, int sy) const;
    bool hitHeaderBack(int sx, int sy) const;

    // Backup logic
    void startBackup(bool withSamples);

    // Restore logic
    void scanBackupFolders();
    void scanFolderSongsTick();      // counts songs for one folder per call
    void scanFolderFiles(const char* folder);
    void startRestore(int fileIdx);  // -1 = all
    void restoreTick();
};
