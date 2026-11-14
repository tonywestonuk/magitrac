#include "SongPage.h"
#include "ServerPairing.h"
#include "Autosave.h"
#include <Arduino.h>
#include <string.h>
#include <stdio.h>

// Size passed to the name keyboard (it counts the terminator): song names are
// typed up to 8 characters — well inside what a song-list entry carries whole
// (SL_NAME_LEN - 5, see startServerLoad()).
static const uint8_t SONG_NAME_INPUT_MAX = 9;

// Header-bar banners.  SAVE FAILED: the save never left (sendSaveActive()
// false), or the server answered that it could not write it.  NOT CONNECTED:
// a load was asked for with no link, or the link dropped while it was awaited.
static const char* const SAVE_FAILED_TITLE   = "SAVE FAILED";
static const char* const NOT_CONNECTED_TITLE = "NOT CONNECTED";

// File stem ("<name>", no ".mgt") of the song push the server last completed;
// "" when it reports no file.  Copied out at once — the pairing layer rewrites
// its buffer on the link task when the next push lands.
static void pushedSongStem(char* out, size_t outSize) {
    const char* f = gServerPairing.pushedSongFile();
    strncpy(out, f ? f : "", outSize - 1);
    out[outSize - 1] = '\0';
    size_t n = strlen(out);
    if (n >= 4 && strcasecmp(out + n - 4, ".mgt") == 0) out[n - 4] = '\0';
}

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
    _srvRequestedName[0]     = '\0';
}

// ── open() ────────────────────────────────────────────────────────────────────

void SongPage::open() {
    _wasDown = _touch.isTouched;
    // Do NOT clear _loadedFilename — preserves current file across opens.
    _srvAwaitingSong     = false;     // no load of this page's is in flight
    _srvReceivingShown   = false;
    _srvRerequested      = false;
    _srvRequestedName[0] = '\0';

    if (gServerPairing.isPaired()) {
        // Connected to server — open server browser directly.  The server list
        // is a drag-scroll list that accumulates every page into one list.
        gServerPairing.resetBrowse();
        gServerPairing.clearLoadFail();   // a stale failure isn't this page's
        gServerPairing.requestSongList(0);
        _srvPage      = 0;
        _srvListDrawn = false;
        _state = State::SERVER_BROWSE;
        _browser.open();
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
            if (_dialog.confirmed() &&
                !gServerPairing.sendSaveActive(_srvLoadedName)) {
                // YES, but the save never left (link down).  Keep the edits
                // and the dirty flag, and abandon the pending load / NEW
                // rather than replace a song that was not saved.
                _pendingNew = false;
                _state = State::SERVER_BROWSE;
                _browser.setTitle(SAVE_FAILED_TITLE);
                _d.fillScreen(COL_WHITE);
                _browser.draw();
                _d.paintLater();
                return SongPageResult::NONE;
            }
            if (!_pendingNew && !gServerPairing.isPaired()) {
                // NO, but the link has gone: the load cannot happen, so the
                // edits are not discarded either (startServerLoad() would only
                // say NOT CONNECTED — say it here, before marking clean).
                _state = State::SERVER_BROWSE;
                showListBanner(NOT_CONNECTED_TITLE);
                return SongPageResult::NONE;
            }
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
                char prevName[sizeof(_song.name)];
                memcpy(prevName, _song.name, sizeof(prevName));
                strncpy(_song.name, _pendingSaveName, sizeof(_song.name) - 1);
                _song.name[sizeof(_song.name) - 1] = '\0';
                // Patch the server's in-memory song.name to match (so the
                // saved file carries the new display name), then ask it to
                // write to SD.  No full-song stream needed — server already
                // has every edit via the patch / note-set stream.
                gServerPairing.sendSongPatch(_song, _song.name, sizeof(_song.name));
                // "Sent" is all a true return means; the server's verdict
                // arrives later (saveResult*, handled by the main loop, which
                // re-marks the song dirty and calls showSaveFailed()).
                if (gServerPairing.sendSaveActive(_pendingSaveName)) {
                    markSongClean();
                    setServerLoadedName(_pendingSaveName);
                    _browser.setTitle("SERVER");
                    // Refresh list so the newly saved file appears
                    restartServerList("Saving...");
                } else {
                    // Never left (link down).  The song is still the file it
                    // was loaded from: keep that as the SAVE / autosave target
                    // and put its name back, so a later save cannot write the
                    // old file under the new name.  Stays dirty; list stays.
                    memcpy(_song.name, prevName, sizeof(prevName));
                    gServerPairing.sendSongPatch(_song, _song.name, sizeof(_song.name));
                    _browser.setTitle(SAVE_FAILED_TITLE);
                }
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
                _srvAwaitingSong   = false;     // any load in flight is moot now
                _srvReceivingShown = false;
                if (gServerPairing.sendSaveActive(_pendingSaveName)) {
                    markSongClean();
                    setServerLoadedName(_pendingSaveName);
                } else {
                    // The save never left, so no server file has this name —
                    // and the blank song must not keep the PREVIOUS song's
                    // file as its SAVE / autosave target either.  Unnamed and
                    // dirty until a SAVE AS goes through.
                    _srvLoadedName[0] = '\0';
                }
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

        // A song push completed.
        if (bs == BrowseState::SONG_READY) {
            char stem[SRV_NAME_MAX];
            pushedSongStem(stem, sizeof(stem));
            if (_srvAwaitingSong && !_srvRerequested &&
                _srvRequestedName[0] != '\0' &&
                strcasecmp(stem, _srvRequestedName) != 0) {
                // Not the song this page asked for — e.g. the reconnect pull
                // of the previous song landing ahead of our reply.  Take
                // nothing from it; ask again and keep waiting.  Once only:
                // whatever arrives after the re-request is accepted, so two
                // sides that disagree about a name cannot loop.
                Serial.printf("[SongPage] got '%s' while waiting for '%s' — re-requesting\n",
                              stem, _srvRequestedName);
                _srvRerequested = true;
                gServerPairing.requestSongLoadByName(_srvRequestedName);
                _srvLoadStartMs = millis();
            } else if (gServerPairing.copySong(&_song)) {
                _loadedFilename[0] = '\0';
                // Identity is the FILE the server read, not the name field
                // inside it (which can be blank or out of step with the file).
                setServerLoadedName(stem[0] != '\0' ? stem : _song.name);
                _srvAwaitingSong   = false;
                _srvReceivingShown = false;
                gServerPairing.resetBrowse();
                return SongPageResult::SONG_LOADED;
            } else {
                // Corrupt data — show the list again
                _srvAwaitingSong = false;
                restartServerList("Loading...");
            }
        }

        // The server answered the load with a failure (file missing, SD read
        // error, not a song…).  It kept its previous song and so do we — just
        // bring the list back straight away.  The reason goes in the header
        // bar, which stays put once the list has redrawn over the status line.
        if (gServerPairing.loadFailPending()) {
            char title[24];
            snprintf(title, sizeof(title), "LOAD FAILED - %s",
                     gServerPairing.loadFailText());
            gServerPairing.clearLoadFail();
            Serial.printf("[SongPage] %s\n", title);
            _browser.setTitle(title);
            _srvAwaitingSong = false;         // that was the answer
            _srvLoadSlow = true;              // not a slow link — no second notice
            restartServerList("Load failed on server");
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
        }

        // The load in flight ended with neither a song nor a failure report:
        // the link dropped (the pairing layer resets the browse state when it
        // does).  Nothing more will arrive for that request, so give up on it
        // and bring the list back rather than sit on "Receiving song...".
        if (_srvAwaitingSong &&
            (!gServerPairing.isPaired() ||
             (_srvReceivingShown && bs != BrowseState::WAITING_SONG &&
                                    bs != BrowseState::SONG_READY))) {
            Serial.println("[SongPage] link dropped while waiting for a song");
            _srvAwaitingSong = false;
            if (_srvReceivingShown) {
                _srvReceivingShown = false;
                _browser.restoreItems();
            }
            if (_browser.itemCount() == 0) _browser.setStatusText("Not connected");
            showListBanner(NOT_CONNECTED_TITLE);
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

        // Slow load (8 s) — bring the list back so the page stays usable, but
        // do NOT resetBrowse(): that zeroes the receive counters under a
        // stream that is still arriving, so a song that lands at 9 s could
        // never complete.  On a crowded 2.4 GHz channel TCP finishes late,
        // not never — the SONG_READY check above still loads it whenever it
        // lands.  If the socket really died, the reconnect pull recovers.
        if (_srvAwaitingSong && bs == BrowseState::WAITING_SONG && !_srvLoadSlow &&
            millis() - _srvLoadStartMs > 8000) {
            _srvLoadSlow = true;
            Serial.println("[SongPage] slow server load — keeping the stream, showing list");
            restartServerList("Slow link - still receiving...");
        }

        FileBrowserResult r = _browser.poll();
        switch (r.event) {

        case FileBrowserEvent::HOME:
            gServerPairing.resetBrowse();
            return SongPageResult::HOME;

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
            if (gServerPairing.sendSaveActive(_srvLoadedName)) {
                markSongClean();
                _browser.setTitle("SERVER");   // drop any earlier failure banner
                restartServerList("Saving...");
            } else {
                // Never left (link down): keep the list and the dirty flag,
                // and say so in the header bar.
                _browser.setTitle(SAVE_FAILED_TITLE);
            }
            _d.fillScreen(COL_WHITE);
            _browser.draw();
            _d.paintLater();
            break;

        case FileBrowserEvent::SAVE_AS:
            strncpy(_pendingSaveName, _song.name, sizeof(_pendingSaveName) - 1);
            _pendingSaveName[sizeof(_pendingSaveName) - 1] = '\0';
            _keyboard.open(_pendingSaveName, SONG_NAME_INPUT_MAX);
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
    _srvReceivingShown = false;   // the list is on its way back in
    _browser.clearItems();
    _browser.setStatusText(status);
    gServerPairing.requestSongList(0);
}

// Request the server song at absolute list index and show "Receiving song...".
// Asked for by name where the list entry is known to be whole; otherwise by
// the flat list index mapped to the server's page/index (same sorted order).
void SongPage::startServerLoad(int absIdx) {
    if (!gServerPairing.isPaired()) {
        // No link: the request would go nowhere and "Receiving song..." would
        // never end.  Keep the list and say why nothing happened.
        showListBanner(NOT_CONNECTED_TITLE);
        return;
    }
    // Ask by name: an index only means something against the directory scan
    // the server made for the list, and it rescans for the load — a scan that
    // comes back different (or short) would fetch the wrong song, or none.
    // A list entry carries SL_NAME_LEN - 1 chars of "<name>.mgt", so a name of
    // up to SL_NAME_LEN - 5 chars is known to be whole; a longer one may be
    // truncated, and only the index can still identify that file.
    const char* nm = _browser.itemName(absIdx);
    _srvRequestedName[0] = '\0';
    if (nm[0] != '\0' && strlen(nm) <= SL_NAME_LEN - 5) {
        // Remembered so a different song landing first is not taken for it.
        strncpy(_srvRequestedName, nm, sizeof(_srvRequestedName) - 1);
        _srvRequestedName[sizeof(_srvRequestedName) - 1] = '\0';
        gServerPairing.requestSongLoadByName(_srvRequestedName);
    } else {
        // By index: no name to check the reply against — accept what comes.
        gServerPairing.requestSongLoad((uint8_t)(absIdx / SL_PER_PKT),
                                       (uint8_t)(absIdx % SL_PER_PKT));
    }
    _srvAwaitingSong   = true;
    _srvReceivingShown = true;
    _srvRerequested    = false;
    _browser.setTitle("SERVER");       // drop any earlier banner
    _srvLoadStartMs = millis();
    _srvLoadSlow    = false;
    _srvListDrawn   = false;
    // Set the list aside rather than clear it: if the link drops, it comes
    // straight back (there would be no way to fetch it again).
    _browser.hideItems();
    _browser.setStatusText("Receiving song...");
    _d.fillScreen(COL_WHITE);
    _browser.draw();
    _d.paintLater();
}

void SongPage::showListBanner(const char* title) {
    _browser.setTitle(title);
    _d.fillScreen(COL_WHITE);
    _browser.draw();
    _d.paintLater();
}

// Open the keyboard to name a new song.  The blank song isn't created until a
// non-empty name is confirmed (see the NAMING_NEW poll branch).
void SongPage::beginNewSong() {
    _pendingSaveName[0] = '\0';
    _keyboard.open(_pendingSaveName, SONG_NAME_INPUT_MAX);
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

void SongPage::adoptPushedSongName() {
    char stem[SRV_NAME_MAX];
    pushedSongStem(stem, sizeof(stem));
    setServerLoadedName(stem[0] != '\0' ? stem : _song.name);
}

void SongPage::showSaveFailed() {
    _browser.setTitle(SAVE_FAILED_TITLE);
    // Under a keyboard / dialog (or the not-connected notice) the banner
    // waits for the next browser draw.
    if (_state == State::SERVER_BROWSE) {
        _d.fillScreen(COL_WHITE);
        _browser.draw();
        _d.paintLater();
    }
}

void SongPage::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = 540 - rx;
}
