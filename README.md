# MagiTrac

A live-performance MIDI tracker built on ESP32 hardware. Unlike a normal
sequencer, MagiTrac **follows the performer**: WAIT/SYNC cues in the song
snap playback to what you actually play, and the tempo is derived live from
your timing.

## Documentation

- **[Getting Started](docs/getting-started.md)** — hardware, building the
  firmware, first pairing, first song
- **[Tracker Guide](docs/tracker-guide.md)** — the handheld client, page
  by page
- **[Server Guide](docs/server-guide.md)** — the stage box: screens,
  samples, organ
- **[Reference](docs/reference.md)** — note cues, effects, block
  navigation, exact sequencer semantics
- **[Lights](docs/lights.md)** — the PixelPost/DMX light system
- **[Fumble recall](paper/fumble-recall.md)** — a short paper on how the
  sequencer treats a fumbled cue: it acts on the first key, and for half a
  row the last key counts

## Repository layout

- **`magitrac/`** — handheld client (LilyGo T5 e-paper touch): song editing, block/setlist management, performance control.
- **`magitrac_server_s3/`** — server (M5Stack CoreS3 + Module Audio): sequencer, MIDI out, sample playback, drawbar organ, chord recogniser, touch UI.
- **`magitrac_lib/`** — shared library: song data model, MagiLink TCP transport, pairing.
- **`magitrac_gigbar_dmx/`, `magitrac_dmx/`** — PixelPost/DMX light nodes driven by the server over ESP-NOW broadcast.
- **`magitrac_server/`** — the original server (M5Core Basic, ESP32 classic). Obsolete and no longer builds; kept for reference.
- **`paper/`** — the fumble-recall paper and the replay experiment behind it.

Client and server pair over a dedicated softAP network; the client is the editor/remote, the server is the brain that drives synths, samples and lights in time.

## Building

Sketches build from the Arduino IDE with the ESP32 board package. `magitrac_lib` must be available as a library (symlink or copy into your Arduino libraries folder). See [Getting Started](docs/getting-started.md) for the full library list and board settings.

## Acknowledgements

- **CR78 drum machine** — the server's CR78 organ sound
  (`magitrac_server_s3/cr78.cpp`) is a modified port of
  [CW-78](https://github.com/athousanddetails/schwung-cw-78) by A Thousand
  Details (GPL-3.0), a circuit model of Roland's CR-78.  Our changes: the
  snare's snap and the conga/bongo levels refitted to a recorded CR-78, and
  extra settings (kick click and tuning, snare tuning and ring, hat tone,
  conga decay, group levels).
- **808 drum machine** — the 808 organ sound (`magitrac_server_s3/drum808.cpp`)
  builds each drum the way [io-808](https://github.com/vincentriemer/io-808)
  by Vincent Riemer, a Web Audio TR-808, builds it.

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
