import Foundation
import CoreGraphics

/// Carries the active drawing canvas dimensions + derived layout constants.
/// Replaces the 960×540 `#define`s scattered through the C client (`PE_W`,
/// `PE_NBTN_H`, etc.) so pages reflow when the iPhone aspect differs.
///
/// All values are in CoreGraphics points (logical pixels). Pages declare
/// layouts against Screen-derived values, not raw numbers.
public struct Screen: Sendable, Equatable {
    public let width: CGFloat
    public let height: CGFloat

    public init(width: CGFloat, height: CGFloat) {
        precondition(width > 0 && height > 0, "Screen dimensions must be positive")
        self.width = width
        self.height = height
    }

    public var bounds: CGRect { CGRect(x: 0, y: 0, width: width, height: height) }

    // ── Derived layout constants ────────────────────────────────────────
    // Tuned to match the LilyGo client's proportions (960×540 reference)
    // so pages look familiar regardless of actual screen size.

    /// Header strip at the top — page name, BPM, song name.
    public var headerHeight: CGFloat { round(height * 0.09) }

    /// Status strip at the bottom — current row/pattern, mode indicator.
    public var statusHeight: CGFloat { round(height * 0.06) }

    /// Tracker grid area between header and status.
    public var gridArea: CGRect {
        CGRect(x: 0, y: headerHeight,
               width: width,
               height: height - headerHeight - statusHeight)
    }

    /// Standard button cell width — divides the screen into 12 columns.
    public var standardButtonWidth: CGFloat { width / 12 }

    /// Standard button cell height for piano-roll / note-editor button rows.
    public var standardButtonHeight: CGFloat { round(height * 0.16) }

    /// Touch target minimum (Apple HIG: 44pt). Anything smaller gets
    /// proportional inflation logic on the page side.
    public static let minTouchTarget: CGFloat = 44.0
}
