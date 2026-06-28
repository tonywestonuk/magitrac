import Foundation

/// What happens to block position when a matching performer note is played.
public enum BlockSwitch: UInt8, Sendable {
    case stay     = 0  // remain in current block
    case samePos  = 1  // jump to switchTarget, keep current row position
    case top      = 2  // jump to switchTarget, restart from row 0
}

/// What happens to the transposition state when a matching performer note is played.
public enum TransposeAction: UInt8, Sendable {
    case keep   = 0  // unchanged
    case note   = 1  // transpose = performed note's pitch class
    case custom = 2  // apply `transposeValue` semitones
}

/// What happens to the row position when the performer plays a different pitch class.
public enum KeyChangeMode: UInt8, Sendable {
    case samePos = 0  // keep current row
    case top     = 1  // scan to end of block (observing BLK+/BLK-), jump to row 0 of result
}

/// One row of the 12-entry per-pattern input-note matching table.
public struct InputNoteEntry: Equatable, Sendable {
    public var switchMode: BlockSwitch
    public var switchTarget: UInt8
    public var transposeAction: TransposeAction
    public var transposeValue: Int8

    public init(switchMode: BlockSwitch = .stay,
                switchTarget: UInt8 = 0,
                transposeAction: TransposeAction = .keep,
                transposeValue: Int8 = 0) {
        self.switchMode = switchMode
        self.switchTarget = switchTarget
        self.transposeAction = transposeAction
        self.transposeValue = transposeValue
    }
}

/// Block-end navigation: stored as one byte `xxyyyyyy` (mode in top 2 bits,
/// target in low 6). A single sentinel byte 0x3F means "return to caller".
public enum BlockEndNav: Equatable, Hashable, Sendable {
    case loop                       // ignore target, loop this pattern
    case forward(by: UInt8)         // jump forward by N patterns (0..63)
    case back(by: UInt8)            // jump backward by N patterns (0..63)
    case absolute(to: UInt8)        // jump to pattern N (0..63)
    case returnToCaller             // subroutine return sentinel

    public init(rawByte: UInt8) {
        if rawByte == 0x3F {
            self = .returnToCaller
            return
        }
        let target = rawByte & 0x3F
        switch (rawByte >> 6) & 0x3 {
        case 0: self = .loop
        case 1: self = .forward(by: target)
        case 2: self = .back(by: target)
        default: self = .absolute(to: target)
        }
    }

    public var rawByte: UInt8 {
        switch self {
        case .loop:                return 0x00
        case .forward(let n):      return 0x40 | (n & 0x3F)
        case .back(let n):         return 0x80 | (n & 0x3F)
        case .absolute(let n):     return 0xC0 | (n & 0x3F)
        case .returnToCaller:      return 0x3F
        }
    }
}

/// One block in the song. Notes live in a sparse dictionary (replacing the
/// C `NoteNode` linked-list pool — Swift's ARC removes the need).
public struct Pattern: Equatable, Sendable {
    public var length: UInt8                     // active row count: 16, 24, 32, 48, 64
    public var referenceNote: UInt8              // semitone (0..11) that maps to zero transpose
    public var inputNotes: [InputNoteEntry]      // exactly 12 entries
    public var name: String                      // up to 15 chars
    public var keyChangeMode: KeyChangeMode
    public var blockEndNav: BlockEndNav
    public var notes: [GridKey: Note]            // sparse storage

    public init(length: UInt8 = 16,
                referenceNote: UInt8 = 0,
                inputNotes: [InputNoteEntry]? = nil,
                name: String = "",
                keyChangeMode: KeyChangeMode = .samePos,
                blockEndNav: BlockEndNav = .loop,
                notes: [GridKey: Note] = [:]) {
        self.length = length
        self.referenceNote = referenceNote
        self.inputNotes = inputNotes ?? Array(repeating: InputNoteEntry(), count: 12)
        self.name = name
        self.keyChangeMode = keyChangeMode
        self.blockEndNav = blockEndNav
        self.notes = notes
    }

    public subscript(row row: UInt8, col col: UInt8) -> Note? {
        get { notes[GridKey(row: row, col: col)] }
        set {
            let key = GridKey(row: row, col: col)
            if let v = newValue, !v.isEmpty {
                notes[key] = v
            } else {
                notes.removeValue(forKey: key)
            }
        }
    }
}
