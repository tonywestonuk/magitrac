import Foundation
import MagiTrackCore

/// UDP server-presence beacon.
///
/// Broadcast by the magitrac server every ~2 s to UDP port `MAGI_PORT`.
/// Listeners (iOS clients, etc.) discover the server's IP from the source
/// address of the datagram itself — the payload only carries the TCP port
/// and a friendly name, so we don't have to worry about IP encoding /
/// IPv4 vs IPv6.
///
/// Wire format (UDP, no length header — datagram size IS the length):
///   `[u8 type=0x09][u16 tcpPort LE][char name[16] (null-terminated)]`
///   Total: 19 bytes.
///
/// This is a UDP message, not TCP, so it does NOT use the MagiLink
/// framing `MagiMessage` protocol (which prepends `[id][length]`).
public struct MsgServerAnnounce: Equatable, Sendable {
    public static let id          = MagiMsgID.serverAnnounce
    public static let nameLen     = 16
    public static let payloadSize = 1 + 2 + nameLen   // = 19

    public var tcpPort: UInt16
    public var name: String

    public init(tcpPort: UInt16 = MagiTrack.magiPort, name: String) {
        self.tcpPort = tcpPort
        self.name = name
    }

    /// Encode the full UDP datagram (id byte + payload).
    public func encode() -> Data {
        var w = ByteWriter()
        w.u8(Self.id.rawValue)
        w.u16(tcpPort)
        w.cstr(name, width: Self.nameLen)
        return w.data
    }

    /// Decode a UDP datagram. Returns nil if the type byte doesn't match
    /// or the datagram is the wrong size — we don't throw because the
    /// listener may see all sorts of unrelated UDP traffic on the same
    /// port and "ignore silently" is the right call there.
    public init?(decoding data: Data) {
        guard data.count == Self.payloadSize else { return nil }
        var r = ByteReader(data)
        guard let typeByte = try? r.u8(),
              typeByte == Self.id.rawValue else { return nil }
        guard let port = try? r.u16(),
              let name = try? r.cstr(width: Self.nameLen) else { return nil }
        self.tcpPort = port
        self.name = name
    }
}
