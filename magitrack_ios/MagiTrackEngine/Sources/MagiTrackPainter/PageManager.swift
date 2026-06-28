import Foundation

/// Owns the stack of active pages + the popup overlay model. The render
/// loop drives this; pages don't push/pop themselves directly — they
/// return PageResult from `poll()` and the manager applies it.
@MainActor
public final class PageManager {

    public let painter: Painter
    public let screen: Screen
    private(set) public var stack: [Page] = []

    /// Set to true when a redraw is required this frame. The PainterView's
    /// CADisplayLink consults this and calls `drawIfDirty()`.
    public var dirty: Bool = true

    public init(painter: Painter, screen: Screen, root: Page) {
        self.painter = painter
        self.screen = screen
        root.open(screen: screen)
        stack.append(root)
    }

    public var top: Page { stack.last! }

    /// Push a page onto the stack. The parent is frozen until the new
    /// page pops.
    public func push(_ page: Page) {
        page.open(screen: screen)
        stack.append(page)
        dirty = true
    }

    /// Pop the top page. No-op if there's only the root.
    public func pop() {
        guard stack.count > 1 else { return }
        stack.removeLast()
        dirty = true
    }

    /// Replace the entire stack with a new root. Used for navigation that
    /// shouldn't accumulate history (e.g. boot menu → main, or song reload).
    public func replaceRoot(_ page: Page) {
        page.open(screen: screen)
        stack = [page]
        dirty = true
    }

    /// Pump one tick of touch + draw. Call from the CADisplayLink.
    ///
    /// Currently always redraws — page state can change from network
    /// callbacks at any moment without any signal back to here, so a
    /// strict dirty check would leave the screen stale on "Connecting…"
    /// etc. Per-region dirty marking can come back as an optimisation
    /// once the page set stabilises.
    public func tick(touch: TouchState) {
        let result = top.poll(touch: touch)
        apply(result)
        top.draw(in: painter)
        painter.commit()
        dirty = false
    }

    private func apply(_ result: PageResult) {
        switch result {
        case .stay:
            break
        case .pop:
            pop()
        case .popWithTag:
            pop() // Tag is for the caller to inspect via a callback — not tracked here.
        case .push(let desc):
            push(desc.make())
        case .replaceRoot(let desc):
            replaceRoot(desc.make())
        }
    }
}
