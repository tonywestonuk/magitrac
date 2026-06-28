import Foundation
import MagiTrackCore

/// `.mgt` v19 codec. Byte-faithful with the C client's raw-struct write — the
/// C code uses `fwrite(&song.patterns, sizeof(song->patterns), ...)` etc., so
/// every compiler-inserted padding byte leaks into the file. The Swift writer
/// emits exactly those padding bytes (always zero).
///
/// File layout (the original "compact" v19):
///   SongFileHeader (8 bytes)
///   Pattern[50]     (72 bytes each = 3600)
///   ColumnSettings[21] (20 bytes each = 420)
///   Song tail (154 bytes — includes 2 padding bytes for struct alignment)
///   uint16  noteCount
///   SerializedNote[noteCount] (7 bytes each)
public enum MgtV19Codec {

    // ── Sizes (locked by .mgt v19 compatibility) ────────────────────────
    public static let headerSize       = 8
    public static let patternSize      = 72
    public static let columnSize       = 20
    public static let perfPadSize      = 13
    public static let songTailSize     = 154
    public static let serializedNoteSize = 7

    /// Encode a Song into the v19 byte stream.
    public static func encode(_ song: Song) -> Data {
        var w = ByteWriter()
        writeHeader(into: &w)
        writePatterns(song, into: &w)
        writeColumns(song, into: &w)
        writeSongTail(song, into: &w)
        writeNotes(song, into: &w)
        return w.data
    }

    /// Decode a v19 byte stream into a Song. Throws if the header is wrong
    /// version or the stream is truncated.
    public static func decode(_ data: Data) throws -> Song {
        var r = ByteReader(data)
        try readAndCheckHeader(&r)
        let patterns = try readPatterns(&r)
        let columns  = try readColumns(&r)
        var song     = try readSongTail(&r, patterns: patterns, columns: columns)
        try readNotes(&r, into: &song)
        return song
    }

    public enum Error: Swift.Error, Equatable {
        case badMagic(UInt32)
        case unsupportedVersion(UInt8)
        case truncated
        case noteCountOverflow(UInt16)
        case noteOnInvalidPattern(UInt8)
    }

    // ── Header ───────────────────────────────────────────────────────────
    private static func writeHeader(into w: inout ByteWriter) {
        w.u32(MagiTrack.songFileMagic)
        w.u8(MagiTrack.songFileVersion)
        w.pad(3)
    }

    private static func readAndCheckHeader(_ r: inout ByteReader) throws {
        let magic = try r.u32()
        guard magic == MagiTrack.songFileMagic else { throw Error.badMagic(magic) }
        let version = try r.u8()
        guard version == MagiTrack.songFileVersion else { throw Error.unsupportedVersion(version) }
        try r.skip(3) // _pad[3]
    }

    // ── Patterns ─────────────────────────────────────────────────────────
    //
    // Written as raw `Pattern patterns[MAX_PATTERNS]`. Each Pattern is 72
    // bytes:
    //   u16 noteHead        (loader ignores; always 0xFFFF on write for
    //                        clarity — could be any value)
    //   u8  length
    //   u8  referenceNote
    //   InputNoteEntry inputNotes[12]  (4 bytes each = 48)
    //   char name[16]
    //   u8  keyChangeMode
    //   u8  blockEndNav
    //   u8  _padOld[2]
    private static func writePatterns(_ song: Song, into w: inout ByteWriter) {
        for i in 0..<MagiTrack.maxPatterns {
            if i < song.patterns.count {
                writePattern(song.patterns[i], into: &w)
            } else {
                writeEmptyPattern(into: &w)
            }
        }
    }

    private static func writePattern(_ p: Pattern, into w: inout ByteWriter) {
        w.u16(0xFFFF) // noteHead — loader rebuilds from the note stream
        w.u8(p.length)
        w.u8(p.referenceNote)
        for entry in p.inputNotes.prefix(12) {
            w.u8(entry.switchMode.rawValue)
            w.u8(entry.switchTarget)
            w.u8(entry.transposeAction.rawValue)
            w.i8(entry.transposeValue)
        }
        // Pad inputNotes to exactly 12 if the array was short.
        let inputShortBy = 12 - min(p.inputNotes.count, 12)
        w.pad(inputShortBy * 4)
        w.cstr(p.name, width: 16)
        w.u8(p.keyChangeMode.rawValue)
        w.u8(p.blockEndNav.rawByte)
        w.pad(2) // _padOld
    }

    private static func writeEmptyPattern(into w: inout ByteWriter) {
        w.pad(patternSize)
    }

    private static func readPatterns(_ r: inout ByteReader) throws -> [Pattern] {
        var out: [Pattern] = []
        out.reserveCapacity(MagiTrack.maxPatterns)
        for _ in 0..<MagiTrack.maxPatterns {
            out.append(try readPattern(&r))
        }
        return out
    }

    private static func readPattern(_ r: inout ByteReader) throws -> Pattern {
        _ = try r.u16() // noteHead — discarded
        let length        = try r.u8()
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
        return Pattern(length: length,
                       referenceNote: referenceNote,
                       inputNotes: inputs,
                       name: name,
                       keyChangeMode: KeyChangeMode(rawValue: kcm) ?? .samePos,
                       blockEndNav: BlockEndNav(rawByte: nav),
                       notes: [:])
    }

    // ── Columns ──────────────────────────────────────────────────────────
    private static func writeColumns(_ song: Song, into w: inout ByteWriter) {
        for i in 0..<MagiTrack.maxColumns {
            let c = (i < song.columns.count) ? song.columns[i] : ColumnSettings()
            writeColumn(c, into: &w)
        }
    }

    private static func writeColumn(_ c: ColumnSettings, into w: inout ByteWriter) {
        w.u8(c.midiChannel)
        w.u8(c.bankMSB)
        w.u8(c.program)
        w.u8(c.volume)
        w.i8(c.transpose)
        w.u8(c.mute ? 1 : 0)
        w.pad(2) // _pad
        w.cstr(c.name, width: 12)
    }

    private static func readColumns(_ r: inout ByteReader) throws -> [ColumnSettings] {
        var out: [ColumnSettings] = []
        out.reserveCapacity(MagiTrack.maxColumns)
        for _ in 0..<MagiTrack.maxColumns {
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

    // ── Song tail ────────────────────────────────────────────────────────
    //
    // Written as raw bytes from `numPatterns` to end-of-struct. Including
    // 1 byte of padding before `transposeChMask` (u16 alignment) and 1 byte
    // of trailing alignment after `_songPad`. Total: 154 bytes.
    //
    //   u8  numPatterns
    //   u8  startPattern
    //   u16 bpm
    //   u16 minBPM
    //   u16 maxBPM
    //   u8  speed
    //   char name[32]
    //   u8  midiInChannel
    //   u8  midiInNoteMin
    //   u8  midiInNoteMax
    //   u8  performerMask
    //   u8  _gap                 (compiler padding for u16 alignment)
    //   u16 transposeChMask
    //   PerfPadConfig perfPads[8]  (13 bytes each = 104)
    //   u8  _songPad
    //   u8  _trail               (struct-end alignment)
    private static func writeSongTail(_ song: Song, into w: inout ByteWriter) {
        let startOffset = w.data.count
        let numPatterns = UInt8(min(song.patterns.count, MagiTrack.maxPatterns))
        w.u8(numPatterns)
        w.u8(song.startPattern)
        w.u16(song.bpm)
        w.u16(song.minBPM)
        w.u16(song.maxBPM)
        w.u8(song.speed)
        w.cstr(song.name, width: 32)
        w.u8(song.midiInChannel)
        w.u8(song.midiInNoteMin)
        w.u8(song.midiInNoteMax)
        w.u8(song.performerMask)
        w.pad(1) // alignment padding before transposeChMask
        w.u16(song.transposeChMask)
        for i in 0..<MagiTrack.perfPadCount {
            let pad = (i < song.perfPads.count) ? song.perfPads[i] : PerfPadConfig()
            w.cstr(pad.name, width: 12)
            w.u8(pad.mode.rawValue)
        }
        w.pad(1) // _songPad
        w.pad(1) // trailing struct alignment

        let written = w.data.count - startOffset
        assert(written == songTailSize, "song tail size mismatch: \(written) vs \(songTailSize)")
    }

    private static func readSongTail(_ r: inout ByteReader,
                                     patterns: [Pattern],
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

        // Truncate patterns to numPatterns — the trailing entries in the
        // on-disk array are noise (zero-init for memset'd songs).
        let live = patterns.prefix(Int(numPatterns))
        return Song(
            patterns: Array(live),
            columns: columns,
            startPattern: startPattern,
            bpm: bpm,
            minBPM: minBPM,
            maxBPM: maxBPM,
            speed: speed,
            name: name,
            midiInChannel: midiInChannel,
            midiInNoteMin: midiInNoteMin,
            midiInNoteMax: midiInNoteMax,
            performerMask: performerMask,
            transposeChMask: transposeChMask,
            perfPads: perfPads)
    }

    // ── Notes ────────────────────────────────────────────────────────────
    //
    //   u16 noteCount
    //   { u8 pattern, u8 row, u8 col, u8 note, u8 velocity, u8 effect, u8 param } × noteCount
    //
    // Writer order: pattern 0..MAX_PATTERNS-1, within each pattern (row, col)
    // ascending. (The C code walks the circular list, but the LIST was built
    // in sorted order by `NoteGrid` — so the on-disk stream is sorted.)
    private static func writeNotes(_ song: Song, into w: inout ByteWriter) {
        var total: UInt16 = 0
        for pat in song.patterns { total &+= UInt16(pat.notes.count) }
        w.u16(total)
        for (pIdx, pat) in song.patterns.enumerated() {
            let sorted = pat.notes.sorted(by: { $0.key < $1.key })
            for (key, note) in sorted {
                w.u8(UInt8(pIdx))
                w.u8(key.row)
                w.u8(key.col)
                w.u8(note.note)
                w.u8(note.velocity)
                w.u8(note.effect)
                w.u8(note.param)
            }
        }
    }

    private static func readNotes(_ r: inout ByteReader, into song: inout Song) throws {
        let count = try r.u16()
        if count > MagiTrack.maxSongNotes { throw Error.noteCountOverflow(count) }
        for _ in 0..<Int(count) {
            let pIdx = try r.u8()
            let row  = try r.u8()
            let col  = try r.u8()
            let nv   = try r.u8()
            let vel  = try r.u8()
            let eff  = try r.u8()
            let par  = try r.u8()
            guard pIdx < song.patterns.count else { throw Error.noteOnInvalidPattern(pIdx) }
            song.patterns[Int(pIdx)].notes[GridKey(row: row, col: col)] =
                Note(note: nv, velocity: vel, effect: eff, param: par)
        }
    }
}
