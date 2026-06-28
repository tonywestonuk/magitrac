import Foundation

/// One screenful of UI. Mirrors the C client's `open() / draw() / poll()`
/// convention but enforced by protocol.
///
/// Pages are `@MainActor`-isolated because the render + touch loops both
/// fire on the main run loop (via CADisplayLink). This lets page code reach
/// MainActor singletons (e.g. AppState) without `await` ceremony.
///
/// Lifecycle:
///   1. `PageManager.push(page)` — calls `open()` and queues a redraw.
///   2. The render loop calls `draw(in:)` whenever the page is dirty.
///   3. The touch loop calls `poll(touch:)` every frame; the return value
///      tells the manager how to respond (stay / pop / open another page).
///
/// Popups (HexpadPopup, KeyboardPopup, etc.) implement Page too — the
/// PageManager stacks them on top, freezing the parent until they pop.
@MainActor
public protocol Page: AnyObject {

    /// One-time setup after the manager pushes this page. The page can
    /// load song data here and pre-compute layout against `screen`.
    func open(screen: Screen)

    /// Render the page into the painter. The painter is pre-cleared by the
    /// PageManager when this page comes into view for the first time, but
    /// subsequent calls do partial redraws based on the page's dirty regions.
    func draw(in painter: Painter)

    /// Poll touch input and decide what happens next.
    func poll(touch: TouchState) -> PageResult
}

/// What the page wants the manager to do next.
public enum PageResult: Equatable {
    /// Nothing changed; keep polling this page.
    case stay
    /// Pop this page off the stack (return to parent).
    case pop
    /// Pop with a result tag — useful when popups return a chosen value.
    case popWithTag(Int)
    /// Push another page on top.
    case push(PageDescriptor)
    /// Replace the entire stack with a new root page.
    case replaceRoot(PageDescriptor)
}

extension PageResult {
    public static func == (lhs: PageResult, rhs: PageResult) -> Bool {
        switch (lhs, rhs) {
        case (.stay, .stay), (.pop, .pop): return true
        case (.popWithTag(let a), .popWithTag(let b)): return a == b
        case (.push, .push), (.replaceRoot, .replaceRoot): return true
        default: return false
        }
    }
}

/// Type-erased descriptor for "construct this kind of page". Used in
/// PageResult so pages can request transitions without holding refs to the
/// concrete next-page type.
public struct PageDescriptor {
    public let make: @MainActor () -> Page
    public init(make: @escaping @MainActor () -> Page) { self.make = make }
}

/// Polled touch state. The TouchPump (UIKit side) updates this every frame
/// from UITouch events. Pages observe the same shape the C client uses for
/// the GT911 — `isTouched / x / y` with rising/falling-edge detection done
/// page-side.
public struct TouchState: Equatable, Sendable {
    public var isTouched: Bool
    public var x: Int
    public var y: Int

    public init(isTouched: Bool = false, x: Int = 0, y: Int = 0) {
        self.isTouched = isTouched
        self.x = x
        self.y = y
    }
}
