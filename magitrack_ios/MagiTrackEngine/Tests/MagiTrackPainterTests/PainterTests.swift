import XCTest
import CoreGraphics
@testable import MagiTrackPainter

final class PainterTests: XCTestCase {

    func testScreenDerivedLayouts() {
        let s = Screen(width: 960, height: 540)
        XCTAssertEqual(s.headerHeight, 49) // 540 * 0.09 = 48.6 → 49
        XCTAssertEqual(s.statusHeight, 32) // 540 * 0.06 = 32.4 → 32
        XCTAssertEqual(s.gridArea.height, 540 - 49 - 32)
        XCTAssertEqual(s.standardButtonWidth, 80)
    }

    func testClearFillsWhole() {
        let p = Painter(screen: Screen(width: 100, height: 100))
        p.clear(.black)
        p.commit()
        XCTAssertEqual(p.sample(x: 0, y: 0),  .black)
        XCTAssertEqual(p.sample(x: 50, y: 50), .black)
        XCTAssertEqual(p.sample(x: 99, y: 99), .black)
    }

    func testFillRectPaintsExpectedRegion() {
        let p = Painter(screen: Screen(width: 100, height: 100))
        p.clear(.white)
        p.fillRect(x: 10, y: 10, w: 30, h: 30, color: .dkgrey)
        p.commit()
        // Inside the rect
        XCTAssertEqual(p.sample(x: 20, y: 20), .dkgrey)
        XCTAssertEqual(p.sample(x: 39, y: 39), .dkgrey)
        // Outside the rect
        XCTAssertEqual(p.sample(x: 5, y: 5),   .white)
        XCTAssertEqual(p.sample(x: 50, y: 50), .white)
    }

    func testShadePaletteRoundsConsistently() {
        // Each shade's luminance maps back to itself via sample's nearest-match.
        let p = Painter(screen: Screen(width: 4, height: 4))
        for (i, shade) in Shade.allCases.enumerated() {
            p.fillRect(x: CGFloat(i), y: 0, w: 1, h: 4, color: shade)
        }
        p.commit()
        for (i, shade) in Shade.allCases.enumerated() {
            XCTAssertEqual(p.sample(x: i, y: 0), shade,
                "shade column \(i) (\(shade)) didn't round-trip")
        }
    }

    @MainActor
    func testPageManagerBasicPushPop() {
        // Minimal Page implementations to exercise the manager
        final class StubPage: Page {
            var openedTimes = 0
            var drawnTimes = 0
            var result: PageResult = .stay
            func open(screen: Screen) { openedTimes += 1 }
            func draw(in painter: Painter) { drawnTimes += 1 }
            func poll(touch: TouchState) -> PageResult { result }
        }

        let painter = Painter(screen: Screen(width: 100, height: 100))
        let root = StubPage()
        let mgr = PageManager(painter: painter, screen: painter.screen, root: root)
        XCTAssertEqual(root.openedTimes, 1)
        XCTAssertEqual(mgr.stack.count, 1)

        // tick draws root (always-draw mode)
        mgr.tick(touch: TouchState())
        XCTAssertEqual(root.drawnTimes, 1)

        // Push a popup
        let popup = StubPage()
        root.result = .push(PageDescriptor { popup })
        mgr.tick(touch: TouchState())
        XCTAssertEqual(mgr.stack.count, 2)
        XCTAssertEqual(popup.openedTimes, 1)
        // popup got the post-push tick's draw
        XCTAssertEqual(popup.drawnTimes, 1)
        // root did NOT draw this tick — popup is on top
        XCTAssertEqual(root.drawnTimes, 1)

        // Pop back
        popup.result = .pop
        mgr.tick(touch: TouchState())
        XCTAssertEqual(mgr.stack.count, 1)
        // root re-draws after popup pops
        XCTAssertEqual(root.drawnTimes, 2)
    }
}
