import Foundation
import MagiTrackCore

// ── UDP messages (server → client) ────────────────────────────────────────
//
// These use the legacy single-byte type prefix (no length field) — UDP
// datagram boundaries carry the framing. Compare to MagiLink TCP messages
// which use [id][length][payload].

/// Server → client: current sequencer position. Sent best-effort over UDP
/// while playing (~50–100 Hz). Receiver overwrites the latest known
/// position; drops are fine (next packet supersedes).
public struct MsgSeqPos: Equatable, Sendable {
    public static let id   = MagiMsgID.seek // 0x32 — wait, no. Defined separately as MSG_SEQ_POS = 0x31
    public static let size = 3

    public var pattern: UInt8
    public var row: UInt8

    public init(pattern: UInt8, row: UInt8) {
        self.pattern = pattern; self.row = row
    }

    public init?(decoding data: Data) {
        guard data.count >= Self.size else { return nil }
        guard data[data.startIndex] == MagiSeqPosType else { return nil }
        var r = ByteReader(data)
        _ = try? r.u8()
        self.pattern = (try? r.u8()) ?? 0
        self.row     = (try? r.u8()) ?? 0
    }
}

/// Server → client: MIDI note-on received from performer while sequencer
/// is stopped. Lets the client UI light up which key was hit even when
/// not playing.
public struct MsgMidiNoteIn: Equatable, Sendable {
    public static let size = 3

    public var midiNote: UInt8
    public var velocity: UInt8

    public init(midiNote: UInt8, velocity: UInt8) {
        self.midiNote = midiNote; self.velocity = velocity
    }

    public init?(decoding data: Data) {
        guard data.count >= Self.size else { return nil }
        guard data[data.startIndex] == MagiMidiNoteInType else { return nil }
        var r = ByteReader(data)
        _ = try? r.u8()
        self.midiNote = (try? r.u8()) ?? 0
        self.velocity = (try? r.u8()) ?? 0
    }
}

/// Server → client: preview playhead row position (column-preview feature).
public struct MsgPreviewRow: Equatable, Sendable {
    public static let size = 2

    public var row: UInt8

    public init(row: UInt8) { self.row = row }

    public init?(decoding data: Data) {
        guard data.count >= Self.size else { return nil }
        guard data[data.startIndex] == MagiPreviewRowType else { return nil }
        var r = ByteReader(data)
        _ = try? r.u8()
        self.row = (try? r.u8()) ?? 0
    }
}

// Type-byte constants for the UDP-only messages (these IDs aren't in the
// MagiMsgID enum because they use the legacy MagiMsgType byte prefix on
// UDP, not the {id,length} TCP framing).
public let MagiSeqPosType:      UInt8 = 0x31
public let MagiMidiNoteInType:  UInt8 = 0x61
public let MagiPreviewRowType:  UInt8 = 0x39
