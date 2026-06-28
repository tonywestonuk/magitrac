import Foundation
import CoreGraphics

/// 4-shade e-paper palette. Mirrors the LilyGo T5 S3's `COL_WHITE` /
/// `COL_LTGREY` / `COL_DKGREY` / `COL_BLACK` constants so the colour
/// discipline ports cleanly: never dkgrey-on-ltgrey, disabled = white bg
/// + dkgrey text, etc.
public enum Shade: UInt8, Sendable, Equatable, CaseIterable {
    case white   = 0
    case ltgrey  = 1
    case dkgrey  = 2
    case black   = 3

    /// 0..1 luminance for CGColor construction.
    public var luminance: CGFloat {
        switch self {
        case .white:  return 1.0
        case .ltgrey: return 0.75
        case .dkgrey: return 0.35
        case .black:  return 0.0
        }
    }

    public var cgColor: CGColor {
        let l = luminance
        return CGColor(srgbRed: l, green: l, blue: l, alpha: 1.0)
    }
}
