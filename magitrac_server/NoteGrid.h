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

    // Const constructor — read-only operations only (set/clear are no-ops).
    NoteGrid(const NoteNode* pool, const uint16_t* noteHead)
        : _pool(const_cast<NoteNode*>(pool))
        , _freeHead(nullptr)
        , _noteHead(const_cast<uint16_t*>(noteHead)) {}

    // Returns the Note at (row, col), or an all-zero Note if absent.
    Note get(uint8_t row, uint8_t col) const;

    // Insert or overwrite a note.  Clears the cell if n is all-zero.
    // Returns false if the pool is full and a new node was needed.
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

    // Returns the lowest row > afterRow that has at least one note, or 0xFF if none.
    uint8_t nextOccupiedRow(uint8_t afterRow) const;

    // Number of notes currently in this pattern.
    uint16_t count() const;

private:
    NoteNode* _pool;
    uint16_t* _freeHead;
    uint16_t* _noteHead;

    // Returns the sort key used to order nodes.
    static uint16_t _key(uint8_t row, uint8_t col) { return ((uint16_t)row << 8) | col; }

    // Allocate a node from the free list. Returns NOTE_NULL if exhausted.
    uint16_t _alloc();

    // Return a node to the free list.
    void _free(uint16_t idx);
};
