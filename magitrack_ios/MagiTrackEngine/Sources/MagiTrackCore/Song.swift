import Foundation

/// Root song container. The Swift representation is structurally cleaner than
/// the C `Song` struct — `numPatterns` becomes `patterns.count`, the
/// `NoteNode` pool is gone, etc. — but every field that lives in the `.mgt`
/// v19 file or on the MagiLink wire is preserved.
public struct Song: Equatable, Sendable {
    public var patterns: [Pattern]           // 0..50
    public var columns: [ColumnSettings]     // exactly maxColumns (21)
    public var startPattern: UInt8
    public var bpm: UInt16
    public var minBPM: UInt16
    public var maxBPM: UInt16
    public var speed: UInt8                  // ticks per row (classic: 6)
    public var name: String                  // up to 31 chars
    public var midiInChannel: UInt8          // 0=ANY, 1..16
    public var midiInNoteMin: UInt8
    public var midiInNoteMax: UInt8
    public var performerMask: UInt8          // bits 0..3: MIDI ch 1..4 driven by performer keys
    public var transposeChMask: UInt16       // bit n = MIDI ch (n+1) follows performer transpose
    public var perfPads: [PerfPadConfig]     // exactly perfPadCount (8)

    public init(patterns: [Pattern] = [],
                columns: [ColumnSettings]? = nil,
                startPattern: UInt8 = 0,
                bpm: UInt16 = 120,
                minBPM: UInt16 = 60,
                maxBPM: UInt16 = 240,
                speed: UInt8 = 6,
                name: String = "",
                midiInChannel: UInt8 = 0,
                midiInNoteMin: UInt8 = 0,
                midiInNoteMax: UInt8 = 127,
                performerMask: UInt8 = 0,
                transposeChMask: UInt16 = 0xFDFF, // all 16 channels except ch 10 (drums)
                perfPads: [PerfPadConfig]? = nil) {
        self.patterns = patterns
        self.columns = columns ?? Array(repeating: ColumnSettings(volume: 100), count: MagiTrack.maxColumns)
        self.startPattern = startPattern
        self.bpm = bpm
        self.minBPM = minBPM
        self.maxBPM = maxBPM
        self.speed = speed
        self.name = name
        self.midiInChannel = midiInChannel
        self.midiInNoteMin = midiInNoteMin
        self.midiInNoteMax = midiInNoteMax
        self.performerMask = performerMask
        self.transposeChMask = transposeChMask
        self.perfPads = perfPads ?? Array(repeating: PerfPadConfig(), count: MagiTrack.perfPadCount)
    }

    /// Total notes across all patterns. Must stay <= `MagiTrack.maxSongNotes`
    /// for server compatibility (the on-wire pool is fixed-size).
    public var totalNoteCount: Int {
        patterns.reduce(0) { $0 + $1.notes.count }
    }

    /// Builds the demo arpeggio song that `initSong()` in TrackerData.cpp creates.
    public static func demo() -> Song {
        Song(
            patterns: [Pattern(length: 16)],
            startPattern: 0
        )
    }
}
