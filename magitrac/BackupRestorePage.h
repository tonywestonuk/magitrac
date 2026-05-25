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

#define BR_MAX_FILES    (SRV_MAX_FILES + 1)  // songs + instruments
#define BR_MAX_FOLDERS  16
#define BR_FOLDER_MAX   32  // max folder name length
#define BR_BACKUPS_DIR  "/backups"

// List display
#define BR_LIST_COLS    2
#define BR_LIST_ROWS    7
#define BR_LIST_PER_PAGE (BR_LIST_COLS * BR_LIST_ROWS)

enum class BRState : uint8_t {
    MENU,
    BACKUP_WAITING_LIST,
    BACKUP_PROGRESS,
    BACKUP_DONE,
    FOLDER_LIST,
    FILE_LIST,
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

    // After poll() returns true, main loop checks this — if true, opens
    // TcpTestPage instead of restoring whatever was behind us.  One-shot
    // (consumed by reading).
    bool consumeTcpTestRequest() {
        bool r = _tcpTestRequested;
        _tcpTestRequested = false;
        return r;
    }

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    I2C_BM8563*          _rtc;
    BRState              _state;
    bool                 _wasDown;

    // Backup state
    char     _bkFolder[48];                        // full path e.g. "/backups/bk_20260425_103000"
    char     _bkFileNames[BR_MAX_FILES][SRV_FNAME_MAX];
    int      _bkFileTotal;
    int      _bkFileCurrent;
    // Set from UI task on Cancel tap, read from the v2 backup worker
    // task — must be volatile for visibility across cores/tasks.
    volatile bool _bkCancelled;


    // TCP/IP test request flag (set when user taps the TCP TEST menu button)
    bool     _tcpTestRequested = false;

    // Restore state
    char     _rsFolders[BR_MAX_FOLDERS][BR_FOLDER_MAX];
    int      _rsFolderCount;
    int      _rsFolderPage;
    char     _rsSelectedFolder[48];
    char     _rsFileNames[BR_MAX_FILES][SRV_FNAME_MAX];
    int      _rsFileTotal;
    int      _rsFileCurrent;
    int      _rsRestoreCount;
    int      _rsRestoreList[BR_MAX_FILES];  // indices of files to restore
    bool     _rsCancelled;

    // Error message
    char     _errMsg[48];

    // Drawing
    void drawHeader(const char* title);
    void drawMenu();
    void drawBackupProgress();
    void drawBackupDone();
    void drawFolderList();
    void drawFileList();
    void drawRestoreProgress();
    void drawRestoreDone();
    void drawError();
    void drawProgressBar(int x, int y, int w, int h, int current, int total);

    // Touch
    void rawToScreen(int rx, int ry, int& sx, int& sy) const;
    bool hitHome(int sx, int sy) const;

    // Backup logic
    void startBackup();
    void backupTick();

    // Restore logic
    void scanBackupFolders();
    void scanFolderFiles(const char* folder);
    void startRestore(int fileIdx);  // -1 = all
    void restoreTick();
};
