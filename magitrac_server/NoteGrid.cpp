#include "NoteGrid.h"
#include <string.h>

// ── Internal helpers ──────────────────────────────────────────────────────────

uint16_t NoteGrid::_alloc() {
    if (*_freeHead == NOTE_NULL) return NOTE_NULL;
    uint16_t idx = *_freeHead;
    *_freeHead = _pool[idx].next;
    return idx;
}

void NoteGrid::_free(uint16_t idx) {
    _pool[idx].next = *_freeHead;
    *_freeHead = idx;
}

// ── get ───────────────────────────────────────────────────────────────────────

Note NoteGrid::get(uint8_t row, uint8_t col) const {
    if (*_noteHead == NOTE_NULL) return {};
    uint16_t key = _key(row, col);
    uint16_t idx = *_noteHead;
    do {
        NoteNode& n = _pool[idx];
        if (_key(n.row, n.col) == key) {
            Note out;
            out.note      = n.note;
            out.velocity  = n.velocity;
            out.effect    = n.effect;
            out.param     = n.param;
            return out;
        }
        // List is sorted; stop early if we've passed the target key.
        if (_key(n.row, n.col) > key) break;
        idx = n.next;
    } while (idx != *_noteHead);
    return {};
}

// ── has ───────────────────────────────────────────────────────────────────────

bool NoteGrid::has(uint8_t row, uint8_t col) const {
    if (*_noteHead == NOTE_NULL) return false;
    uint16_t key = _key(row, col);
    uint16_t idx = *_noteHead;
    do {
        NoteNode& n = _pool[idx];
        uint16_t k = _key(n.row, n.col);
        if (k == key) return true;
        if (k  > key) return false;
        idx = n.next;
    } while (idx != *_noteHead);
    return false;
}

// ── set ───────────────────────────────────────────────────────────────────────

bool NoteGrid::set(uint8_t row, uint8_t col, const Note& n) {
    // All-zero note means clear.
    if (n.note == 0 && n.effect == 0 && n.param == 0) {
        clear(row, col);
        return true;
    }

    uint16_t key = _key(row, col);

    // ── Update existing node ──────────────────────────────────────────────────
    if (*_noteHead != NOTE_NULL) {
        uint16_t idx = *_noteHead;
        do {
            NoteNode& node = _pool[idx];
            if (_key(node.row, node.col) == key) {
                node.note      = n.note;
                node.velocity  = n.velocity;
                node.effect    = n.effect;
                node.param     = n.param;
                return true;
            }
            idx = node.next;
        } while (idx != *_noteHead);
    }

    // ── Insert new node ───────────────────────────────────────────────────────
    uint16_t newIdx = _alloc();
    if (newIdx == NOTE_NULL) {
        Serial.println("[NoteGrid] WARNING: note pool exhausted");
        return false;
    }

    NoteNode& newNode = _pool[newIdx];
    newNode.row       = row;
    newNode.col       = col;
    newNode.note      = n.note;
    newNode.velocity  = n.velocity;
    newNode.effect    = n.effect;
    newNode.param     = n.param;

    // Empty list — single circular node pointing to itself.
    if (*_noteHead == NOTE_NULL) {
        newNode.next = newIdx;
        *_noteHead   = newIdx;
        return true;
    }

    // Find insertion point: insert before the first node whose key > newKey.
    // Walk the list to find the predecessor of that node.
    uint16_t prev = NOTE_NULL;
    uint16_t cur  = *_noteHead;
    do {
        if (_key(_pool[cur].row, _pool[cur].col) > key) break;
        prev = cur;
        cur  = _pool[cur].next;
    } while (cur != *_noteHead);

    if (prev == NOTE_NULL) {
        // Insert before the current head — find the tail (prev of head) first.
        uint16_t tail = *_noteHead;
        while (_pool[tail].next != *_noteHead) tail = _pool[tail].next;
        newNode.next      = *_noteHead;
        _pool[tail].next  = newIdx;
        *_noteHead        = newIdx;
    } else {
        // Insert after prev, before cur.
        newNode.next     = _pool[prev].next;
        _pool[prev].next = newIdx;
    }

    return true;
}

// ── clear ─────────────────────────────────────────────────────────────────────

void NoteGrid::clear(uint8_t row, uint8_t col) {
    if (*_noteHead == NOTE_NULL) return;
    uint16_t key = _key(row, col);

    uint16_t prev = NOTE_NULL;
    uint16_t idx  = *_noteHead;

    // Find the node with a matching key, tracking its predecessor.
    do {
        NoteNode& n = _pool[idx];
        if (_key(n.row, n.col) == key) {
            // Found. Unlink it.
            if (_pool[idx].next == idx) {
                // Only node — list becomes empty.
                *_noteHead = NOTE_NULL;
            } else {
                if (prev == NOTE_NULL) {
                    // Removing the head — find the tail to update its next.
                    uint16_t tail = idx;
                    while (_pool[tail].next != idx) tail = _pool[tail].next;
                    *_noteHead        = _pool[idx].next;
                    _pool[tail].next  = *_noteHead;
                } else {
                    _pool[prev].next = _pool[idx].next;
                }
            }
            _free(idx);
            return;
        }
        if (_key(n.row, n.col) > key) return;  // past insertion point, not found
        prev = idx;
        idx  = n.next;
    } while (idx != *_noteHead);
}

// ── clearAll ─────────────────────────────────────────────────────────────────

void NoteGrid::clearAll() {
    if (*_noteHead == NOTE_NULL) return;

    // Walk the circular list once, collecting all node indices, then free them.
    uint16_t idx = *_noteHead;
    do {
        uint16_t next = _pool[idx].next;
        _free(idx);
        idx = next;
    } while (idx != *_noteHead);

    *_noteHead = NOTE_NULL;
}

// ── forRow ────────────────────────────────────────────────────────────────────

void NoteGrid::forRow(uint8_t row, void (*fn)(uint8_t col, const Note& n, void* ctx), void* ctx) const {
    if (*_noteHead == NOTE_NULL) return;
    uint16_t idx = *_noteHead;
    do {
        NoteNode& n = _pool[idx];
        if (n.row == row) {
            Note out = { n.note, n.velocity, n.effect, n.param };
            fn(n.col, out, ctx);
        } else if (n.row > row) {
            break;  // sorted — no more notes in this row
        }
        idx = n.next;
    } while (idx != *_noteHead);
}

// ── forAll ────────────────────────────────────────────────────────────────────

void NoteGrid::forAll(void (*fn)(uint8_t row, uint8_t col, const Note& n, void* ctx), void* ctx) const {
    if (*_noteHead == NOTE_NULL) return;
    uint16_t idx = *_noteHead;
    do {
        NoteNode& n = _pool[idx];
        Note out = { n.note, n.velocity, n.effect, n.param };
        fn(n.row, n.col, out, ctx);
        idx = n.next;
    } while (idx != *_noteHead);
}

// ── nextOccupiedRow ───────────────────────────────────────────────────────────

uint8_t NoteGrid::nextOccupiedRow(uint8_t afterRow) const {
    if (*_noteHead == NOTE_NULL) return 0xFF;
    uint16_t idx = *_noteHead;
    do {
        NoteNode& n = _pool[idx];
        if (n.row > afterRow) return n.row;
        idx = n.next;
    } while (idx != *_noteHead);
    return 0xFF;
}

// ── count ─────────────────────────────────────────────────────────────────────

uint16_t NoteGrid::count() const {
    if (*_noteHead == NOTE_NULL) return 0;
    uint16_t c   = 0;
    uint16_t idx = *_noteHead;
    do {
        ++c;
        idx = _pool[idx].next;
    } while (idx != *_noteHead);
    return c;
}
