#pragma once

// Umbrella header for magitrac_lib — the shared core between
// the magitrac client (UI) and magitrac_server (MIDI player).
// Including this in the .ino entry point triggers Arduino's
// library discovery and puts magitrac_lib/src on the include path.

#include "TrackerData.h"
#include "NoteGrid.h"
#include "SongMigration.h"
#include "MagiMsg.h"
#include "MagiComms.h"
#include "MagiCommsEspNow.h"
#include "PairNVS.h"
