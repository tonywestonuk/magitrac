import XCTest
@testable import MagiTrackFile
@testable import MagiTrackCore

final class FileSmokeTests: XCTestCase {

    func testCurrentVersion() {
        XCTAssertEqual(MagiTrackFile.currentVersion, 19)
    }

    /// Verifies the encoded layout has the byte structure the spec promises.
    func testEncodedSizeMatchesLayout() {
        let song = Song(patterns: [MagiTrackCore.Pattern(length: 16)])
        let data = MgtV19Codec.encode(song)
        let expected = MgtV19Codec.headerSize
            + MgtV19Codec.patternSize * MagiTrack.maxPatterns
            + MgtV19Codec.columnSize * MagiTrack.maxColumns
            + MgtV19Codec.songTailSize
            + 2 // noteCount
            + 0 // no notes in this song
        XCTAssertEqual(data.count, expected,
            "encoded size \(data.count) doesn't match expected layout \(expected)")
    }

    func testHeaderMagicAndVersion() {
        let song = Song()
        let data = MgtV19Codec.encode(song)
        // Magic 0x4D414754 ("MAGT") in little-endian = T G A M at offsets 0..3
        XCTAssertEqual(data[0], 0x54) // 'T'
        XCTAssertEqual(data[1], 0x47) // 'G'
        XCTAssertEqual(data[2], 0x41) // 'A'
        XCTAssertEqual(data[3], 0x4D) // 'M'
        XCTAssertEqual(data[4], 19)   // version
        XCTAssertEqual(data[5], 0)    // _pad
        XCTAssertEqual(data[6], 0)
        XCTAssertEqual(data[7], 0)
    }

    func testEmptySongRoundTrip() throws {
        let original = Song()
        let bytes = MgtV19Codec.encode(original)
        let decoded = try MgtV19Codec.decode(bytes)
        XCTAssertEqual(decoded.patterns.count, 0)
        XCTAssertEqual(decoded.bpm, original.bpm)
        XCTAssertEqual(decoded.minBPM, original.minBPM)
        XCTAssertEqual(decoded.maxBPM, original.maxBPM)
        XCTAssertEqual(decoded.speed, original.speed)
        XCTAssertEqual(decoded.transposeChMask, original.transposeChMask)
        XCTAssertEqual(decoded.columns, original.columns)
        XCTAssertEqual(decoded.perfPads, original.perfPads)
    }

    func testRichSongRoundTrip() throws {
        let original = makeRichSong()
        let bytes = MgtV19Codec.encode(original)
        let decoded = try MgtV19Codec.decode(bytes)
        XCTAssertEqual(decoded, original)
        // And re-encoding the decoded song produces identical bytes.
        let reBytes = MgtV19Codec.encode(decoded)
        XCTAssertEqual(reBytes, bytes, "re-encode must produce byte-identical output")
    }

    func testRejectsBadMagic() {
        var data = MgtV19Codec.encode(Song())
        data[0] = 0xDE; data[1] = 0xAD; data[2] = 0xBE; data[3] = 0xEF
        XCTAssertThrowsError(try MgtV19Codec.decode(data))
    }

    func testRejectsBadVersion() {
        var data = MgtV19Codec.encode(Song())
        data[4] = 18 // pretend it's an older format
        XCTAssertThrowsError(try MgtV19Codec.decode(data))
    }

    func testNoteOrderingIsDeterministic() {
        // Same logical Song must produce same bytes regardless of insertion order.
        var p1 = MagiTrackCore.Pattern(length: 16)
        p1[row: 5, col: 3] = Note(note: 50)
        p1[row: 2, col: 7] = Note(note: 40)
        p1[row: 5, col: 1] = Note(note: 30)
        let s1 = Song(patterns: [p1])

        var p2 = MagiTrackCore.Pattern(length: 16)
        p2[row: 5, col: 1] = Note(note: 30)
        p2[row: 5, col: 3] = Note(note: 50)
        p2[row: 2, col: 7] = Note(note: 40)
        let s2 = Song(patterns: [p2])

        XCTAssertEqual(MgtV19Codec.encode(s1), MgtV19Codec.encode(s2))
    }

    // ── Fixture builders ────────────────────────────────────────────────

    private func makeRichSong() -> Song {
        Song(
            patterns: [makeRichPattern()],
            columns: makeRichColumns(),
            startPattern: 0,
            bpm: 161,
            minBPM: 80,
            maxBPM: 200,
            speed: 5,
            name: "ROUNDTRIP TEST",
            midiInChannel: 2,
            midiInNoteMin: 24,
            midiInNoteMax: 96,
            performerMask: 0x0F,
            transposeChMask: 0xFDFF,
            perfPads: makeRichPerfPads())
    }

    private func makeRichPattern() -> MagiTrackCore.Pattern {
        var pat = MagiTrackCore.Pattern(
            length: 32,
            referenceNote: 5,
            inputNotes: makeRichInputs(),
            name: "BLOCK 01",
            keyChangeMode: .top,
            blockEndNav: .forward(by: 3))
        pat[row: 0,  col: 0]  = Note(note: 0,    velocity: 0x80, effect: Effect.wait.rawValue, param: 0)
        pat[row: 4,  col: 1]  = Note(note: 49,   velocity: 100,  effect: 0,    param: 0)
        pat[row: 8,  col: 2]  = Note(note: 56,   velocity: 64,   effect: 0xA0, param: 0xA1)
        pat[row: 15, col: 20] = Note(note: 0xFF, velocity: 0x80, effect: 0xFF, param: 0xFF)
        return pat
    }

    private func makeRichInputs() -> [InputNoteEntry] {
        var result: [InputNoteEntry] = []
        result.reserveCapacity(12)
        for i in 0..<12 {
            let sm: BlockSwitch = BlockSwitch(rawValue: UInt8(i % 3)) ?? .stay
            let ta: TransposeAction = TransposeAction(rawValue: UInt8(i % 3)) ?? .keep
            let tv: Int8 = Int8(i - 6)
            result.append(InputNoteEntry(
                switchMode: sm,
                switchTarget: UInt8(i),
                transposeAction: ta,
                transposeValue: tv))
        }
        return result
    }

    private func makeRichColumns() -> [ColumnSettings] {
        var columns = Array(repeating: ColumnSettings(volume: 100), count: MagiTrack.maxColumns)
        columns[1]  = ColumnSettings(midiChannel: 1,  bankMSB: 0, program: 27, volume: 110, transpose: -5, mute: false, name: "PIANO")
        columns[2]  = ColumnSettings(midiChannel: 10, bankMSB: 0, program: 0,  volume: 90,  transpose: 0,  mute: true,  name: "DRUMS")
        columns[20] = ColumnSettings(midiChannel: 18, bankMSB: 0, program: 0,  volume: 0,   transpose: 0,  mute: false, name: "PIXELPOST")
        return columns
    }

    private func makeRichPerfPads() -> [PerfPadConfig] {
        (0..<MagiTrack.perfPadCount).map { i in
            PerfPadConfig(name: "PAD \(i)", mode: i % 2 == 0 ? .immediate : .queued)
        }
    }
}
