# MagiTrac

A live-performance MIDI tracker built on ESP32 hardware:

- **`magitrac/`** — handheld client (LilyGo T5 e-paper touch): song editing, block/setlist management, performance control.
- **`magitrac_server_s3/`** — server (M5Stack CoreS3 + Module Audio): sequencer, MIDI out, sample playback, drawbar organ, chord recogniser, touch UI.
- **`magitrac_lib/`** — shared library: song data model, MagiLink TCP transport, pairing.
- **`magitrac_gigbar_dmx/`, `magitrac_dmx/`** — PixelPost/DMX light nodes driven by the server over ESP-NOW broadcast.
- **`magitrac_server/`** — frozen fallback server build (M5Core Basic, ESP32 classic).

Client and server pair over a dedicated softAP network; the client is the editor/remote, the server is the brain that drives synths, samples and lights in time.

## Building

Sketches build from the Arduino IDE with the ESP32 board package. `magitrac_lib` must be available as a library (symlink or copy into your Arduino libraries folder). The CoreS3 server needs USB Mode set to TinyUSB for USB-MSC file transfer.

## License

Copyright (C) 2026 Tony Weston

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

See [LICENSE](LICENSE) for the full text.
