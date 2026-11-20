#pragma once
#include <stddef.h>
#include <stdint.h>

struct Song;

// ── AI: rewrite the active song with the Claude API ─────────────────────────
//
// The loop task (commands_server.ino) owns the song: it calls aiBegin() with
// the current song, which turns it into text (song_text.h), reads the key from
// /ai/key.txt and builds the request, then hands the HTTPS call to a task of
// its own on core 0.  That task streams the answer into a PSRAM buffer and
// finishes; the loop task picks it up with aiTakeReply(), parses it and — only
// if it parses — installs it.  Nothing here touches the active song.
//
// /ai/key.txt (on the server's SD card, put there from a computer):
//   sk-ant-...            the API key (any line without '=')
//   model=claude-opus-5-5 optional
//   effort=medium         optional: low / medium / high / xhigh / max

// False (and why, in err) if it can't start: already busy, no key, not on an
// external network, not enough memory.
bool aiBegin(const Song* current, const char* prompt, char* err, size_t errLen);

void aiCancel();               // the task drops the connection; result = FAILED "Cancelled"
bool aiBusy();                 // a request is in flight (or its reply not yet taken)

uint8_t  aiState();            // AiState (MagiMsg.h) while busy
uint32_t aiBytes();            // reply text received so far
uint32_t aiSeconds();          // since aiBegin

// Once the task has finished: true, with ok and either the reply text or the
// reason it failed.  The reply stays valid until aiRelease().
bool aiTakeReply(bool& ok, const char*& reply, size_t& len, char* err, size_t errLen);
void aiRelease();
