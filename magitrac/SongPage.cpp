#include "SongPage.h"
#include "ServerPairing.h"
#include "SongSource.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>

extern void initSong(Song*);

// ── Constructor ───────────────────────────────────────────────────────────────

SongPage::SongPage(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _keyboard(display, touch)
    , _dialog(display, touch)
    , _browser(display, touch)
    , _state(State::FILE_BROWSE)
    , _sdAvailable(false)
    , _wasDown(false)
    , _pendingDeleteIdx(-1)
    , _fileCount(0)
    , _filePage(0)
    , _srvPage(0)
    , _srvListDrawn(false)
    , _srvPendingDeleteIdx(-1)
{
    _loadedFilename[0]  = '\0';
    _srvLoadedName[0]   = '\0';
    _pendingSaveName[0] = '\0';
}

// ── open() ────────────────────────────────────────────────────────────────────

void SongPage::open(bool sdAvailable) {
    _sdAvailable = sdAvailable;
    _filePage    = 0;
    _wasDown     = _touch.isTouched;
    // Do NOT clear _loadedFilename — preserves current file across opens.

    if (sdAvailable && !SD.exists(SONGS_DIR)) {
        SD.mkdir(SONGS_DIR);
    }

    if (gServerPairing.isPaired()) {
        // Connected to server — open server browser directly
        gServerPairing.resetBrowse();
        gServerPairing.requestSongList(0);
        _srvPage      = 0;
        _srvListDrawn = false;
        _state = State::SERVER_BROWSE;
        _browser.open();
        _browser.setTitle("SERVER");
        _browser.setLoadedName(_srvLoadedName);
        _browser.setHasPrev(false);
        _browser.setHasNext(false);
        _browser.clearItems();
        _browser.setStatusText("Loading...");
    } else {
        // Not connected — open SD card browser directly
        loadFileList();
        populateBrowserFromSD();
        _state = State::FILE_BROWSE;
        _browser.open();
    }
}

// ── draw() ────────────────────────────────────────────────────────────────────

void SongPage::draw() {
    switch (_state) {
        case State::FILE_BROWSE:
        case State::SERVER_BROWSE:
            _browser.draw();
            break;
        case State::SAVING_AS:
        case State::SAVING_AS_SRV:
            _browser.draw();
            _keyboard.draw();
            break;
        case State::CONFIRM_OVERWRITE:
        case State::CONFIRM_DELETE:
        case State::CONFIRM_DELETE_SRV:
            _browser.draw();
            _dialog.draw();
            break;
    }
}

// ── poll() ────────────────────────────────────────────────────────────────────

SongPageResult SongPage::poll() {

    // ── CONFIRM_DELETE ────────────────────────────────────────────────────────
    if (_state == State::CONFIRM_DELETE) {
        if (_dialog.poll()) {
            if (_dialog.confirmed() && _pendingDeleteIdx >= 0) {
                doDelete(_pendingDeleteIdx);
                _pendingDeleteIdx = -1;
                populateBrowserFromSD();
            }
            _state = State::FILE_BROWSE;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }
        return SongPageResult::NONE;
    }

    // ── CONFIRM_DELETE_SRV ────────────────────────────────────────────────────
    if (_state == State::CONFIRM_DELETE_SRV) {
        if (_dialog.poll()) {
            if (_dialog.confirmed() && _srvPendingDeleteIdx >= 0) {
                const char* name = gServerPairing.listName(_srvPendingDeleteIdx);
                if (name && name[0] != '\0') {
                    gServerPairing.deleteSongOnServer(name);
                    if (_srvLoadedName[0] != '\0' &&
                        strcmp(name, _srvLoadedName) == 0) {
                        _srvLoadedName[0] = '\0';
                        gSongSource = SongSource::NONE;
                    }
                }
                _srvPendingDeleteIdx = -1;
                _srvListDrawn = false;
                gServerPairing.requestSongList(_srvPage);
            }
            _state = State::SERVER_BROWSE;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }
        return SongPageResult::NONE;
    }

    // ── CONFIRM_OVERWRITE ─────────────────────────────────────────────────────
    if (_state == State::CONFIRM_OVERWRITE) {
        if (_dialog.poll()) {
            if (_dialog.confirmed()) {
                doSave(_pendingSaveName);
                loadFileList();
                populateBrowserFromSD();
            }
            _state = State::FILE_BROWSE;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }
        return SongPageResult::NONE;
    }

    // ── SAVING_AS — keyboard ──────────────────────────────────────────────────
    if (_state == State::SAVING_AS) {
        if (_keyboard.poll()) {
            if (_keyboard.isDone() && _pendingSaveName[0] != '\0') {
                if (fileExists(_pendingSaveName)) {
                    _dialog.open("OVERWRITE EXISTING FILE?");
                    _state = State::CONFIRM_OVERWRITE;
                    _d.fillScreen(COL_WHITE);
                    _browser.draw();
                    _dialog.draw();
                    _d.paintLater();
                } else {
                    doSave(_pendingSaveName);
                    loadFileList();
                    populateBrowserFromSD();
                    _state = State::FILE_BROWSE;
                    _d.fillScreen(COL_WHITE);
                    _browser.draw();
                    _d.paintLater();
                }
            } else {
                _state = State::FILE_BROWSE;
                _d.fillScreen(COL_WHITE);
                _browser.draw();
                _d.paintLater();
            }
        }
        return SongPageResult::NONE;
    }

    // ── SAVING_AS_SRV — keyboard for server save-as ───────────────────────────
    if (_state == State::SAVING_AS_SRV) {
        if (_keyboard.poll()) {
            if (_keyboard.isDone() && _pendingSaveName[0] != '\0') {
                strncpy(_song.name, _pendingSaveName, sizeof(_song.name) - 1);
                _song.name[sizeof(_song.name) - 1] = '\0';
                gServerPairing.sendSongToServer(_pendingSaveName, &_song);
                strncpy(_srvLoadedName, _pendingSaveName, sizeof(_srvLoadedName) - 1);
                _srvLoadedName[sizeof(_srvLoadedName) - 1] = '\0';
                // Refresh list so the newly saved file appears
                gServerPairing.requestSongList(_srvPage);
                _srvListDrawn = false;
                _browser.setHasPrev(false);
                _browser.setHasNext(false);
                _browser.clearItems();
                _browser.setStatusText("Saving...");
            }
            _state = State::SERVER_BROWSE;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }
        return SongPageResult::NONE;
    }

    // ── SERVER_BROWSE ─────────────────────────────────────────────────────────
    if (_state == State::SERVER_BROWSE) {
        BrowseState bs = gServerPairing.browseState();

        // Song received — load it and return
        if (bs == BrowseState::SONG_READY) {
            if (gServerPairing.copySong(&_song)) {
                _loadedFilename[0] = '\0';
                strncpy(_srvLoadedName, _song.name, sizeof(_srvLoadedName) - 1);
                _srvLoadedName[sizeof(_srvLoadedName) - 1] = '\0';
                gSongSource = SongSource::SERVER;
                gServerPairing.resetBrowse();
                return SongPageResult::SONG_LOADED;
            }
            // Corrupt data — request list again
            gServerPairing.requestSongList(_srvPage);
            _srvListDrawn = false;
        }

        // List arrived — repopulate browser
        if (bs == BrowseState::LIST_READY && !_srvListDrawn) {
            _srvListDrawn = true;
            populateBrowserFromServer();
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }

        // Song load timeout — give up after 8 s and show list again
        if (bs == BrowseState::WAITING_SONG && millis() - _srvLoadStartMs > 8000) {
            gServerPairing.resetBrowse();
            gServerPairing.requestSongList(_srvPage);
            _srvListDrawn = false;
        }

        FileBrowserResult r = _browser.poll();
        switch (r.event) {

        case FileBrowserEvent::HOME:
            gServerPairing.resetBrowse();
            return SongPageResult::HOME;

        case FileBrowserEvent::PREV_PAGE:
            _srvPage--;
            _srvListDrawn = false;
            gServerPairing.requestSongList(_srvPage);
            _browser.clearItems();
            _browser.setHasPrev(false);
            _browser.setHasNext(false);
            _browser.setStatusText("Loading...");
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::NEXT_PAGE:
            _srvPage++;
            _srvListDrawn = false;
            gServerPairing.requestSongList(_srvPage);
            _browser.clearItems();
            _browser.setHasPrev(false);
            _browser.setHasNext(false);
            _browser.setStatusText("Loading...");
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::NEW:
            initSong(&_song);
            _srvLoadedName[0] = '\0';
            gSongSource = SongSource::NONE;
            gServerPairing.sendSongToServer("", &_song);  // push blank song to server (no SD save)
            return SongPageResult::SONG_LOADED;

        case FileBrowserEvent::SAVE:
            gServerPairing.sendSongToServer(_srvLoadedName, &_song);
            gServerPairing.requestSongList(_srvPage);
            _srvListDrawn = false;
            _browser.setHasPrev(false);
            _browser.setHasNext(false);
            _browser.clearItems();
            _browser.setStatusText("Saving...");
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::SAVE_AS:
            strncpy(_pendingSaveName, _song.name, sizeof(_pendingSaveName) - 1);
            _pendingSaveName[sizeof(_pendingSaveName) - 1] = '\0';
            _keyboard.open(_pendingSaveName, 9);
            _state = State::SAVING_AS_SRV;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _keyboard.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::ITEM_TAP:
            gServerPairing.requestSongLoad(_srvPage, (uint8_t)r.index);
            _srvLoadStartMs = millis();
            _srvListDrawn = false;
            _browser.clearItems();
            _browser.setHasPrev(false);
            _browser.setHasNext(false);
            _browser.setStatusText("Receiving song...");
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::ITEM_DELETE:
            _srvPendingDeleteIdx = r.index;
            _dialog.open("DELETE SERVER FILE?");
            _state = State::CONFIRM_DELETE_SRV;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _dialog.draw();
            _d.paintLater();
            break;

        default:
            break;
        }
        return SongPageResult::NONE;
    }

    // ── FILE_BROWSE ───────────────────────────────────────────────────────────
    if (_state == State::FILE_BROWSE) {
        FileBrowserResult r = _browser.poll();
        switch (r.event) {

        case FileBrowserEvent::HOME:
            return SongPageResult::HOME;

        case FileBrowserEvent::PREV_PAGE:
            _filePage--;
            populateBrowserFromSD();
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::NEXT_PAGE:
            _filePage++;
            populateBrowserFromSD();
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::NEW:
            initSong(&_song);
            _loadedFilename[0] = '\0';
            gSongSource = SongSource::NONE;
            return SongPageResult::SONG_LOADED;

        case FileBrowserEvent::SAVE:
            doSave(_loadedFilename);
            loadFileList();
            populateBrowserFromSD();
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::SAVE_AS:
            strncpy(_pendingSaveName, _song.name, sizeof(_pendingSaveName) - 1);
            _pendingSaveName[sizeof(_pendingSaveName) - 1] = '\0';
            _keyboard.open(_pendingSaveName, 9);
            _state = State::SAVING_AS;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _keyboard.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::ITEM_TAP: {
            int absIdx = _filePage * FB_PER_PAGE + r.index;
            if (absIdx >= 0 && absIdx < _fileCount) {
                char path[64];
                buildPath(_files[absIdx], path, sizeof(path));
                if (loadSong(path, &_song)) {
                    strncpy(_loadedFilename, _files[absIdx],
                            sizeof(_loadedFilename) - 1);
                    _loadedFilename[sizeof(_loadedFilename) - 1] = '\0';
                    gSongSource = SongSource::SD;
                    return SongPageResult::SONG_LOADED;
                }
                Serial.printf("[SongPage] load failed: %s\n", path);
            }
            break;
        }

        case FileBrowserEvent::ITEM_DELETE: {
            int absIdx = _filePage * FB_PER_PAGE + r.index;
            if (absIdx >= 0 && absIdx < _fileCount) {
                _pendingDeleteIdx = absIdx;
                _dialog.open("DELETE FILE?");
                _state = State::CONFIRM_DELETE;
                _d.fillScreen(COL_WHITE);
                _browser.draw();
                _dialog.draw();
                _d.paintLater();
            }
            break;
        }

        default:
            break;
        }
        return SongPageResult::NONE;
    }

    return SongPageResult::NONE;
}

// ── Drawing ───────────────────────────────────────────────────────────────────

void SongPage::drawHeader(const char* title, int h) {
    _d.fillRect(0, 0, 960, h, COL_BLACK);
    _d.setTextSize(3);
    _d.setTextColor(COL_WHITE);
    int tw = (int)strlen(title) * 18;
    _d.setCursor((960 - tw) / 2, (h - 24) / 2);
    _d.print(title);
}

// ── Browser population helpers ────────────────────────────────────────────────

void SongPage::populateBrowserFromSD() {
    // Build display name from loaded filename (strip .mgt)
    char loadedDisplay[STORAGE_FILENAME_MAX] = {};
    if (_loadedFilename[0]) {
        strncpy(loadedDisplay, _loadedFilename, sizeof(loadedDisplay) - 1);
        int len = (int)strlen(loadedDisplay);
        if (len > 4 && loadedDisplay[len - 4] == '.' &&
            (loadedDisplay[len - 3] == 'm' || loadedDisplay[len - 3] == 'M')) {
            loadedDisplay[len - 4] = '\0';
        }
    }

    _browser.setTitle("SD CARD");
    _browser.setLoadedName(loadedDisplay);
    _browser.setHasPrev(_filePage > 0);
    _browser.setHasNext((_filePage + 1) * FB_PER_PAGE < _fileCount);
    _browser.clearItems();
    _browser.setStatusText("");

    int first = _filePage * FB_PER_PAGE;
    for (int i = first; i < _fileCount && (i - first) < FB_PER_PAGE; i++) {
        char label[STORAGE_FILENAME_MAX];
        strncpy(label, _files[i], sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
        int len = (int)strlen(label);
        if (len > 4 && label[len - 4] == '.' &&
            (label[len - 3] == 'm' || label[len - 3] == 'M')) {
            label[len - 4] = '\0';
        }
        _browser.addItem(label);
    }
}

void SongPage::populateBrowserFromServer() {
    _browser.setTitle("SERVER");
    _browser.setLoadedName(_srvLoadedName);
    _browser.setHasPrev(_srvPage > 0);
    _browser.setHasNext(_srvPage + 1 < gServerPairing.listTotalPages());
    _browser.clearItems();
    _browser.setStatusText("");

    int count = gServerPairing.listCount();
    for (int i = 0; i < count; i++) {
        _browser.addItem(gServerPairing.listName(i));
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void SongPage::loadFileList() {
    _fileCount = listSongs(SONGS_DIR, _files, STORAGE_MAX_FILES);
    _filePage  = 0;
}

void SongPage::buildPath(const char* name, char* out, int outLen) const {
    snprintf(out, outLen, "%s/%s", SONGS_DIR, name);
    int len = (int)strlen(out);
    if (len >= 4) {
        const char* ext = out + len - 4;
        if (!(ext[0] == '.' &&
              (ext[1] == 'm' || ext[1] == 'M') &&
              (ext[2] == 'g' || ext[2] == 'G') &&
              (ext[3] == 't' || ext[3] == 'T'))) {
            if (len + 4 < outLen) strcat(out, ".mgt");
        }
    }
}

bool SongPage::fileExists(const char* name) const {
    char path[64];
    buildPath(name, path, sizeof(path));
    return SD.exists(path);
}

void SongPage::doSave(const char* name) {
    strncpy(_song.name, name, sizeof(_song.name) - 1);
    _song.name[sizeof(_song.name) - 1] = '\0';

    char path[64];
    buildPath(name, path, sizeof(path));
    if (saveSong(path, &_song)) {
        char fname[STORAGE_FILENAME_MAX];
        snprintf(fname, sizeof(fname), "%s", name);
        int len = (int)strlen(fname);
        if (len < 4 || fname[len - 4] != '.') {
            if (len + 4 < (int)sizeof(fname)) strcat(fname, ".mgt");
        }
        strncpy(_loadedFilename, fname, sizeof(_loadedFilename) - 1);
        _loadedFilename[sizeof(_loadedFilename) - 1] = '\0';
    } else {
        Serial.printf("[SongPage] save failed: %s\n", path);
    }
}

void SongPage::setServerLoadedName(const char* name) {
    if (name && name[0]) {
        strncpy(_srvLoadedName, name, sizeof(_srvLoadedName) - 1);
        _srvLoadedName[sizeof(_srvLoadedName) - 1] = '\0';
    } else {
        _srvLoadedName[0] = '\0';
    }
}

void SongPage::doDelete(int fileIdx) {
    char path[64];
    buildPath(_files[fileIdx], path, sizeof(path));
    SD.remove(path);
    if (_loadedFilename[0] != '\0' &&
        strcmp(_files[fileIdx], _loadedFilename) == 0) {
        _loadedFilename[0] = '\0';
    }
    loadFileList();
}

void SongPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

// ── Boot button alt mode ───────────────────────────────────────────────────────

void SongPage::onBootPress() {
    if (_state == State::SAVING_AS || _state == State::SAVING_AS_SRV) {
        _keyboard.toggleSymbolLayer();
    } else if (_state == State::FILE_BROWSE || _state == State::SERVER_BROWSE) {
        _browser.onBootPress();
    }
    // Ignore on CONFIRM_* states
}
