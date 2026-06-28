#include "SongPage.h"
#include "ServerPairing.h"
#include "SongSource.h"
#include "Autosave.h"
#include <Arduino.h>
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
    , _state(State::NOT_CONNECTED)
    , _wasDown(false)
    , _srvPage(0)
    , _srvListDrawn(false)
{
    _loadedFilename[0]       = '\0';
    _srvLoadedName[0]        = '\0';
    _pendingSaveName[0]      = '\0';
    _srvPendingDeleteName[0] = '\0';
}

// ── open() ────────────────────────────────────────────────────────────────────

void SongPage::open() {
    _wasDown = _touch.isTouched;
    // Do NOT clear _loadedFilename — preserves current file across opens.

    if (gServerPairing.isPaired()) {
        // Connected to server — open server browser directly.  The server list
        // is a drag-scroll list that accumulates every page into one list.
        gServerPairing.resetBrowse();
        gServerPairing.requestSongList(0);
        _srvPage      = 0;
        _srvListDrawn = false;
        _state = State::SERVER_BROWSE;
        _browser.open();
        _browser.setListMode(true);
        _browser.setTitle("SERVER");
        _browser.setLoadedName(_srvLoadedName);
        _browser.clearItems();
        _browser.setStatusText("Loading...");
    } else {
        // Not connected — the client is server-only, so there's nothing to
        // browse locally.  Show a notice prompting the user to pair.
        _state = State::NOT_CONNECTED;
    }
}

// ── draw() ────────────────────────────────────────────────────────────────────

void SongPage::draw() {
    switch (_state) {
        case State::SERVER_BROWSE:
            _browser.draw();
            break;
        case State::SAVING_AS_SRV:
        case State::NAMING_NEW:
            _browser.draw();
            _keyboard.draw();
            break;
        case State::CONFIRM_DELETE_SRV:
        case State::CONFIRM_SAVE_LOAD:
        case State::CONFIRM_NEW_EXISTS:
            _browser.draw();
            _dialog.draw();
            break;
        case State::NOT_CONNECTED: {
            drawHeader("SONGS", 50);
            _d.setTextSize(3);
            _d.setTextColor(COL_BLACK);
            const char* l1 = "Not connected.";
            const char* l2 = "Pair to a server to load songs.";
            _d.setCursor((960 - (int)strlen(l1) * 18) / 2, 210);
            _d.print(l1);
            _d.setCursor((960 - (int)strlen(l2) * 18) / 2, 260);
            _d.print(l2);
            uiButton(_d, 0, 483, 160, 57, "BACK", COL_BLACK, COL_WHITE, 3);
            break;
        }
    }
}

// ── poll() ────────────────────────────────────────────────────────────────────

SongPageResult SongPage::poll() {

    // ── NOT_CONNECTED — server-only notice; the BACK button returns home ──────
    if (_state == State::NOT_CONNECTED) {
        if (_touch.read()) {
            bool down = _touch.isTouched;
            int sx, sy;
            rawToScreen(_touch.x, _touch.y, sx, sy);
            if (!down && _wasDown) {
                _wasDown = false;
                if (sx >= 0 && sx < 160 && sy >= 483) return SongPageResult::HOME;
            } else if (down && !_wasDown) {
                _wasDown = true;
            }
        }
        return SongPageResult::NONE;
    }

    // ── CONFIRM_DELETE_SRV ────────────────────────────────────────────────────
    if (_state == State::CONFIRM_DELETE_SRV) {
        if (_dialog.poll()) {
            if (_dialog.confirmed() && _srvPendingDeleteName[0] != '\0') {
                const char* name = _srvPendingDeleteName;
                gServerPairing.deleteSongOnServer(name);
                if (_srvLoadedName[0] != '\0' && strcmp(name, _srvLoadedName) == 0) {
                    _srvLoadedName[0] = '\0';
                    gSongSource = SongSource::NONE;
                }
                _srvPendingDeleteName[0] = '\0';
                restartServerList("Loading...");
            }
            _state = State::SERVER_BROWSE;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }
        return SongPageResult::NONE;
    }

    // ── CONFIRM_SAVE_LOAD ─────────────────────────────────────────────────────
    // "Save changes to X?" before loading a different server song or starting a
    // NEW one.
    //   YES → save the current song to the server first.
    //   NO  → discard edits.
    // Then proceed with the pending action (load the tapped song, or name+create
    // a new one).
    if (_state == State::CONFIRM_SAVE_LOAD) {
        if (_dialog.poll()) {
            if (_dialog.confirmed())
                gServerPairing.sendSaveActive(_srvLoadedName);
            markSongClean();                 // saved, or edits discarded
            if (_pendingNew) {
                _pendingNew = false;
                beginNewSong();
            } else {
                _state = State::SERVER_BROWSE;
                startServerLoad(_pendingLoadAbsIdx);
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
                // Patch the server's in-memory song.name to match (so the
                // saved file carries the new display name), then ask it to
                // write to SD.  No full-song stream needed — server already
                // has every edit via the patch / note-set stream.
                gServerPairing.sendSongPatch(_song, _song.name, sizeof(_song.name));
                gServerPairing.sendSaveActive(_pendingSaveName);
                markSongClean();
                strncpy(_srvLoadedName, _pendingSaveName, sizeof(_srvLoadedName) - 1);
                _srvLoadedName[sizeof(_srvLoadedName) - 1] = '\0';
                // Refresh list so the newly saved file appears
                restartServerList("Saving...");
            }
            _state = State::SERVER_BROWSE;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }
        return SongPageResult::NONE;
    }

    // ── NAMING_NEW — keyboard for a freshly-created song's name ────────────────
    if (_state == State::NAMING_NEW) {
        if (_keyboard.poll()) {
            if (_keyboard.isDone() && _pendingSaveName[0] != '\0') {
                // Reject a name that already exists on the server — the user
                // must pick a different one (or cancel).
                if (nameExistsOnServer(_pendingSaveName)) {
                    _dialog.open("NAME EXISTS - RENAME?");
                    _state = State::CONFIRM_NEW_EXISTS;
                    _d.fillScreen(COL_WHITE);
                    _browser.draw();
                    _dialog.draw();
                    _d.paintLater();
                    return SongPageResult::NONE;
                }
                // Create a blank song with the entered name on both client and
                // server, and persist it immediately so it appears in the list.
                initSong(&_song);
                strncpy(_song.name, _pendingSaveName, sizeof(_song.name) - 1);
                _song.name[sizeof(_song.name) - 1] = '\0';
                gServerPairing.sendNewSong();   // wipe server's in-memory copy
                gServerPairing.sendSongPatch(_song, _song.name, sizeof(_song.name));
                gServerPairing.sendSaveActive(_pendingSaveName);
                markSongClean();
                strncpy(_srvLoadedName, _pendingSaveName, sizeof(_srvLoadedName) - 1);
                _srvLoadedName[sizeof(_srvLoadedName) - 1] = '\0';
                gSongSource = SongSource::SERVER;
                return SongPageResult::SONG_LOADED;
            }
            // Cancelled / empty name — abandon NEW, back to the server list.
            _state = State::SERVER_BROWSE;
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }
        return SongPageResult::NONE;
    }

    // ── CONFIRM_NEW_EXISTS — NEW name clashed an existing server song ──────────
    //   YES → re-enter a name.   NO → abandon NEW, back to the list.
    if (_state == State::CONFIRM_NEW_EXISTS) {
        if (_dialog.poll()) {
            if (_dialog.confirmed()) {
                beginNewSong();
            } else {
                _state = State::SERVER_BROWSE;
                _d.fillScreen(COL_WHITE);
                _browser.draw();
                _d.paintLater();
            }
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
            // Corrupt data — show the list again
            restartServerList("Loading...");
        }

        // A list page arrived — append its entries, then request the next page
        // until the whole (server-sorted) list is accumulated, then draw.
        if (bs == BrowseState::LIST_READY && !_srvListDrawn) {
            _srvListDrawn = true;   // this page consumed
            int c = gServerPairing.listCount();
            for (int i = 0; i < c; i++) _browser.addItem(gServerPairing.listName(i));

            if (_srvPage + 1 < gServerPairing.listTotalPages()) {
                _srvPage++;
                _srvListDrawn = false;            // expect the next page
                gServerPairing.requestSongList(_srvPage);
            } else {
                _browser.setLoadedName(_srvLoadedName);
                _browser.setStatusText("");
                _d.fillScreen(COL_WHITE);
                _browser.draw();
                _d.paintLater();
            }
        }

        // Song load timeout — give up after 8 s and show list again
        if (bs == BrowseState::WAITING_SONG && millis() - _srvLoadStartMs > 8000) {
            gServerPairing.resetBrowse();
            restartServerList("Loading...");
        }

        FileBrowserResult r = _browser.poll();
        switch (r.event) {

        case FileBrowserEvent::HOME:
            gServerPairing.resetBrowse();
            return SongPageResult::HOME;

        // PREV_PAGE / NEXT_PAGE are never emitted in list mode (drag-scroll).

        case FileBrowserEvent::NEW:
            // Same guard as loading a different song: offer to save edits first.
            if (songIsDirty() && _srvLoadedName[0] != '\0') {
                _pendingNew = true;
                char msg[48];
                snprintf(msg, sizeof(msg), "SAVE CHANGES TO %s?", _srvLoadedName);
                _dialog.open(msg);
                _state = State::CONFIRM_SAVE_LOAD;
                _d.fillScreen(COL_WHITE);
                _browser.draw();
                _dialog.draw();
                _d.paintLater();
            } else {
                beginNewSong();
            }
            break;

        case FileBrowserEvent::SAVE:
            // Server already has every edit via the patch / note-set stream;
            // ask it to write its own in-memory copy to SD under this name.
            gServerPairing.sendSaveActive(_srvLoadedName);
            markSongClean();
            restartServerList("Saving...");
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
            // If the current song has unsaved edits, offer to save it before
            // replacing it with the tapped one.
            if (songIsDirty() && _srvLoadedName[0] != '\0') {
                _pendingNew        = false;
                _pendingLoadAbsIdx = r.index;
                char msg[48];
                snprintf(msg, sizeof(msg), "SAVE CHANGES TO %s?", _srvLoadedName);
                _dialog.open(msg);
                _state = State::CONFIRM_SAVE_LOAD;
                _d.fillScreen(COL_WHITE);
                _browser.draw();
                _dialog.draw();
                _d.paintLater();
            } else {
                startServerLoad(r.index);
            }
            break;

        case FileBrowserEvent::ITEM_DELETE:
            strncpy(_srvPendingDeleteName, _browser.itemName(r.index),
                    sizeof(_srvPendingDeleteName) - 1);
            _srvPendingDeleteName[sizeof(_srvPendingDeleteName) - 1] = '\0';
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

// Restart the server list: clear accumulated items and re-request from page 0.
void SongPage::restartServerList(const char* status) {
    _srvPage      = 0;
    _srvListDrawn = false;
    _browser.clearItems();
    _browser.setStatusText(status);
    gServerPairing.requestSongList(0);
}

// Request the server song at absolute list index and show "Receiving...".
// The flat list index maps to the server's page/index (same sorted order).
void SongPage::startServerLoad(int absIdx) {
    gServerPairing.requestSongLoad((uint8_t)(absIdx / SL_PER_PKT),
                                   (uint8_t)(absIdx % SL_PER_PKT));
    _srvLoadStartMs = millis();
    _srvListDrawn   = false;
    _browser.clearItems();
    _browser.setStatusText("Receiving song...");
    _d.fillScreen(COL_WHITE);
    _browser.draw();
    _d.paintLater();
}

// Open the keyboard to name a new song.  The blank song isn't created until a
// non-empty name is confirmed (see the NAMING_NEW poll branch).
void SongPage::beginNewSong() {
    _pendingSaveName[0] = '\0';
    _keyboard.open(_pendingSaveName, 9);
    _state = State::NAMING_NEW;
    _d.fillScreen(COL_WHITE);
    _browser.draw();
    _keyboard.draw();
    _d.paintLater();
}

// True if `name` already exists in the accumulated server song list (the
// browser holds every page fetched in SERVER_BROWSE).  Case-insensitive to
// match the server's own filename comparison.
bool SongPage::nameExistsOnServer(const char* name) const {
    for (int i = 0; i < _browser.itemCount(); i++)
        if (strcasecmp(_browser.itemName(i), name) == 0) return true;
    return false;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void SongPage::setServerLoadedName(const char* name) {
    if (name && name[0]) {
        strncpy(_srvLoadedName, name, sizeof(_srvLoadedName) - 1);
        _srvLoadedName[sizeof(_srvLoadedName) - 1] = '\0';
    } else {
        _srvLoadedName[0] = '\0';
    }
}

void SongPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}

// ── Boot button alt mode ───────────────────────────────────────────────────────

void SongPage::onBootPress() {
    if (_state == State::SAVING_AS_SRV || _state == State::NAMING_NEW) {
        _keyboard.toggleSymbolLayer();
    } else if (_state == State::SERVER_BROWSE) {
        _browser.onBootPress();
    }
    // Ignore on CONFIRM_* / NOT_CONNECTED states
}
