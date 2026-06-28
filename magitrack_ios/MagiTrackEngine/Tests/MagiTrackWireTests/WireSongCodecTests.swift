import XCTest
@testable import MagiTrackWire
@testable import MagiTrackCore

/// Tests for the wire format (raw C `Song` struct dump). We can't test
/// round-trip end-to-end without an encoder (Phase 5), but we can
/// synthesise a byte image that matches the C layout and verify it
/// decodes correctly.
final class WireSongCodecTests: XCTestCase {

    func testTotalSizeMath() {
        XCTAssertEqual(WireSongCodec.notePoolSize, 32000)
        XCTAssertEqual(WireSongCodec.totalSongStruct, 36176)
        XCTAssertEqual(WireSongCodec.totalWireSize, 36184)
    }

    func testRejectsTruncated() {
        let small = Data(count: 100)
        XCTAssertThrowsError(try WireSongCodec.decode(small))
    }

    func testRejectsBadMagic() {
        var w = ByteWriter()
        // Bad magic, but enough bytes overall to get past the size check
        w.u32(0xDEADBEEF)
        w.u8(MagiTrack.songFileVersion)
        w.pad(3)
        w.pad(WireSongCodec.totalSongStruct)
        XCTAssertThrowsError(try WireSongCodec.decode(w.data))
    }

    func testRejectsBadVersion() {
        var w = ByteWriter()
        w.u32(MagiTrack.songFileMagic)
        w.u8(18) // wrong version
        w.pad(3)
        w.pad(WireSongCodec.totalSongStruct)
        XCTAssertThrowsError(try WireSongCodec.decode(w.data))
    }

    /// Build a minimal valid wire image: header + zeroed pool + one
    /// pattern with one linked-list note + tail saying numPatterns=1.
    func testDecodesSinglePatternSingleNote() throws {
        let data = makeWireImageOnePatternOneNote()
        XCTAssertEqual(data.count, WireSongCodec.totalWireSize)

        let song = try WireSongCodec.decode(data)
        XCTAssertEqual(song.patterns.count, 1)
        XCTAssertEqual(song.patterns[0].length, 16)
        XCTAssertEqual(song.patterns[0].name, "TEST")
        XCTAssertEqual(song.patterns[0].notes.count, 1)
        let key = GridKey(row: 4, col: 1)
        XCTAssertEqual(song.patterns[0].notes[key]?.note, 49)
        XCTAssertEqual(song.patterns[0].notes[key]?.velocity, 100)
        XCTAssertEqual(song.bpm, 120)
        XCTAssertEqual(song.name, "ROUNDTRIP")
    }

    /// A multi-note pattern: 3 notes forming a circular linked list.
    func testDecodesCircularLinkedList() throws {
        let data = makeWireImageOnePatternThreeNotes()
        let song = try WireSongCodec.decode(data)
        XCTAssertEqual(song.patterns[0].notes.count, 3)
        // Notes are at (1,1), (2,1), (3,1) — all C-4 (49)
        XCTAssertNotNil(song.patterns[0].notes[GridKey(row: 1, col: 1)])
        XCTAssertNotNil(song.patterns[0].notes[GridKey(row: 2, col: 1)])
        XCTAssertNotNil(song.patterns[0].notes[GridKey(row: 3, col: 1)])
    }

    // ── Fixture builders ────────────────────────────────────────────────

    private func makeWireImageOnePatternOneNote() -> Data {
        var w = ByteWriter()
        // Header
        w.u32(MagiTrack.songFileMagic)
        w.u8(MagiTrack.songFileVersion)
        w.pad(3)
        // NotePool — one occupied node at index 0, rest zero
        appendNoteNode(into: &w, row: 4, col: 1, note: 49, vel: 100, eff: 0, par: 0, next: 0)
        // Remaining pool: 3999 zero NoteNodes
        for _ in 1..<WireSongCodec.notePoolCount {
            appendNoteNode(into: &w, row: 0, col: 0, note: 0, vel: 0, eff: 0, par: 0, next: 0)
        }
        // noteFreeHead
        w.u16(1)
        // Pattern[0]: noteHead=0, length=16, name="TEST"
        appendPattern(into: &w, noteHead: 0, length: 16, name: "TEST")
        // Patterns[1..49] zeroed
        for _ in 1..<WireSongCodec.patternCount {
            w.pad(WireSongCodec.patternSize)
        }
        // Columns[21] zeroed
        for _ in 0..<WireSongCodec.columnCount {
            w.pad(WireSongCodec.columnSize)
        }
        // Song tail
        appendSongTail(into: &w, numPatterns: 1, songName: "ROUNDTRIP")
        return w.data
    }

    private func makeWireImageOnePatternThreeNotes() -> Data {
        var w = ByteWriter()
        // Header
        w.u32(MagiTrack.songFileMagic)
        w.u8(MagiTrack.songFileVersion)
        w.pad(3)
        // NotePool — 3 occupied at indices 0,1,2 forming circular list
        //   pool[0]: row 1, next → 1
        //   pool[1]: row 2, next → 2
        //   pool[2]: row 3, next → 0 (wraps)
        appendNoteNode(into: &w, row: 1, col: 1, note: 49, vel: 100, eff: 0, par: 0, next: 1)
        appendNoteNode(into: &w, row: 2, col: 1, note: 49, vel: 100, eff: 0, par: 0, next: 2)
        appendNoteNode(into: &w, row: 3, col: 1, note: 49, vel: 100, eff: 0, par: 0, next: 0)
        for _ in 3..<WireSongCodec.notePoolCount {
            appendNoteNode(into: &w, row: 0, col: 0, note: 0, vel: 0, eff: 0, par: 0, next: 0)
        }
        w.u16(3) // noteFreeHead
        appendPattern(into: &w, noteHead: 0, length: 16, name: "TEST")
        for _ in 1..<WireSongCodec.patternCount {
            w.pad(WireSongCodec.patternSize)
        }
        for _ in 0..<WireSongCodec.columnCount {
            w.pad(WireSongCodec.columnSize)
        }
        appendSongTail(into: &w, numPatterns: 1, songName: "")
        return w.data
    }

    private func appendNoteNode(into w: inout ByteWriter,
                                row: UInt8, col: UInt8, note: UInt8,
                                vel: UInt8, eff: UInt8, par: UInt8, next: UInt16) {
        w.u8(row); w.u8(col); w.u8(note); w.u8(vel); w.u8(eff); w.u8(par)
        w.u16(next)
    }

    private func appendPattern(into w: inout ByteWriter,
                               noteHead: UInt16, length: UInt8, name: String) {
        w.u16(noteHead)
        w.u8(length)
        w.u8(0) // referenceNote
        w.pad(12 * 4) // inputNotes[12] all zeroed
        w.cstr(name, width: 16)
        w.u8(0) // keyChangeMode
        w.u8(0) // blockEndNav (loop)
        w.pad(2) // _padOld
    }

    private func appendSongTail(into w: inout ByteWriter,
                                numPatterns: UInt8, songName: String) {
        w.u8(numPatterns)
        w.u8(0)                                // startPattern
        w.u16(120); w.u16(60); w.u16(240)      // bpm/min/max
        w.u8(6)                                // speed
        w.cstr(songName, width: 32)
        w.u8(0); w.u8(0); w.u8(127); w.u8(0)   // midiInChannel/min/max, performerMask
        w.pad(1)                               // alignment
        w.u16(0xFDFF)                          // transposeChMask
        for _ in 0..<MagiTrack.perfPadCount {
            w.cstr("", width: 12)
            w.u8(0)
        }
        w.pad(1) // _songPad
        w.pad(1) // trailing alignment
    }
}
