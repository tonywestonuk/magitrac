import Foundation

/// Library instrument preset. No MIDI channel — that lives in `ColumnSettings`.
/// Disk size: 18 bytes (11-char name + null + 6-byte tail).
public struct Instrument: Equatable, Sendable {
    public var name: String     // up to 11 chars
    public var bankMSB: UInt8
    public var program: UInt8
    public var volume: UInt8
    public var transpose: Int8

    public init(name: String = "",
                bankMSB: UInt8 = 0,
                program: UInt8 = 0,
                volume: UInt8 = 100,
                transpose: Int8 = 0) {
        self.name = name
        self.bankMSB = bankMSB
        self.program = program
        self.volume = volume
        self.transpose = transpose
    }
}
