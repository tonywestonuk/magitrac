#pragma once
#include <stdint.h>

// Tracks where the currently-loaded song came from.
// Used to prefix the song name display and to decide whether to sync edits.
enum class SongSource : uint8_t {
    NONE,    // new / unsaved
    SD,      // loaded from local SD card
    SERVER,  // loaded from server over ESP-NOW
};

extern SongSource gSongSource;
