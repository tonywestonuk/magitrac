import Foundation

/// Tiny little-endian byte stream reader/writer. ESP32 is LE; the .mgt file is
/// raw struct dumps from the C client, so we match LE end-to-end.
public struct ByteWriter {
    public private(set) var data = Data()
    public init() {}

    public mutating func u8(_ v: UInt8)  { data.append(v) }
    public mutating func i8(_ v: Int8)   { data.append(UInt8(bitPattern: v)) }
    public mutating func u16(_ v: UInt16) {
        data.append(UInt8(v & 0xFF))
        data.append(UInt8((v >> 8) & 0xFF))
    }
    public mutating func u32(_ v: UInt32) {
        data.append(UInt8(v & 0xFF))
        data.append(UInt8((v >> 8) & 0xFF))
        data.append(UInt8((v >> 16) & 0xFF))
        data.append(UInt8((v >> 24) & 0xFF))
    }
    /// Append raw bytes verbatim.
    public mutating func bytes(_ b: Data) { data.append(b) }
    /// Append `n` zero bytes (struct padding).
    public mutating func pad(_ n: Int) {
        for _ in 0..<n { data.append(0) }
    }
    /// Append a NUL-terminated string into a fixed-size field. Truncates or
    /// zero-pads to `width`. Always at least one trailing zero if the string
    /// would otherwise fill the field.
    public mutating func cstr(_ s: String, width: Int) {
        let bytes = Array(s.utf8)
        let maxContent = width - 1
        let take = min(bytes.count, maxContent)
        for i in 0..<take { data.append(bytes[i]) }
        for _ in take..<width { data.append(0) }
    }
}

public struct ByteReader {
    private let buf: Data
    public private(set) var cursor: Int

    public init(_ data: Data) {
        self.buf = data
        self.cursor = 0
    }

    public var remaining: Int { buf.count - cursor }

    public enum Error: Swift.Error {
        case underflow(needed: Int, remaining: Int)
        case invalidEncoding
    }

    public mutating func u8() throws -> UInt8 {
        guard remaining >= 1 else { throw Error.underflow(needed: 1, remaining: remaining) }
        let v = buf[buf.startIndex + cursor]
        cursor += 1
        return v
    }
    public mutating func i8() throws -> Int8 { Int8(bitPattern: try u8()) }
    public mutating func u16() throws -> UInt16 {
        guard remaining >= 2 else { throw Error.underflow(needed: 2, remaining: remaining) }
        let lo = UInt16(buf[buf.startIndex + cursor])
        let hi = UInt16(buf[buf.startIndex + cursor + 1])
        cursor += 2
        return (hi << 8) | lo
    }
    public mutating func u32() throws -> UInt32 {
        guard remaining >= 4 else { throw Error.underflow(needed: 4, remaining: remaining) }
        var v: UInt32 = 0
        for i in 0..<4 {
            v |= UInt32(buf[buf.startIndex + cursor + i]) << (8 * i)
        }
        cursor += 4
        return v
    }
    public mutating func skip(_ n: Int) throws {
        guard remaining >= n else { throw Error.underflow(needed: n, remaining: remaining) }
        cursor += n
    }
    /// Read `width` bytes, decode as UTF-8 up to the first NUL byte.
    public mutating func cstr(width: Int) throws -> String {
        guard remaining >= width else { throw Error.underflow(needed: width, remaining: remaining) }
        let slice = buf[(buf.startIndex + cursor)..<(buf.startIndex + cursor + width)]
        cursor += width
        // Find NUL terminator
        var end = slice.startIndex
        while end < slice.endIndex && slice[end] != 0 { slice.formIndex(after: &end) }
        let trimmed = slice[slice.startIndex..<end]
        return String(decoding: trimmed, as: UTF8.self)
    }
    /// Read `n` raw bytes.
    public mutating func bytes(_ n: Int) throws -> Data {
        guard remaining >= n else { throw Error.underflow(needed: n, remaining: remaining) }
        let s = buf.startIndex + cursor
        let chunk = buf[s..<(s+n)]
        cursor += n
        return Data(chunk)
    }
    /// Read all remaining bytes.
    public mutating func remainingBytes() -> Data {
        let n = remaining
        let s = buf.startIndex + cursor
        cursor = buf.count
        return Data(buf[s..<(s+n)])
    }
}
