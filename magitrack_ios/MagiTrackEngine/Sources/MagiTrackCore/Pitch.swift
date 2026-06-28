import Foundation

/// Semantic view of the `note` byte. Encoding:
///   0      → empty (cell has no note)
///   1..96  → pitch (1 = C-0, 2 = C#0, …, 96 = B-7)
///   0xFE   → "any" (col-0 marker matches any played pitch)
///   0xFF   → note-off (sends MIDI note-off on the column)
public enum Pitch: Equatable, Hashable, Sendable {
    case empty
    case pitch(semitone: UInt8, octave: UInt8) // semitone 0..11, octave 0..7
    case any
    case off

    public init(raw: UInt8) {
        switch raw {
        case 0: self = .empty
        case 0xFE: self = .any
        case 0xFF: self = .off
        default:
            let n = raw - 1
            self = .pitch(semitone: n % 12, octave: n / 12)
        }
    }

    public var raw: UInt8 {
        switch self {
        case .empty: return 0
        case .any:   return 0xFE
        case .off:   return 0xFF
        case .pitch(let s, let o): return (o * 12) + s + 1
        }
    }

    public static let names: [String] = [
        "C-", "C#", "D-", "D#", "E-", "F-",
        "F#", "G-", "G#", "A-", "A#", "B-",
    ]

    /// Three-char display string, terminator-free. "C-4", "ANY", "OFF", "---".
    public var displayString: String {
        switch self {
        case .empty: return "---"
        case .any:   return "ANY"
        case .off:   return "OFF"
        case .pitch(let s, let o): return "\(Self.names[Int(s)])\(o)"
        }
    }
}

public extension UInt8 {
    /// Convenience: build a raw note byte from a semitone (0..11) and octave (0..7).
    static func note(semitone: UInt8, octave: UInt8) -> UInt8 {
        (octave * 12) + semitone + 1
    }
}
