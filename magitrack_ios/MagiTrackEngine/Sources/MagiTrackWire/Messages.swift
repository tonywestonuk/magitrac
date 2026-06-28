import Foundation
import MagiTrackCore

// ── Empty control messages ────────────────────────────────────────────────
//
// Fire-and-forget — the message ID alone is the instruction. Encoded as
// a header-only frame (3 bytes total).

private protocol EmptyMagiMessage: MagiMessage {
    init()
}
extension EmptyMagiMessage {
    public func encode() -> Data { Self.headerOnlyFrame() }
    public init(payload: Data) throws { self.init() }
}

public struct MsgConnect:        MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.connect;      public init() {} }
public struct MsgConnectAck:     MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.connectAck;   public init() {} }
public struct MsgDisconnect:     MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.disconnect;   public init() {} }
public struct MsgPlay:           MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.play;         public init() {} }
public struct MsgStop:           MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.stop;         public init() {} }
public struct MsgPause:          MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.pause;        public init() {} }
public struct MsgUnpause:        MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.unpause;      public init() {} }
public struct MsgCancelQueue:    MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.cancelQueue;  public init() {} }
public struct MsgPreviewStop:    MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.previewStop;  public init() {} }
public struct MsgNoSong:         MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.noSong;       public init() {} }
public struct MsgEndOfData:      MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.endOfData;    public init() {} }
public struct MsgNewSong:        MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.newSong;      public init() {} }
public struct MsgInstrumentsReq: MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.instrumentsReq; public init() {} }
public struct MsgStartBackup:    MagiMessage, EmptyMagiMessage { public static let id = MagiMsgID.startBackup;  public init() {} }

// ── Playback / position ──────────────────────────────────────────────────

public struct MsgSeek: MagiMessage {
    public static let id = MagiMsgID.seek
    public var pattern: UInt8
    public var row: UInt8
    public init(pattern: UInt8, row: UInt8) { self.pattern = pattern; self.row = row }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(pattern); w.u8(row); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.pattern = try r.u8()
        self.row     = try r.u8()
    }
}

public struct MsgGoto: MagiMessage {
    public static let id = MagiMsgID.gotoPos
    public var pattern: UInt8
    public var row: UInt8
    public init(pattern: UInt8, row: UInt8) { self.pattern = pattern; self.row = row }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(pattern); w.u8(row); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.pattern = try r.u8()
        self.row     = try r.u8()
    }
}

public struct MsgQueueBlock: MagiMessage {
    public static let id = MagiMsgID.queueBlock
    public var pattern: UInt8
    public init(pattern: UInt8) { self.pattern = pattern }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(pattern); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.pattern = try r.u8()
    }
}

public struct MsgPreviewStart: MagiMessage {
    public static let id = MagiMsgID.previewStart
    public var pattern: UInt8
    public var col: UInt8
    public init(pattern: UInt8, col: UInt8) { self.pattern = pattern; self.col = col }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(pattern); w.u8(col); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.pattern = try r.u8()
        self.col     = try r.u8()
    }
}

public struct MsgSetWifiChannel: MagiMessage {
    public static let id = MagiMsgID.setWifiChannel
    public var idx: UInt8 // 0/1/2 → channels 1/6/11
    public init(idx: UInt8) { self.idx = idx }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(idx); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.idx = try r.u8()
    }
}

// ── Song list / load ──────────────────────────────────────────────────────

public struct MsgSongListReq: MagiMessage {
    public static let id = MagiMsgID.songListReq
    public var page: UInt8
    public init(page: UInt8) { self.page = page }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(page); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.page = try r.u8()
    }
}

public struct MsgSongListResp: MagiMessage {
    public static let id = MagiMsgID.songListResp
    public static let nameLen   = 16
    public static let perPacket = 7
    public var page: UInt8
    public var totalPages: UInt8
    public var names: [String] // up to perPacket entries
    public init(page: UInt8, totalPages: UInt8, names: [String]) {
        self.page = page; self.totalPages = totalPages; self.names = names
    }
    public func encode() -> Data {
        var w = ByteWriter()
        w.u8(page)
        w.u8(totalPages)
        let count = UInt8(min(names.count, Self.perPacket))
        w.u8(count)
        for i in 0..<Self.perPacket {
            let s = i < Int(count) ? names[i] : ""
            w.cstr(s, width: Self.nameLen)
        }
        return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.page       = try r.u8()
        self.totalPages = try r.u8()
        let count       = try r.u8()
        var out: [String] = []
        out.reserveCapacity(Int(count))
        for i in 0..<Self.perPacket {
            let s = try r.cstr(width: Self.nameLen)
            if i < Int(count) { out.append(s) }
        }
        self.names = out
    }
}

public struct MsgSongLoadReq: MagiMessage {
    public static let id = MagiMsgID.songLoadReq
    public var page: UInt8
    public var index: UInt8
    public init(page: UInt8, index: UInt8) { self.page = page; self.index = index }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(page); w.u8(index); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.page  = try r.u8()
        self.index = try r.u8()
    }
}

public struct MsgSongLoadName: MagiMessage {
    public static let id = MagiMsgID.songLoadName
    public static let nameLen = 16
    public var name: String
    public init(name: String) { self.name = name }
    public func encode() -> Data {
        var w = ByteWriter(); w.cstr(name, width: Self.nameLen); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.name = try r.cstr(width: Self.nameLen)
    }
}

public struct MsgSongDelete: MagiMessage {
    public static let id = MagiMsgID.songDelete
    public static let nameLen = 24
    public var name: String
    public init(name: String) { self.name = name }
    public func encode() -> Data {
        var w = ByteWriter(); w.cstr(name, width: Self.nameLen); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.name = try r.cstr(width: Self.nameLen)
    }
}

public struct MsgSaveActive: MagiMessage {
    public static let id = MagiMsgID.saveActive
    public static let nameLen = 24
    public var name: String
    public init(name: String) { self.name = name }
    public func encode() -> Data {
        var w = ByteWriter(); w.cstr(name, width: Self.nameLen); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.name = try r.cstr(width: Self.nameLen)
    }
}

// ── Song push (server → client) ───────────────────────────────────────────
//
// Server announces total bytes with a HEADER, then sends N BODY chunks
// each carrying up to 1024 bytes of the raw Song stream.

public struct MsgSongPushHeader: MagiMessage {
    public static let id = MagiMsgID.songPushHeader
    public var totalSize: UInt32
    public init(totalSize: UInt32) { self.totalSize = totalSize }
    public func encode() -> Data {
        var w = ByteWriter(); w.u32(totalSize); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.totalSize = try r.u32()
    }
}

/// Stream body — the wire format reserves a fixed 1024-byte payload area
/// with `dataLen` indicating how many bytes are meaningful. Bodies are
/// always exactly 1029 bytes on the wire (1 + 2 + 2 + 1024).
public struct MsgSongPushBody: MagiMessage {
    public static let id = MagiMsgID.songPushBody
    public static let maxDataLen = 1024
    public var data: Data // 0..1024 bytes
    public init(data: Data) {
        precondition(data.count <= Self.maxDataLen, "song push body too large")
        self.data = data
    }
    public func encode() -> Data {
        var w = ByteWriter()
        w.u16(UInt16(data.count))
        w.bytes(data)
        w.pad(Self.maxDataLen - data.count)
        return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        let n = Int(try r.u16())
        guard n <= Self.maxDataLen else {
            throw MagiWireError.oversizedField(field: "data_len", max: Self.maxDataLen, got: n)
        }
        // Read the meaningful prefix; skip the rest of the fixed area.
        self.data = try r.bytes(n)
        try r.skip(Self.maxDataLen - n)
    }
}

// ── MIDI / note ───────────────────────────────────────────────────────────

public struct MsgMidiData: MagiMessage {
    public static let id = MagiMsgID.midiData
    public var data: Data // 1..3 bytes
    public init(data: Data) {
        precondition((1...3).contains(data.count), "MIDI message must be 1..3 bytes")
        self.data = data
    }
    public func encode() -> Data {
        var w = ByteWriter()
        w.u8(UInt8(data.count))
        w.bytes(data)
        w.pad(3 - data.count) // fixed-width tail (struct is 3 bytes)
        return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        let n = Int(try r.u8())
        guard (1...3).contains(n) else {
            throw MagiWireError.invalidFrame(reason: "midiLen \(n) out of range")
        }
        self.data = try r.bytes(n)
        try r.skip(3 - n)
    }
}

public struct MsgNoteSet: MagiMessage {
    public static let id = MagiMsgID.noteSet
    public var pattern: UInt8
    public var row: UInt8
    public var col: UInt8
    public var note: Note
    public init(pattern: UInt8, row: UInt8, col: UInt8, note: Note) {
        self.pattern = pattern; self.row = row; self.col = col; self.note = note
    }
    public func encode() -> Data {
        var w = ByteWriter()
        w.u8(pattern); w.u8(row); w.u8(col)
        w.u8(note.note); w.u8(note.velocity); w.u8(note.effect); w.u8(note.param)
        return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.pattern = try r.u8()
        self.row     = try r.u8()
        self.col     = try r.u8()
        let nn = try r.u8(); let vv = try r.u8(); let ee = try r.u8(); let pp = try r.u8()
        self.note = Note(note: nn, velocity: vv, effect: ee, param: pp)
    }
}

public struct MsgNoteAudition: MagiMessage {
    public static let id = MagiMsgID.noteAudition
    public var pattern: UInt8
    public var row: UInt8
    public var col: UInt8
    public init(pattern: UInt8, row: UInt8, col: UInt8) {
        self.pattern = pattern; self.row = row; self.col = col
    }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(pattern); w.u8(row); w.u8(col); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.pattern = try r.u8(); self.row = try r.u8(); self.col = try r.u8()
    }
}

public struct MsgAuditionRawNote: MagiMessage {
    public static let id = MagiMsgID.auditionRawNote
    public var channel: UInt8 // 1..16
    public var note: UInt8
    public var velocity: UInt8
    public init(channel: UInt8, note: UInt8, velocity: UInt8) {
        self.channel = channel; self.note = note; self.velocity = velocity
    }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(channel); w.u8(note); w.u8(velocity); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.channel = try r.u8(); self.note = try r.u8(); self.velocity = try r.u8()
    }
}

public struct MsgAuditionProgram: MagiMessage {
    public static let id = MagiMsgID.auditionProgram
    public var channel: UInt8 // 1..16
    public var program: UInt8
    public init(channel: UInt8, program: UInt8) {
        self.channel = channel; self.program = program
    }
    public func encode() -> Data {
        var w = ByteWriter(); w.u8(channel); w.u8(program); return Self.frame(payload: w.data)
    }
    public init(payload: Data) throws {
        var r = ByteReader(payload)
        self.channel = try r.u8(); self.program = try r.u8()
    }
}
