import XCTest
@testable import MagiTrackWire
@testable import MagiTrackCore

final class WireSmokeTests: XCTestCase {

    func testPort() {
        XCTAssertEqual(MagiTrackWire.port, 4242)
        XCTAssertEqual(MagiTrackWire.headerSize, 3)
    }

    // ── Empty / fire-and-forget messages ───────────────────────────────

    func testEmptyMessagesAreThreeBytes() {
        XCTAssertEqual(MsgConnect().encode().count, 3)
        XCTAssertEqual(MsgPlay().encode().count, 3)
        XCTAssertEqual(MsgStop().encode().count, 3)
        XCTAssertEqual(MsgPause().encode().count, 3)
        XCTAssertEqual(MsgUnpause().encode().count, 3)
        XCTAssertEqual(MsgNoSong().encode().count, 3)
        XCTAssertEqual(MsgNewSong().encode().count, 3)
        XCTAssertEqual(MsgEndOfData().encode().count, 3)
    }

    func testEmptyMessageHeaderBytes() {
        let bytes = MsgPlay().encode()
        XCTAssertEqual(bytes[0], MagiMsgID.play.rawValue) // id
        XCTAssertEqual(bytes[1], 0x03) // length low
        XCTAssertEqual(bytes[2], 0x00) // length high
    }

    // ── Small fixed-payload round-trips ────────────────────────────────

    func testSeekRoundTrip() throws {
        let orig = MsgSeek(pattern: 7, row: 23)
        let bytes = orig.encode()
        XCTAssertEqual(bytes.count, 5) // 3-byte header + 2-byte payload
        XCTAssertEqual(bytes[0], MagiMsgID.seek.rawValue)
        XCTAssertEqual(bytes[1], 5)
        XCTAssertEqual(bytes[2], 0)
        XCTAssertEqual(bytes[3], 7)
        XCTAssertEqual(bytes[4], 23)
        let decoded = try MsgSeek(payload: bytes.subdata(in: 3..<5))
        XCTAssertEqual(decoded.pattern, 7)
        XCTAssertEqual(decoded.row, 23)
    }

    func testQueueBlockRoundTrip() throws {
        let bytes = MsgQueueBlock(pattern: 12).encode()
        let payload = bytes.subdata(in: 3..<bytes.count)
        let decoded = try MsgQueueBlock(payload: payload)
        XCTAssertEqual(decoded.pattern, 12)
    }

    func testMidiDataRoundTrip() throws {
        // Note-on, ch 1, note C4, vel 100
        let midi: Data = Data([0x90, 60, 100])
        let bytes = MsgMidiData(data: midi).encode()
        // Wire: id(1) + length(2) + midiLen(1) + data[3] = 7 bytes total
        XCTAssertEqual(bytes.count, 7)
        let decoded = try MsgMidiData(payload: bytes.subdata(in: 3..<bytes.count))
        XCTAssertEqual(decoded.data, midi)

        // 1-byte MIDI (real-time clock 0xF8) — payload still 1+3 = 4 bytes
        let clock = MsgMidiData(data: Data([0xF8]))
        let clockBytes = clock.encode()
        let clockDecoded = try MsgMidiData(payload: clockBytes.subdata(in: 3..<clockBytes.count))
        XCTAssertEqual(clockDecoded.data, Data([0xF8]))
    }

    func testNoteSetRoundTrip() throws {
        let note = Note(note: 49, velocity: 100, effect: 0xA0, param: 0xA1)
        let m = MsgNoteSet(pattern: 3, row: 8, col: 5, note: note)
        let bytes = m.encode()
        // Wire: 3 header + 3 (pattern,row,col) + 4 (note) = 10 bytes
        XCTAssertEqual(bytes.count, 10)
        let decoded = try MsgNoteSet(payload: bytes.subdata(in: 3..<bytes.count))
        XCTAssertEqual(decoded.pattern, 3)
        XCTAssertEqual(decoded.row, 8)
        XCTAssertEqual(decoded.col, 5)
        XCTAssertEqual(decoded.note, note)
    }

    func testSongLoadNameRoundTrip() throws {
        let m = MsgSongLoadName(name: "BLUE MONDAY")
        let bytes = m.encode()
        // Wire: 3 header + 16 name = 19 bytes
        XCTAssertEqual(bytes.count, 19)
        let decoded = try MsgSongLoadName(payload: bytes.subdata(in: 3..<bytes.count))
        XCTAssertEqual(decoded.name, "BLUE MONDAY")
    }

    // ── Variable-length stream body ────────────────────────────────────

    func testSongPushBodyAtMaxSize() throws {
        let payload = Data((0..<1024).map { UInt8($0 % 256) })
        let m = MsgSongPushBody(data: payload)
        let bytes = m.encode()
        // Wire: 3 header + 2 data_len + 1024 data = 1029 bytes
        XCTAssertEqual(bytes.count, 1029)
        let decoded = try MsgSongPushBody(payload: bytes.subdata(in: 3..<bytes.count))
        XCTAssertEqual(decoded.data, payload)
    }

    func testSongPushBodyPartial() throws {
        let payload = Data([0xDE, 0xAD, 0xBE, 0xEF])
        let m = MsgSongPushBody(data: payload)
        let bytes = m.encode()
        // Even with 4 bytes of meaningful data, the wire frame is the full
        // 1029 bytes (matches the C struct layout exactly).
        XCTAssertEqual(bytes.count, 1029)
        let decoded = try MsgSongPushBody(payload: bytes.subdata(in: 3..<bytes.count))
        XCTAssertEqual(decoded.data.count, 4)
        XCTAssertEqual(decoded.data, payload)
    }

    func testSongPushHeader() throws {
        let m = MsgSongPushHeader(totalSize: 0xDEADBEEF)
        let bytes = m.encode()
        XCTAssertEqual(bytes.count, 7)
        let decoded = try MsgSongPushHeader(payload: bytes.subdata(in: 3..<bytes.count))
        XCTAssertEqual(decoded.totalSize, 0xDEADBEEF)
    }

    // ── Song list response ─────────────────────────────────────────────

    func testSongListRespRoundTrip() throws {
        let m = MsgSongListResp(
            page: 1, totalPages: 3,
            names: ["BLUE MONDAY", "ELECTRICITY", "ATMOSPHERE"])
        let bytes = m.encode()
        // Wire: 3 header + 1 page + 1 totalPages + 1 count + 7×16 names = 118
        XCTAssertEqual(bytes.count, 118)
        let decoded = try MsgSongListResp(payload: bytes.subdata(in: 3..<bytes.count))
        XCTAssertEqual(decoded.page, 1)
        XCTAssertEqual(decoded.totalPages, 3)
        XCTAssertEqual(decoded.names, ["BLUE MONDAY", "ELECTRICITY", "ATMOSPHERE"])
    }

    // ── Frame parser — TCP boundary handling ───────────────────────────

    func testParserSingleCompleteFrame() throws {
        let parser = MagiFrameParser()
        let frames = try parser.feed(MsgPlay().encode())
        XCTAssertEqual(frames.count, 1)
        XCTAssertEqual(frames[0].id, MagiMsgID.play.rawValue)
        XCTAssertEqual(frames[0].payload.count, 0)
    }

    func testParserMultipleFramesOneRead() throws {
        let parser = MagiFrameParser()
        var combined = Data()
        combined.append(MsgPlay().encode())
        combined.append(MsgStop().encode())
        combined.append(MsgSeek(pattern: 4, row: 0).encode())
        let frames = try parser.feed(combined)
        XCTAssertEqual(frames.count, 3)
        XCTAssertEqual(frames[0].id, MagiMsgID.play.rawValue)
        XCTAssertEqual(frames[1].id, MagiMsgID.stop.rawValue)
        XCTAssertEqual(frames[2].id, MagiMsgID.seek.rawValue)
        let seek = try MsgSeek(payload: frames[2].payload)
        XCTAssertEqual(seek.pattern, 4)
        XCTAssertEqual(seek.row, 0)
    }

    func testParserPartialFrameAcrossReads() throws {
        let parser = MagiFrameParser()
        let full = MsgSeek(pattern: 9, row: 17).encode() // 5 bytes
        // Feed byte-by-byte. Should accumulate without emitting until full.
        for i in 0..<full.count {
            let chunk = full.subdata(in: i..<(i+1))
            let frames = try parser.feed(chunk)
            if i < full.count - 1 {
                XCTAssertEqual(frames.count, 0, "should have nothing at byte \(i)")
            } else {
                XCTAssertEqual(frames.count, 1, "should emit after final byte")
                XCTAssertEqual(frames[0].id, MagiMsgID.seek.rawValue)
            }
        }
    }

    func testParserMixedBoundaries() throws {
        let parser = MagiFrameParser()
        // Frame A complete, frame B header only, then B's payload arrives
        let a = MsgPlay().encode()
        let b = MsgQueueBlock(pattern: 42).encode() // 4 bytes
        var first = Data()
        first.append(a)
        first.append(b.subdata(in: 0..<3)) // header only of B
        var frames = try parser.feed(first)
        XCTAssertEqual(frames.count, 1, "first read produces just A")
        XCTAssertEqual(frames[0].id, MagiMsgID.play.rawValue)
        // Now send B's payload
        frames = try parser.feed(b.subdata(in: 3..<b.count))
        XCTAssertEqual(frames.count, 1)
        let qb = try MsgQueueBlock(payload: frames[0].payload)
        XCTAssertEqual(qb.pattern, 42)
    }

    func testParserRejectsImpossibleLength() {
        let parser = MagiFrameParser()
        // length=2 (less than header itself) is corrupt
        let bogus = Data([0x10, 0x02, 0x00])
        XCTAssertThrowsError(try parser.feed(bogus))
    }
}
