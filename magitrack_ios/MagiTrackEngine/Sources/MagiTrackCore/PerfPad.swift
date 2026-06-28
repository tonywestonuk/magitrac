import Foundation

/// Performance-mode pad config (8 per song).
/// Disk size: 13 bytes (12-char name + 1-byte mode).
public struct PerfPadConfig: Equatable, Sendable {
    public enum Mode: UInt8, Sendable {
        case immediate = 0
        case queued    = 1
    }

    public var name: String   // up to 11 chars; empty = use block number as label
    public var mode: Mode

    public init(name: String = "", mode: Mode = .immediate) {
        self.name = name
        self.mode = mode
    }
}
