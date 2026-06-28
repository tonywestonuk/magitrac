import XCTest
@testable import MagiTrackCore

final class CoreSmokeTests: XCTestCase {

    func testPitchRoundTrip() {
        // C-0 .. B-7
        for raw: UInt8 in 1...96 {
            let p = Pitch(raw: raw)
            XCTAssertEqual(p.raw, raw, "Pitch round-trip failed for raw=\(raw)")
        }
        // Sentinels
        XCTAssertEqual(Pitch(raw: 0).raw, 0)
        XCTAssertEqual(Pitch(raw: 0xFE).raw, 0xFE)
        XCTAssertEqual(Pitch(raw: 0xFF).raw, 0xFF)

        // Display strings
        XCTAssertEqual(Pitch(raw: 1).displayString, "C-0")
        XCTAssertEqual(Pitch(raw: 13).displayString, "C-1")
        XCTAssertEqual(Pitch(raw: 0).displayString, "---")
        XCTAssertEqual(Pitch(raw: 0xFE).displayString, "ANY")
        XCTAssertEqual(Pitch(raw: 0xFF).displayString, "OFF")
    }

    func testEffectCases() {
        XCTAssertTrue(Effect.wait.isWaitVariant)
        XCTAssertTrue(Effect.wat1.isWaitVariant)
        XCTAssertTrue(Effect.wat2.isWaitVariant)
        XCTAssertFalse(Effect.sync.isWaitVariant)
        XCTAssertFalse(Effect.none.isWaitVariant)

        XCTAssertTrue(Effect.wait.isInputCue)
        XCTAssertTrue(Effect.pasa.isInputCue)
        XCTAssertTrue(Effect.avrg.isInputCue)
        XCTAssertFalse(Effect.tempoSet.isInputCue)
        XCTAssertFalse(Effect.none.isInputCue)
    }

    func testBlockEndNavRoundTrip() {
        for raw: UInt8 in 0...255 {
            let nav = BlockEndNav(rawByte: raw)
            let back = nav.rawByte
            // Normalise: loop ignores its target, so loop+target>0 → loop+0.
            // EXCEPT for the 0x3F sentinel which is returnToCaller.
            if raw == 0x3F {
                XCTAssertEqual(back, 0x3F)
            } else if (raw >> 6) == 0 {
                XCTAssertEqual(back, 0x00, "loop byte should round-trip to 0x00, got 0x\(String(back, radix:16)) from raw 0x\(String(raw, radix:16))")
            } else {
                XCTAssertEqual(back, raw, "non-loop byte should round-trip exactly")
            }
        }
    }

    func testPatternNoteSubscript() {
        var pat = Pattern(length: 16)
        XCTAssertNil(pat[row: 0, col: 1])
        pat[row: 0, col: 1] = Note(note: .note(semitone: 0, octave: 4))
        XCTAssertEqual(pat[row: 0, col: 1]?.note, 49) // C-4 = (4*12)+0+1
        // Setting empty deletes the entry
        pat[row: 0, col: 1] = nil
        XCTAssertNil(pat[row: 0, col: 1])
        XCTAssertEqual(pat.notes.count, 0)
    }

    func testSongDefaults() {
        let song = Song()
        XCTAssertEqual(song.bpm, 120)
        XCTAssertEqual(song.speed, 6)
        XCTAssertEqual(song.minBPM, 60)
        XCTAssertEqual(song.maxBPM, 240)
        XCTAssertEqual(song.columns.count, MagiTrack.maxColumns)
        XCTAssertEqual(song.perfPads.count, MagiTrack.perfPadCount)
        XCTAssertEqual(song.transposeChMask, 0xFDFF)
    }

    func testConstants() {
        XCTAssertEqual(MagiTrack.songFileVersion, 19)
        XCTAssertEqual(MagiTrack.songFileMagic, 0x4D414754)
        XCTAssertEqual(MagiTrack.magiPort, 4242)
    }
}
