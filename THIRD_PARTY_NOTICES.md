# Third-party notices

This project is licensed under Apache-2.0 (see `LICENSE`). It contains code derived from, or links against, the third-party projects below. Licenses were read from each project's own LICENSE file or source headers.

Not legal advice. If you ship this commercially, have a lawyer review it.

## Code included in this repository

| Component | Where it is used | License | Copyright |
|---|---|---|---|
| [attermann/microReticulum](https://github.com/attermann/microReticulum) `examples/common/lora_interface/` | `lib/lora_interface/` (vendored, **modified**) | Apache-2.0 | attermann |
| [colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you) | tiered detection, descending channel hop, IE fingerprint logic ported into `src/main.cpp` | MIT | (c) 2026 colonelpanichacks |
| [nitekry/nite-oui-collection](https://github.com/nitekry/nite-oui-collection) | OUI prefixes in `src/main.cpp` | MIT | (c) 2026 nitekry |

### Modifications to microReticulum's LoRaInterface

Apache-2.0 section 4(b) requires modified files to say so. Both files carry that notice in their headers. Changes: added the `BOARD_XIAO_S3_SX1262` pin/radio branch for the Seeed XIAO ESP32S3 + Wio-SX1262 kit, and set the frequency to 915 MHz. The Apache-2.0 text is in `LICENSE`.

### MIT license: flock-you

```
MIT License

Copyright (c) 2026 colonelpanichacks

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### MIT license: nite-oui-collection

```
MIT License

Copyright (c) 2026 nitekry

(Same permission notice and disclaimer as above.)
```

## Libraries fetched at build time (not redistributed in this repository)

| Library | License |
|---|---|
| attermann/microReticulum | Apache-2.0 |
| attermann/microStore | Apache-2.0 |
| attermann/Crypto | MIT (c) 2024 Chad Attermann |
| jgromes/RadioLib | MIT (c) 2018 Jan Gromes |
| bblanchon/ArduinoJson | MIT (c) 2014-2026 Benoit Blanchon |
| hideakitai/MsgPack | MIT (c) 2020 Hideaki Tai |
| adafruit/Adafruit SSD1306, Adafruit GFX, Adafruit seesaw | BSD (per source headers; the notice must be kept in redistributions) |
| espressif arduino-esp32 (framework) | LGPL-2.1 |

If you distribute **compiled firmware binaries** (not just source), you take on those libraries' redistribution terms: include the MIT/BSD/Apache notices, and for the LGPL-2.1 arduino-esp32 core, provide a way for recipients to relink (for example by publishing this source and build files). Publishing source only does not trigger this.

## Upstream note: Reticulum

microReticulum is a C++ port of [Reticulum](https://github.com/markqvist/Reticulum) (Mark Qvist). The Python reference implementation uses an MIT license with two added restrictions: no use in systems designed to do purposeful harm to human beings, and no use in building AI/ML training datasets. Read its LICENSE file for the exact wording. microReticulum itself declares Apache-2.0. Whether the port's license fully replaces the reference license is a question for its author, not this project. This project uses microReticulum only through its published Apache-2.0 license.
