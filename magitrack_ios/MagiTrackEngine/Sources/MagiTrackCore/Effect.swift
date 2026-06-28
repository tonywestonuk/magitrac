import Foundation

/// Note effect byte. Wraps a raw `UInt8` because the full byte space carries
/// (effect, param) pairs that aren't all enumerated — but the named cases for
/// column-0 cue effects and the magic A0xx / FFFF attribute markers get
/// first-class access.
public struct Effect: RawRepresentable, Equatable, Hashable, Sendable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }

    public static let none      = Effect(rawValue: 0x00)
    public static let avrg      = Effect(rawValue: 0x0D) // col0: BPM = mean of last 4 WAIT-derived
    public static let sync      = Effect(rawValue: 0x0E) // col0: snap to row when performer plays match
    public static let wait      = Effect(rawValue: 0x0F) // col0: snap + halt (500 ms timeout)
    public static let wat1      = Effect(rawValue: 0x10) // col0: WAIT with 1 s timeout
    public static let wat2      = Effect(rawValue: 0x11) // col0: WAIT with 2 s timeout
    public static let pasa      = Effect(rawValue: 0x12) // col0: PASS-All — absorb every matching note

    /// `A0xx` row attribute on a non-input output column: sets tempo to `xx` BPM
    /// until the next WAIT. Output-column-only.
    public static let tempoSet  = Effect(rawValue: 0xA0)

    /// `FFFF` row attribute on a non-input output column: stop the song.
    public static let stopSong  = Effect(rawValue: 0xFF)

    public var isWaitVariant: Bool {
        self == .wait || self == .wat1 || self == .wat2
    }

    /// True if this is a column-0 cue effect (WAIT/WAT1/WAT2/SYNC/AVRG/PASA).
    public var isInputCue: Bool {
        switch self {
        case .avrg, .sync, .wait, .wat1, .wat2, .pasa: return true
        default: return false
        }
    }
}
