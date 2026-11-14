#pragma once

// Called from any edit site that mutates the currently-loaded Song (in
// practice ServerPairing::sendSongPatch / sendNoteSet, plus the drum-import
// paths in magitrac.ino).  Resets a 30-second countdown; when it elapses (and
// we're sitting on the tracker page, linked, with a known server filename)
// the main loop asks the server to autosave, with a brief "Saving..."
// indicator in the bottom-left corner.
void markSongDirty();

// Called by SongPage once a save has been sent to the server, or the user
// chose to discard the edits.  (The autosave clears the flag itself.)
void markSongClean();

// True if the loaded song has unsaved edits since the last save / load.
bool songIsDirty();
