#pragma once
#include "TrackerData.h"

// ── NoteGrid ──────────────────────────────────────────────────────────────────
// Abstracts the sparse circular linked list for one pattern's notes.
// The pool and free list live in Song; this class holds only pointers into them.
//
// List invariant: nodes are kept sorted by (row, col).  The last node's next
// field equals *_noteHead, making the list circular.  An empty pattern has
// *_noteHead == NOTE_NULL.

class NoteGrid {
public:
    // Mutable constructor — all operations available.
    NoteGrid(NoteNode* pool, uint16_t* freeHead, uint16_t* noteHead)
        : _pool(pool), _freeHead(freeHead), _noteHead(noteHead) {}

    // Const constructor — read-only grid.  There is no free list (_freeHead is
    // null), so set() / clear() / clearAll() refuse to run: set() returns
    // false, the other two return without touching anything.
    NoteGrid(const NoteNode* pool, const uint16_t* noteHead)
        : _pool(const_cast<NoteNode*>(pool))
        , _freeHead(nullptr)
        , _noteHead(const_cast<uint16_t*>(noteHead)) {}

    // Returns the Note at (row, col), or an all-zero Note if absent.
    // NB: all-zero is NOT the "empty" encoding set() looks for (velocity 0 is
    // an explicit value) — use has() to tell an absent cell from a stored one.
    Note get(uint8_t row, uint8_t col) const;

    // Insert or overwrite a note.  Clears the cell instead if n is empty:
    // note 0, effect 0, param 0 and a "use default" velocity (high bit set,
    // e.g. VEL_DEFAULT).  A velocity of 0–127 alone is enough to keep the cell.
    // Returns false if the pool is full and a new node was needed, or if this
    // is a read-only grid.
    bool set(uint8_t row, uint8_t col, const Note& n);

    // Remove the note at (row, col). No-op if absent.
    void clear(uint8_t row, uint8_t col);

    // Remove all notes in this pattern, returning every node to the free list.
    void clearAll();

    // Returns true if any node exists at (row, col).
    bool has(uint8_t row, uint8_t col) const;

    // Call fn(col, note) for every note in the given row, in column order.
    void forRow(uint8_t row, void (*fn)(uint8_t col, const Note& n, void* ctx), void* ctx) const;

    // Call fn(row, col, note) for every note in the pattern, in (row,col) order.
    void forAll(void (*fn)(uint8_t row, uint8_t col, const Note& n, void* ctx), void* ctx) const;

private:
    NoteNode* _pool;
    uint16_t* _freeHead;   // null for a read-only (const-constructed) grid
    uint16_t* _noteHead;

    // Returns the sort key used to order nodes.
    static uint16_t _key(uint8_t row, uint8_t col) { return ((uint16_t)row << 8) | col; }

    // Allocate a node from the free list. Returns NOTE_NULL if exhausted.
    uint16_t _alloc();

    // Return a node to the free list.
    void _free(uint16_t idx);
};
