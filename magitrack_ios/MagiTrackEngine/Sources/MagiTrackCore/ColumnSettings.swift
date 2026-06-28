import Foundation

/// Per-song per-column MIDI output configuration.
/// Wire/disk size: 20 bytes (must stay 20 for `.mgt` v19 compatibility).
public struct ColumnSettings: Equatable, Sendable {
    public var midiChannel: UInt8  // 1..16; 0 = muted/unset; 17 = SFX, 18 = PixelPost
    public var bankMSB: UInt8      // CC0, 0..127
    public var program: UInt8      // 0..127
    public var volume: UInt8       // CC7, 0..127
    public var transpose: Int8     // semitones, -24..+24
    public var mute: Bool          // disk byte: 0 = unmuted, 1 = muted
    public var name: String        // up to 11 chars

    public init(midiChannel: UInt8 = 0,
                bankMSB: UInt8 = 0,
                program: UInt8 = 0,
                volume: UInt8 = 100,
                transpose: Int8 = 0,
                mute: Bool = false,
                name: String = "") {
        self.midiChannel = midiChannel
        self.bankMSB = bankMSB
        self.program = program
        self.volume = volume
        self.transpose = transpose
        self.mute = mute
        self.name = name
    }

    public var isSfx: Bool       { midiChannel == MagiTrack.sfxChannel }
    public var isPixelPost: Bool { midiChannel == MagiTrack.pixelPostChannel }
    public var isMidiOutput: Bool {
        (1...16).contains(midiChannel)
    }
}
