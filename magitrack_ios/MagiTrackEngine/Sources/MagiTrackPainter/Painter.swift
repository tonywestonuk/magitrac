import Foundation
import CoreGraphics
import CoreText

/// Adafruit-GFX-style drawing surface backed by a CGContext bitmap. The C
/// client's `EPD_Painter_Adafruit` exposes `fillRect`/`drawRect`/`drawLine`/
/// `setCursor`/`print`/`drawBitmap` — same shape here, so page code ports
/// mechanically.
///
/// Threading: not thread-safe. Pages are expected to call draw* methods
/// from the same render thread that consumes the bitmap.
public final class Painter {

    public let screen: Screen

    /// 8-bit-per-channel sRGB bitmap. Resized on `resize(to:)`. The PainterView
    /// (iOS-side) blits this to its UIView.layer.contents.
    public private(set) var bitmap: CGImage?

    private var ctx: CGContext
    private var cursor: CGPoint = .zero
    private var textShade: Shade = .black
    private var textSize: CGFloat = 16
    private var fontName: String = "Menlo-Bold"

    public init(screen: Screen) {
        self.screen = screen
        self.ctx = Self.makeContext(width: Int(screen.width), height: Int(screen.height))
    }

    private static func makeContext(width: Int, height: Int) -> CGContext {
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard let ctx = CGContext(
            data: nil,
            width: width, height: height,
            bitsPerComponent: 8,
            bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else {
            preconditionFailure("Failed to create CGContext at \(width)×\(height)")
        }
        // CoreGraphics default origin is bottom-left; flip to top-left so
        // coordinates match the LilyGo client.
        ctx.translateBy(x: 0, y: CGFloat(height))
        ctx.scaleBy(x: 1, y: -1)
        // Disable interpolation for crisp pixel output (matches the e-paper feel).
        ctx.interpolationQuality = .none
        return ctx
    }

    /// Flush the current bitmap into `self.bitmap` for the view layer to consume.
    public func commit() {
        bitmap = ctx.makeImage()
    }

    // ── Whole-screen ────────────────────────────────────────────────────

    public func clear(_ shade: Shade = .white) {
        ctx.setFillColor(shade.cgColor)
        ctx.fill(screen.bounds)
    }

    // ── Rectangles ──────────────────────────────────────────────────────

    public func fillRect(x: CGFloat, y: CGFloat, w: CGFloat, h: CGFloat, color: Shade) {
        ctx.setFillColor(color.cgColor)
        ctx.fill(CGRect(x: x, y: y, width: w, height: h))
    }

    public func drawRect(x: CGFloat, y: CGFloat, w: CGFloat, h: CGFloat, color: Shade) {
        ctx.setStrokeColor(color.cgColor)
        ctx.setLineWidth(1)
        ctx.stroke(CGRect(x: x + 0.5, y: y + 0.5, width: w - 1, height: h - 1))
    }

    public func drawLine(x0: CGFloat, y0: CGFloat, x1: CGFloat, y1: CGFloat, color: Shade) {
        ctx.setStrokeColor(color.cgColor)
        ctx.setLineWidth(1)
        ctx.beginPath()
        ctx.move(to: CGPoint(x: x0 + 0.5, y: y0 + 0.5))
        ctx.addLine(to: CGPoint(x: x1 + 0.5, y: y1 + 0.5))
        ctx.strokePath()
    }

    // ── Text ────────────────────────────────────────────────────────────

    public func setCursor(x: CGFloat, y: CGFloat) {
        cursor = CGPoint(x: x, y: y)
    }

    public func setTextColor(_ shade: Shade) { textShade = shade }
    public func setTextSize(_ pts: CGFloat) { textSize = pts }
    public func setFont(_ name: String) { fontName = name }

    /// Draw a string at the current cursor and advance the cursor by the
    /// rendered width. Mirrors Adafruit GFX's `print()`.
    ///
    /// Uses CoreText directly to avoid the UIKit/AppKit dependency on
    /// NSAttributedString.Key.font — the engine package must build on
    /// macOS for headless testing too.
    public func print(_ s: String) {
        ctx.saveGState()
        // Re-flip Y inside this call so CoreText doesn't draw mirrored.
        ctx.translateBy(x: 0, y: screen.height)
        ctx.scaleBy(x: 1, y: -1)
        let flippedCursor = CGPoint(x: cursor.x, y: screen.height - cursor.y - textSize)
        let font = CTFontCreateWithName(fontName as CFString, textSize, nil)
        let attrs: [CFString: Any] = [
            kCTFontAttributeName: font,
            kCTForegroundColorAttributeName: textShade.cgColor,
        ]
        guard let attrString = CFAttributedStringCreate(nil, s as CFString, attrs as CFDictionary) else {
            ctx.restoreGState()
            return
        }
        let line = CTLineCreateWithAttributedString(attrString)
        ctx.textPosition = flippedCursor
        CTLineDraw(line, ctx)
        let width = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
        cursor.x += width
        ctx.restoreGState()
    }

    // ── Pixel sampling (for tests) ──────────────────────────────────────

    /// Read the shade at a given point. Used by unit tests to verify draws.
    /// Returns nil if the point is out of bounds.
    public func sample(x: Int, y: Int) -> Shade? {
        guard x >= 0, x < Int(screen.width), y >= 0, y < Int(screen.height) else { return nil }
        guard let img = bitmap ?? ctx.makeImage() else { return nil }
        guard let dataProvider = img.dataProvider, let data = dataProvider.data,
              let ptr = CFDataGetBytePtr(data) else { return nil }
        let bytesPerRow = img.bytesPerRow
        let pixel = ptr.advanced(by: y * bytesPerRow + x * 4)
        let r = CGFloat(pixel[0]) / 255.0
        // Closest matching Shade by luminance.
        return Shade.allCases.min(by: { abs($0.luminance - r) < abs($1.luminance - r) })
    }
}
