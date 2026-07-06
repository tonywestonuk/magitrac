#pragma once

// Called from any edit site that mutates the currently-loaded Song.
// Resets a 30-second countdown; when it elapses (and we're sitting on
// the tracker page with a known SD filename) the main loop performs an
// autosave with a brief "Saving..." indicator in the bottom-left corner.
void markSongDirty();

// Called by SongPage::doSave to suppress an immediate redundant autosave.
void markSongClean();

// True if the loaded song has unsaved edits since the last save / load.
bool songIsDirty();
