import Foundation
import MagiTrackCore

/// Decodes a Song from the MagiLink wire format the server uses for the
/// `MSG_SONG_PUSH_*` stream: an 8-byte `SongFileHeader` followed by a
/// **raw C `Song` struct dump** (36176 bytes — fixed). That's different
/// from the `.mgt` v19 file format, which is compact (sorted SerializedNote
/// stream) — both produce the same Swift `Song`, but via different bytes.
///
/// Reference: `magitrac/ServerPairing.cpp:794–799` does
///   `memcpy(out, _songBuf + sizeof(SongFileHeader), sizeof(Song))`
/// on the C side. We replicate that byte-for-byte.
public enum WireSongCodec {

    // Layout constants — locked by C struct sizes on the server side.
    public static let headerSize       = 8
    public static let noteNodeSize     = 8
    public static let notePoolCount    = 4000
    public static let notePoolSize     = noteNodeSize * notePoolCount        // 32000
    public static let noteFreeHeadSize = 2
    public static let patternSize      = 72
    public static let patternCount     = 50
    public static let patternsSize     = patternSize * patternCount          // 3600
    public static let columnSize       = 20
    public static let columnCount      = 21
    public static let columnsSize      = columnSize * columnCount            // 420
    public static let songTailSize     = 154
    public static let totalSongStruct  = notePoolSize + noteFreeHeadSize
                                       + patternsSize + columnsSize + songTailSize  // 36176
    public static let totalWireSize    = headerSize + totalSongStruct        // 36184

    public enum Error: Swift.Error, Equatable {
        case wrongTotalSize(expected: Int, got: Int)
        case badMagic(UInt32)
        case unsupportedVersion(UInt8)
        case truncated
        case malformedNoteList(pattern: Int)
    }

    public static func decode(_ data: Data) throws -> Song {
        guard data.count >= totalWireSize else {
            throw Error.wrongTotalSize(expected: totalWireSize, got: data.count)
        }
        var r = ByteReader(data)
        try readAndCheckHeader(&r)
        let pool = try readNotePool(&r)
        _ = try r.u16()                              // noteFreeHead — ignored
        let rawPatterns = try readPatterns(&r)
        let columns     = try readColumns(&r)
        var song        = try readSongTail(&r,
                                           rawPatterns: rawPatterns,
                                           pool: pool,
                                           columns: columns)
        _ = song // suppress unused warning before the assemble step
        // Patterns already built with notes inside rawPatterns → song
        return song
    }

    // ── Header ──────────────────────────────────────────────────────────

    private static func readAndCheckHeader(_ r: inout ByteReader) throws {
        let magic = try r.u32()
        guard magic == MagiTrack.songFileMagic else { throw Error.badMagic(magic) }
        let version = try r.u8()
        guard version == MagiTrack.songFileVersion else { throw Error.unsupportedVersion(version) }
        try r.skip(3) // _pad[3]
    }

    // ── Note pool ───────────────────────────────────────────────────────

    /// In-memory NoteNode used only during decode — kept here (not exposed)
    /// because the Swift model has no pool concept above the wire layer.
    private struct RawNote {
        let row: UInt8
        let col: UInt8
        let note: UInt8
        let velocity: UInt8
        let effect: UInt8
        let param: UInt8
        let next: UInt16
    }

    private static func readNotePool(_ r: inout ByteReader) throws -> [RawNote] {
        var pool: [RawNote] = []
        pool.reserveCapacity(notePoolCount)
        for _ in 0..<notePoolCount {
            let row  = try r.u8()
            let col  = try r.u8()
            let note = try r.u8()
            let vel  = try r.u8()
            let eff  = try r.u8()
            let par  = try r.u8()
            let next = try r.u16()
            pool.append(RawNote(row: row, col: col, note: note,
                                velocity: vel, effect: eff, param: par,
                                next: next))
        }
        return pool
    }

    // ── Patterns ────────────────────────────────────────────────────────
    //
    // Same 72-byte layout as the file format, but here we honour the
    // `noteHead` value (the file format zeroes it on load). Returned as
    // (Pattern, noteHead) pairs so the caller can build the notes dict by
    // walking the pool.

    private static func readPatterns(_ r: inout ByteReader) throws -> [(pattern: Pattern, noteHead: UInt16)] {
        var out: [(Pattern, UInt16)] = []
        out.reserveCapacity(patternCount)
        for _ in 0..<patternCount {
            out.append(try readPattern(&r))
        }
        return out
    }

    private static func readPattern(_ r: inout ByteReader) throws -> (Pattern, UInt16) {
        let noteHead     = try r.u16()
        let length       = try r.u8()
        let referenceNote = try r.u8()
        var inputs: [InputNoteEntry] = []
        inputs.reserveCapacity(12)
        for _ in 0..<12 {
            let sm = try r.u8()
            let st = try r.u8()
            let ta = try r.u8()
            let tv = try r.i8()
            inputs.append(InputNoteEntry(
                switchMode: BlockSwitch(rawValue: sm) ?? .stay,
                switchTarget: st,
                transposeAction: TransposeAction(rawValue: ta) ?? .keep,
                transposeValue: tv))
        }
        let name = try r.cstr(width: 16)
        let kcm  = try r.u8()
        let nav  = try r.u8()
        try r.skip(2) // _padOld
        let pat = Pattern(
            length: length,
            referenceNote: referenceNote,
            inputNotes: inputs,
            name: name,
            keyChangeMode: KeyChangeMode(rawValue: kcm) ?? .samePos,
            blockEndNav: BlockEndNav(rawByte: nav),
            notes: [:])
        return (pat, noteHead)
    }

    // ── Columns ─────────────────────────────────────────────────────────
    // Identical to file format.

    private static func readColumns(_ r: inout ByteReader) throws -> [ColumnSettings] {
        var out: [ColumnSettings] = []
        out.reserveCapacity(columnCount)
        for _ in 0..<columnCount {
            let mc = try r.u8()
            let bm = try r.u8()
            let pr = try r.u8()
            let vo = try r.u8()
            let tr = try r.i8()
            let mu = try r.u8()
            try r.skip(2) // _pad
            let nm = try r.cstr(width: 12)
            out.append(ColumnSettings(
                midiChannel: mc, bankMSB: bm, program: pr, volume: vo,
                transpose: tr, mute: mu != 0, name: nm))
        }
        return out
    }

    // ── Song tail + note-pool walk ──────────────────────────────────────
    //
    // Tail layout matches the file format exactly (the C code writes it
    // raw both times). We parse it the same way then walk each pattern's
    // pool linked list to build its sparse notes dictionary.

    private static func readSongTail(_ r: inout ByteReader,
                                     rawPatterns: [(pattern: Pattern, noteHead: UInt16)],
                                     pool: [RawNote],
                                     columns: [ColumnSettings]) throws -> Song {
        let numPatterns   = try r.u8()
        let startPattern  = try r.u8()
        let bpm           = try r.u16()
        let minBPM        = try r.u16()
        let maxBPM        = try r.u16()
        let speed         = try r.u8()
        let name          = try r.cstr(width: 32)
        let midiInChannel = try r.u8()
        let midiInNoteMin = try r.u8()
        let midiInNoteMax = try r.u8()
        let performerMask = try r.u8()
        try r.skip(1) // alignment padding
        let transposeChMask = try r.u16()
        var perfPads: [PerfPadConfig] = []
        perfPads.reserveCapacity(MagiTrack.perfPadCount)
        for _ in 0..<MagiTrack.perfPadCount {
            let nm = try r.cstr(width: 12)
            let md = try r.u8()
            perfPads.append(PerfPadConfig(
                name: nm,
                mode: PerfPadConfig.Mode(rawValue: md) ?? .immediate))
        }
        try r.skip(1) // _songPad
        try r.skip(1) // trailing struct alignment

        // Walk pool linked-lists into pattern.notes dictionaries
        var patterns: [Pattern] = []
        let live = min(Int(numPatterns), rawPatterns.count)
        patterns.reserveCapacity(live)
        for i in 0..<live {
            var pat  = rawPatterns[i].pattern
            let head = rawPatterns[i].noteHead
            pat.notes = try walkPool(pool: pool, head: head, patternIndex: i)
            patterns.append(pat)
        }

        return Song(
            patterns: patterns,
            columns: columns,
            startPattern: startPattern,
            bpm: bpm, minBPM: minBPM, maxBPM: maxBPM,
            speed: speed,
            name: name,
            midiInChannel: midiInChannel,
            midiInNoteMin: midiInNoteMin,
            midiInNoteMax: midiInNoteMax,
            performerMask: performerMask,
            transposeChMask: transposeChMask,
            perfPads: perfPads)
    }

    /// Walk the circular linked list rooted at `head` through `pool`,
    /// emitting (row,col) → Note. Returns empty if head == NOTE_NULL.
    /// Guarded against malformed lists by capping iterations at
    /// pool.count (no list can legitimately exceed the pool size).
    private static func walkPool(pool: [RawNote], head: UInt16, patternIndex: Int) throws -> [GridKey: Note] {
        let NOTE_NULL: UInt16 = 0xFFFF
        guard head != NOTE_NULL else { return [:] }
        guard Int(head) < pool.count else {
            throw Error.malformedNoteList(pattern: patternIndex)
        }
        var notes: [GridKey: Note] = [:]
        var idx = head
        var hops = 0
        let maxHops = pool.count + 1
        repeat {
            guard Int(idx) < pool.count else {
                throw Error.malformedNoteList(pattern: patternIndex)
            }
            let raw = pool[Int(idx)]
            let key = GridKey(row: raw.row, col: raw.col)
            notes[key] = Note(note: raw.note, velocity: raw.velocity,
                              effect: raw.effect, param: raw.param)
            idx = raw.next
            hops += 1
            if hops > maxHops { throw Error.malformedNoteList(pattern: patternIndex) }
        } while idx != head
        return notes
    }
}
