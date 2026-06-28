import Foundation
import MagiTrackCore

/// MagiLink wire transport — byte-faithful with `magitrac_lib/src/MagiMsg.h`.
///
/// Every TCP frame is:
///   `[u8 id][u16 length LE][payload …]`
/// where `length` is the **total** message size including the 3-byte header.
/// All multi-byte fields are little-endian. C-side structs are `#pragma pack(1)`
/// so there is no internal padding in the messages this module encodes — we
/// emit fields in declaration order with no alignment gaps.
public enum MagiTrackWire {
    public static let port = MagiTrack.magiPort

    /// 3-byte frame header common to every MagiLink message.
    public static let headerSize = 3
}

/// All TCP-side MagiLink message IDs. (UDP-side legacy `MagiMsgType` lives
/// elsewhere; this enum only covers what crosses the MagiLink TCP socket.)
public enum MagiMsgID: UInt8, Sendable, CaseIterable {
    case connect              = 0x02
    case connectAck           = 0x03
    case disconnect           = 0x04
    case serverAnnounce       = 0x09  // UDP broadcast — server presence beacon

    case play                 = 0x10
    case stop                 = 0x11
    case setSongData          = 0x13
    case pause                = 0x14
    case unpause              = 0x15
    case setWifiChannel       = 0x16

    case songListReq          = 0x20
    case songListResp         = 0x21
    case songLoadReq          = 0x22
    case songDelete           = 0x26
    case songLoadName         = 0x28

    case midiData             = 0x30
    case seek                 = 0x32
    case gotoPos              = 0x33
    case queueBlock           = 0x35
    case cancelQueue          = 0x36
    case previewStart         = 0x37
    case previewStop          = 0x38

    case noteAudition         = 0x3A
    case instrumentsReq       = 0x40
    case instrumentsPatch     = 0x42

    case noteSet              = 0x50

    case noSong               = 0x60

    case sampleListReq        = 0x62
    case sampleListResp       = 0x63

    case startBackup          = 0x80
    case backupHeader         = 0x81
    case backupBody           = 0x82
    case endOfData            = 0x83
    case songPushHeader       = 0x84
    case songPushBody         = 0x85
    case saveSongHeader       = 0x86
    case saveSongBody         = 0x87
    case instrumentsPushHeader = 0x88
    case instrumentsPushBody   = 0x89
    case restoreHeader        = 0x8A
    case restoreBody          = 0x8B
    case saveActive           = 0x8C
    case newSong              = 0x8D
    case fileListReq          = 0x8E
    case fileListResp         = 0x8F
    case fileLoadReq          = 0x90
    case fileLoadHeader       = 0x91
    case fileLoadBody         = 0x92
    case auditionRawNote      = 0x97
    case auditionProgram      = 0x98
}

/// Marker protocol implemented by every typed MagiLink message. Encode emits
/// the full frame (3-byte header + payload). Decode is given only the
/// `payload` slice — the framer has already consumed the header.
public protocol MagiMessage: Sendable {
    static var id: MagiMsgID { get }
    func encode() -> Data
    init(payload: Data) throws
}

/// Errors raised by message encoders/decoders.
public enum MagiWireError: Error, Equatable {
    case truncatedPayload(needed: Int, got: Int)
    case oversizedField(field: String, max: Int, got: Int)
    case invalidFrame(reason: String)
    case wrongMessageID(expected: UInt8, got: UInt8)
    case stringDecodeFailed
}

// ── Framing helpers ────────────────────────────────────────────────────────

extension MagiMessage {
    /// Build a MagiLink frame: `[id][length LE][payload]`.
    public static func frame(payload: Data) -> Data {
        let length = UInt16(MagiTrackWire.headerSize + payload.count)
        var out = Data()
        out.reserveCapacity(Int(length))
        out.append(Self.id.rawValue)
        out.append(UInt8(length & 0xFF))
        out.append(UInt8((length >> 8) & 0xFF))
        out.append(payload)
        return out
    }

    /// Build a header-only frame (no payload). Used for the many empty
    /// fire-and-forget control messages (PLAY/STOP/PAUSE/etc).
    public static func headerOnlyFrame() -> Data {
        frame(payload: Data())
    }
}

/// Incremental TCP frame parser. Feed it raw bytes; it emits complete frames
/// (id + payload) as they become available. Holds an internal accumulation
/// buffer between calls so partial frames survive across socket reads.
public final class MagiFrameParser {
    public struct Frame: Sendable {
        public let id: UInt8
        public let payload: Data
    }

    private var buffer = Data()

    public init() {}

    /// Feed newly-read bytes. Returns any complete frames discovered. May
    /// return an empty array if the buffer is still accumulating.
    public func feed(_ bytes: Data) throws -> [Frame] {
        buffer.append(bytes)
        var out: [Frame] = []
        while true {
            guard buffer.count >= MagiTrackWire.headerSize else { return out }
            let id = buffer[buffer.startIndex]
            let lo = UInt16(buffer[buffer.startIndex + 1])
            let hi = UInt16(buffer[buffer.startIndex + 2])
            let length = Int((hi << 8) | lo)
            // Minimum length is the header itself — anything smaller is corrupt.
            guard length >= MagiTrackWire.headerSize else {
                throw MagiWireError.invalidFrame(reason: "length \(length) < header")
            }
            guard buffer.count >= length else { return out }
            let payloadStart = buffer.startIndex + MagiTrackWire.headerSize
            let payloadEnd   = buffer.startIndex + length
            let payload = Data(buffer[payloadStart..<payloadEnd])
            out.append(Frame(id: id, payload: payload))
            buffer.removeSubrange(buffer.startIndex..<payloadEnd)
        }
    }

    public func reset() { buffer.removeAll(keepingCapacity: true) }
}
