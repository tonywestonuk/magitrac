# magitrack_ios

iOS / Swift port of the magitrac client. Drop-in MagiLink TCP client replacing the LilyGo T5 S3 hardware client; talks to the unchanged `magitrac_server_s3` (CoreS3) over WiFi.

## Layout

```
magitrack_ios/
  MagiTrackEngine/          # Pure-Swift package — no UIKit
    Sources/
      MagiTrackCore/        # Song, Pattern, Note, ColumnSettings, enums
      MagiTrackFile/        # .mgt v11→v19 reader/writer + migrations
      MagiTrackWire/        # MagiMsg byte-packed encoders/decoders
      MagiTrackNet/         # MagiLinkClient (NWConnection)
    Tests/
  MagiTrack.xcodeproj       # iOS app target (create in Xcode — see below)
  MagiTrack/                # UIKit app: PainterView, TouchPump, Pages
  Fixtures/                 # real .mgt files for round-trip tests
```

## Building the engine (no Xcode needed)

```
cd MagiTrackEngine
swift build
swift test
```

## Creating the iOS app target

The `.xcodeproj` is not committed (programmatic generation is fragile). Create it in Xcode:

1. File → New → Project → iOS → App
2. Name `MagiTrack`, organization id `com.tonyweston`, interface UIKit, language Swift
3. Save into `magitrack_ios/`
4. Add the local `MagiTrackEngine` package as a dependency: File → Add Package Dependencies → Add Local → select `MagiTrackEngine/`
5. In project settings → General → Supported Destinations: iPhone only
6. Info → Supported Interface Orientations (iPhone): Landscape Left + Landscape Right only

## Why this layout

- The engine package builds in seconds on Mac without a simulator. Round-trip tests for `.mgt` files and wire-format encoders are pure-Swift XCTests.
- The app target is thin: `PainterView`, `TouchPump`, `PageManager`, page implementations.
- Future macOS / iPad versions can re-use the engine package unchanged.

## Compatibility constraints

- `.mgt` file format v19 — byte-faithful round-trip with existing songs (see `MagiTrackFile`).
- MagiLink wire format — byte-faithful with `magitrac_server_s3` (see `MagiTrackWire`).
- Refactor opportunities (Dictionary instead of NoteNode pool, Swift `enum` for effects) live above these serialisation boundaries.
