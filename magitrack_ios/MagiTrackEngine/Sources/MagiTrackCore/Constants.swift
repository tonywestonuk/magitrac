import Foundation

public enum MagiTrack {
    public static let maxPatterns      = 50
    public static let maxRows          = 64
    public static let maxColumns       = 21   // col 0 = input, cols 1..20 = output
    public static let maxSongNotes     = 4000 // pool cap for server compatibility
    public static let maxInstruments   = 256
    public static let instrumentNameLen = 12  // 11 chars + null

    public static let inputColumn      = 0
    public static let sfxChannel       = 17   // routes to onboard sample player
    public static let pixelPostChannel = 18   // routes to pixel_post broadcasts

    public static let perfPadCount     = 8
    public static let perfPadNameLen   = 12

    public static let songFileMagic    : UInt32 = 0x4D414754 // "MAGT"
    public static let songFileVersion  : UInt8  = 19

    public static let magiPort         : UInt16 = 4242
    public static let serverIPv4       = "192.168.0.1"
    public static let clientIPv4       = "192.168.0.2"
}
