import Foundation

/// One cell of the tracker. 4 bytes on the wire / in serialised note records.
public struct Note: Equatable, Hashable, Sendable {
    public var note: UInt8     // 0=empty, 1..96=pitch, 0xFE=any, 0xFF=off
    public var velocity: UInt8 // 0..127 explicit, 0x80+ = default (100)
    public var effect: UInt8   // raw effect byte
    public var param: UInt8    // effect parameter

    public init(note: UInt8 = 0, velocity: UInt8 = Self.velocityDefault, effect: UInt8 = 0, param: UInt8 = 0) {
        self.note = note
        self.velocity = velocity
        self.effect = effect
        self.param = param
    }

    public static let velocityDefault: UInt8 = 0x80

    public var pitch: Pitch {
        get { Pitch(raw: note) }
        set { note = newValue.raw }
    }

    public var effectKind: Effect {
        get { Effect(rawValue: effect) }
        set { effect = newValue.rawValue }
    }

    public var isEmpty: Bool {
        note == 0 && effect == 0 && param == 0
    }

    /// True if this is a col-0 PASS marker (a present note with no cue effect or
    /// PASA). The pitch matching rule is taken from the note byte.
    public var isPassMarker: Bool {
        let e = effectKind
        return note != 0 && note != 0xFF && !e.isInputCue
            || e == .pasa
    }
}

/// Sparse grid key for `Pattern.notes`. Replaces the `NoteNode` linked-list pool
/// from the C client: Swift dictionaries get us O(1) lookup with ARC handling
/// allocation. The 4000-note pool cap becomes a save-time validation.
public struct GridKey: Hashable, Sendable, Comparable {
    public var row: UInt8
    public var col: UInt8
    public init(row: UInt8, col: UInt8) { self.row = row; self.col = col }

    public static func < (a: GridKey, b: GridKey) -> Bool {
        if a.row != b.row { return a.row < b.row }
        return a.col < b.col
    }
}
