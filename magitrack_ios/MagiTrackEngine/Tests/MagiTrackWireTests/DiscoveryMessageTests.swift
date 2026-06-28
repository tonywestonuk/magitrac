import XCTest
@testable import MagiTrackWire
@testable import MagiTrackCore

final class DiscoveryMessageTests: XCTestCase {

    func testRoundTrip() throws {
        let msg = MsgServerAnnounce(tcpPort: 4242, name: "magitrac-srv")
        let bytes = msg.encode()
        XCTAssertEqual(bytes.count, MsgServerAnnounce.payloadSize)
        XCTAssertEqual(bytes[0], MagiMsgID.serverAnnounce.rawValue)
        XCTAssertEqual(bytes[1], 0x92) // 4242 LE low
        XCTAssertEqual(bytes[2], 0x10) // 4242 LE high

        let decoded = try XCTUnwrap(MsgServerAnnounce(decoding: bytes))
        XCTAssertEqual(decoded, msg)
    }

    func testRejectsWrongSize() {
        XCTAssertNil(MsgServerAnnounce(decoding: Data([0x09, 0x92, 0x10])))   // too short
        XCTAssertNil(MsgServerAnnounce(decoding: Data(count: 100)))           // too long
    }

    func testRejectsWrongType() {
        var bytes = MsgServerAnnounce(name: "x").encode()
        bytes[0] = 0xFF
        XCTAssertNil(MsgServerAnnounce(decoding: bytes))
    }

    func testHandlesEmptyName() throws {
        let msg = MsgServerAnnounce(tcpPort: 4242, name: "")
        let decoded = try XCTUnwrap(MsgServerAnnounce(decoding: msg.encode()))
        XCTAssertEqual(decoded.name, "")
    }

    func testTruncatesLongName() throws {
        let msg = MsgServerAnnounce(tcpPort: 4242, name: "this-name-is-way-too-long-and-must-be-truncated")
        let bytes = msg.encode()
        XCTAssertEqual(bytes.count, MsgServerAnnounce.payloadSize)
        let decoded = try XCTUnwrap(MsgServerAnnounce(decoding: bytes))
        // cstr truncates to width-1 chars to leave room for a NUL terminator
        XCTAssertEqual(decoded.name.count, MsgServerAnnounce.nameLen - 1)
        XCTAssertTrue(msg.name.hasPrefix(decoded.name))
    }
}
