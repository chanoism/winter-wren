# Flock Detection + Reticulum Mesh Node

Firmware for a small handheld/vehicle node that does two things at once on a single ESP32-S3:

1. **Passively detects Flock Safety cameras** by listening for their WiFi frames (OUI match, wildcard-probe signature, and an Information Element fingerprint), and
2. **Relays detections and short preset messages over a [Reticulum](https://reticulum.network) mesh** on 915 MHz LoRa, using the C++ port [microReticulum](https://github.com/attermann/microReticulum).

A 128x64 OLED shows status and the last detection; a rotary encoder switches screens and sends preset messages. No GPS, no phone, no internet required.

> **Status: working prototype.** The LoRa link, Reticulum announces with intact `app_data`, WiFi capture rate, OLED and encoder UI are all verified on hardware. Detection against a **real camera has not yet been confirmed** since the most recent fixes. See [Project status](#project-status) for exactly what is and isn't verified.

---

## Contents

- [Project status](#project-status)
- [Hardware](#hardware)
- [Pinout and wiring](#pinout-and-wiring)
- [Quick start](#quick-start)
- [Per-board identity (important)](#per-board-identity-important)
- [Using the device](#using-the-device)
- [Serial output and the heartbeat](#serial-output-and-the-heartbeat)
- [How detection works](#how-detection-works)
- [Concurrency model](#concurrency-model)
- [Radio configuration](#radio-configuration)
- [Known issues](#known-issues)
- [Troubleshooting](#troubleshooting)
- [Repository layout](#repository-layout)
- [Responsible use](#responsible-use)
- [Credits](#credits)
- [License](#license)

---

## Project status

| Area | State |
|---|---|
| LoRa radio on B2B pins, 915 MHz | Verified. Standalone RadioLib link test, two boards, clean packets at ~750 ft indoors-to-outdoors on the stock test antenna |
| Reticulum over LoRa | Verified. Announces arrive with `app_data` intact on the bench and at range |
| WiFi promiscuous + BLE + LoRa + Reticulum together on the S3 | Verified running simultaneously |
| WiFi capture rate | Verified, ~40 frames/sec after fixing BLE radio contention (was ~0.7/sec) |
| OLED + rotary encoder UI, preset message sending | Verified |
| Flock detection against a real camera | **Not yet confirmed** since the BLE-contention and callback fixes. An earlier version caught a camera only after a reset, which is the bug the detection queue targets. Needs a field test |
| IE fingerprint matcher | Ported from upstream. Validated against synthetic frames only, not a real capture |
| Buzzer | Pin allocated, **not populated or tested** |
| Battery / charging | **Untested** |
| BLE Flock/Raven detection | **Stub.** Dedup only, no heuristics |

---

## Hardware

| Part | Notes |
|---|---|
| **Seeed XIAO ESP32S3** | 8 MB flash, PSRAM, native USB |
| **Seeed Wio-SX1262 kit** (product p-5982) | SX1262 LoRa, connects through the **B2B connector** |
| **SSD1306 128x64 I2C OLED** | Address `0x3C`. Cheap listings sometimes ship SH1106 panels, see [Troubleshooting](#troubleshooting) |
| **Adafruit Seesaw rotary encoder** (4991, or 5880 with the EC11 included) | I2C, default `0x36`. Does quadrature decoding on-board, so it costs no extra GPIOs |
| Active buzzer (optional) | GPIO43 |
| LiPo + slide switch (optional) | The Wio-SX1262 has battery charging built in |
| 915 MHz antenna (U.FL pigtail to SMA) | The stock FPC antenna is for testing only |

### Two different "Wio-SX1262" products

Seeed sells two boards with nearly the same name and **different GPIOs**:

| Product | Connection | NSS | RST | BUSY | DIO1 |
|---|---|---|---|---|---|
| **Wio-SX1262 + XIAO ESP32S3 Kit** (p-5982), *this firmware* | B2B connector | **41** | **42** | **40** | **39** |
| Wio-SX1262 for XIAO, header board (p-6379) | soldered pins | 4 | 3 | 2 | 1 |

SPI bus pins (SCK 7, MISO 8, MOSI 9) are the same on both. Most tutorials don't say which board they mean, which is why the numbers online disagree. Values here come from Seeed's own BSP (`one_channel_hub`, `seeed_xiao_esp32s3_devkit_sx1262.c`) and were confirmed on hardware.

### Antennas

The kit has **two** U.FL connectors. They are not interchangeable:

- **On the Wio-SX1262 board** is the 915 MHz LoRa antenna.
- **On the XIAO** is the 2.4 GHz WiFi/BLE antenna that feeds the Flock detector. Leaving it bare cripples detection.

Always attach a LoRa antenna before powering on. Transmitting into an open port can damage the PA.

---

## Pinout and wiring

| Signal | GPIO | Notes |
|---|---|---|
| LoRa NSS / DIO1 / RST / BUSY | 41 / 39 / 42 / 40 | via B2B |
| LoRa SCK / MISO / MOSI | 7 / 8 / 9 | via B2B |
| I2C SDA | 6 (D5) | OLED + encoder share the bus |
| I2C SCL | 44 (D7) | |
| Buzzer | 43 (D6) | Also U0TXD, so it clicks at boot. Harmless |

**Treat D0-D4 (GPIO 1-5) as unavailable.** Seeed's BSP lists both a B2B and a header number for each LoRa control signal, so those nets are bonded to both.

I2C wiring (all four lines are shared between the OLED and the encoder):

| XIAO | OLED | Seesaw |
|---|---|---|
| 3V3 | VCC | VIN |
| GND | GND | GND |
| D5 / GPIO6 | SDA | SDA |
| D7 / GPIO44 | SCL (may be labelled SCK) | SCL |

Use **3V3, not 5V**. The S3's GPIOs are not 5V tolerant. On I2C modules a pin labelled `SCK` is the same signal as `SCL`.

**The I2C bus is remapped.** The XIAO's default I2C is D4/D5, but D4 goes to the LoRa module, so the firmware calls `Wire.begin(6, 44)`. That call must happen **before** `display.begin()` or `encoder.begin()`. Probing the default pins instead hangs at boot with no output.

For wiring diagrams, battery soldering notes, and enclosure layout see [`BUILD_GUIDE.md`](BUILD_GUIDE.md).

---

## Quick start

### Requirements

[PlatformIO](https://platformio.org) (CLI or the VS Code extension). First build needs internet to fetch libraries; after that it works offline.

### Build and flash

```bash
git clone <this repo>
cd <this repo>
cp include/node_key.h.example include/node_key.h
# edit include/node_key.h and paste in a key (see "Per-board identity")
pio run -t upload
pio device monitor
```

Board `seeed_xiao_esp32s3`, framework `arduino`, platform `espressif32`. The default 8 MB partition table is enough; no custom partitions needed.

### Dependencies

All fetched automatically from `lib_deps`:

| Library | Purpose |
|---|---|
| `attermann/microReticulum` | Reticulum stack |
| `attermann/Crypto` (git) | crypto backend for microReticulum |
| `attermann/microStore` (git) | storage backend for microReticulum |
| `jgromes/RadioLib` ^7.0.0 | SX1262 driver |
| `ArduinoJson` ^7.4.2, `MsgPack` ^0.4.2 | microReticulum dependencies |
| `adafruit/Adafruit SSD1306`, `adafruit/Adafruit GFX Library` | OLED |
| `adafruit/Adafruit seesaw Library` | rotary encoder |

`lib/lora_interface/` is **vendored, not fetched.** It is microReticulum's `examples/common/lora_interface/` with a `BOARD_XIAO_S3_SX1262` branch added. The build fails without it.

### Build flags that matter

| Flag | Why |
|---|---|
| `-DARDUINO_USB_CDC_ON_BOOT=1` `-DARDUINO_USB_MODE=1` | **Required.** Without them a native-USB S3 prints nothing and looks dead |
| `-DBOARD_XIAO_S3_SX1262` | Selects the pin map in `LoRaInterface.cpp` |
| `-DRNS_PERSIST_KNOWN_DESTINATIONS=0` | **Do not remove.** See [Known issues](#known-issues) |
| `-DRNS_LOG_LEVEL=RNS_LOG_LEVEL_INFO` | Raise to `RNS_LOG_LEVEL_TRACE` to debug. The macros compile to no-ops below this level, so a runtime `RNS::loglevel()` call alone does nothing |
| `-DRNS_USE_FS` `-DUSTORE_USE_UNIVERSALFS` | microReticulum throws at `start()` if no filesystem is registered |

---

## Per-board identity (important)

`NODE_PRIVATE_KEY_HEX` in `include/node_key.h` gives each board a fixed Reticulum identity so destination hashes survive reflashing.

**Every board needs its own key.** If two boards share one, they share a destination hash. Packets still reach the radio (you will see RSSI/SNR lines), but each node treats the other's announces as its own and never dispatches them. The symptom is a link that is clearly alive where `app_data` never arrives.

Generate a key:

```bash
python3 -c "import secrets; print(secrets.token_hex(65))"
```

**Do not commit a real key to a public repository.** Anyone holding it can impersonate that node's identity. `include/node_key.h` is gitignored and only `include/node_key.h.example` is committed. The build fails with a clear message if the file is missing or the key is not exactly 130 hex characters.

---

## Using the device

### Screens

| # | Screen | Shows |
|---|---|---|
| 0 | Status | RNS state, band |
| 1 | Stats | Current WiFi channel, detection count |
| 2 | Last detection | Confidence tier, MAC, seconds ago |
| 3 | Last received | Last `app_data` from another node (word-wrapped) |
| 4 | Messages | One preset message at a time, with position counter |

With no encoder attached the screens auto-rotate every 3 seconds and everything else still works.

### Encoder

| Mode | Rotate | Press |
|---|---|---|
| **AUTO** (default) | Takes over, halts rotation | Halts rotation |
| **BROWSE** | Change screen | On screen 4, enter the message list. Elsewhere, resume auto-rotate |
| **SELECT** | Scroll presets | **Send the highlighted message** |

A small filled square in the top-right corner means rotation is halted. After 30 seconds with no input the display returns to auto-rotation on its own.

### Messages and announces

Presets live in `PRESET_MESSAGES[]` in `src/main.cpp`. Keep them to about 20 characters: the screen fits roughly 10 characters per line at size 2 and shows two lines.

Everything goes out as a Reticulum announce with the text in `app_data`:

| Trigger | `app_data` |
|---|---|
| Every 60 s | `online` |
| Flock detection | `FLOCK <mac> <tier label>` |
| Preset message (encoder press) | the message text |
| Line typed over USB serial + Enter | the line you typed (useful for range tests) |

---

## Serial output and the heartbeat

115200 baud. Native USB re-enumerates on every reset, so a serial monitor can drop and the port name can change. On macOS/Linux a reconnect loop around `screen` or `miniterm` saves a lot of frustration.

### Boot

```
[i2c] scanning bus...
[i2c]   0x3C  <- OLED
[i2c]   0x36  (seesaw range 0x36-0x3D)
[i2c] 2 device(s)
[enc] seesaw found at 0x36, product 4991
[rns] setting up Reticulum...
[INF] LoRa initializing...
[INF] LoRa init succeeded.
```

The first lines can be missed if the monitor attaches after boot. Press RESET with the monitor already open.

### Heartbeat (every 10 s)

```
[hb] up=60s frames=2488 (mgmt=1777 data=711) raw=0 hits=0 drops=0 ch=11 heap=180716
```

| Field | Meaning |
|---|---|
| `frames` | WiFi frames seen by the sniffer callback. **Healthy is roughly 40+/sec** |
| `mgmt` / `data` | Breakdown by frame type |
| `raw` | Frames that matched a Flock OUI |
| `hits` | Detections that passed the alert cooldown and were processed |
| `drops` | Queue overflows. Should stay 0 |
| `ch` | Current channel (hops 11 / 6 / 1) |
| `heap` | Free heap. Should be stable |

This line is the main diagnostic:

| Observation | Meaning |
|---|---|
| `frames` stops climbing | The WiFi callback has stopped firing (wedged) |
| `frames` healthy, `raw` stays 0 near a known camera | Its OUI is not in the table |
| `raw` climbs but `hits` stays flat | The alert cooldown is suppressing repeats |
| `frames` is very low (a few per second) | Radio contention or a missing WiFi antenna. See Troubleshooting |

### Detection line

```
[flock] hit AA:BB:CC:DD:EE:FF (IE_SIG) rssi=-61 ch=11 t=84213
```

Greppable, and easy to capture with `... | tee flock_log.txt`.

---

## How detection works

### Confidence tiers

| Tier | Label | Criteria |
|---|---|---|
| 5 | `IE_SIG` | Wildcard probe + Flock OUI + IE fingerprint match |
| 4 | `WILDCARD_PROBE` | Wildcard probe + Flock OUI, IE fingerprint did not match |
| 3 | `PROBE_OUI` | Probe request from a Flock OUI |
| 2 | `OUI_addr2` | Flock OUI on the transmitter address of any frame |
| 1 | `OUI_addr1` | Flock OUI on the receiver address. Noisiest: an AP replying to a camera's probe can land here |

A higher-tier hit on a MAC already seen at a lower tier alerts immediately rather than waiting out the cooldown.

Treat results as **"worth a closer look," not confirmed identification.** This is pattern matching against published research.

### Channel hopping

Descending **11 → 6 → 1** at 250 ms. Cameras sweep channels in ascending order emitting wildcard probes roughly every 0.125 s, so hopping the opposite way cuts across the sweep instead of travelling alongside it. 11/6/1 are the non-overlapping 2.4 GHz channels.

### Frame handling

- Both **management and data frames** are processed. Sleeping cameras never transmit, but nearby APs still address data frames to them, which is where the receiver-address match comes from.
- Frames weaker than **-95 dBm** are dropped, since a corrupted MAC that lands on a Flock OUI is a false positive.
- Flock evaluation runs **before** the general dedup cache. (An earlier version deduplicated first, which silently discarded a camera's probes if its MAC had appeared on any unrelated frame in the previous 30 s.)
- Alerts are rate-limited per MAC with a 5 s cooldown, separate from the 30 s general dedup.

### IE fingerprint

A probe request carries a list of Information Elements. Which tags a device emits, in what order, and the contents of its vendor elements, characterise the firmware that built the frame. The fingerprint is built as a comma-separated string: non-vendor tags become their decimal number, vendor elements become `221:` plus up to 8 payload bytes in hex, and the SSID element is skipped. Flock cameras produce:

```
2,12,127,221:506f9a16030103,45,191,221:0050f208000000
```

`50:6f:9a` is the LiteON OUI, the WiFi stack the cameras run.

The parser includes defensive handling for ESP32 driver length/FCS skew: phantom-overflow detection, TLV resync, a second parse that skips a leading empty SSID pair, and re-anchoring on the LiteON element when leading tags are lost. A wildcard probe is detected by walking the element list directly, which distinguishes an empty SSID from a missing one (an `extractSSID()`-style helper cannot).

### OUI table

33 entries:

- 30 from @NitekryDPaul's promiscuous-mode research
- 1 from Michael / DeFlockJoplin field testing (`82:6b:f2`)
- 2 from [flock-you issue #28](https://github.com/colonelpanichacks/flock-you) (`b4:1e:52`, `e0:0a:f6`), **community-reported and not maintainer-verified.** If false positives appear, suspect these two first

### BLE

BLE scans passively at a 30% duty cycle to leave the shared radio to WiFi. There are **no BLE detection heuristics yet**.

---

## Concurrency model

**The WiFi promiscuous callback must not block.** It runs in the WiFi driver task and is expected to return in microseconds. An earlier version called `Serial.printf()`, a blocking buzzer `delay(150)`, and a blocking LoRa transmit (~600 ms) from inside it. That is roughly a second spent in a driver callback, and it matches the observed symptom of detections working once and then stopping until reset.

Current design:

```
WiFi callback ──► enqueueFlockEvent() ──► ring buffer (16) ──► loop() drains
   (fast, no alloc)                                              (slow work is fine)
```

`loop()` does the serial output, buzzer, LoRa announce and display. The drain is capped at 4 events per iteration so a burst cannot starve LoRa receive polling, and the buzzer is a non-blocking timestamp state machine.

`LoRaInterface::loop()` **polls** the IRQ register rather than using a hardware interrupt, so anything that stalls `loop()` can delay LoRa receive. Keep `loop()` non-blocking.

**Anything added to the callback must stay fast and allocation-free.**

---

## Radio configuration

Set in `lib/lora_interface/LoRaInterface.h` and the `BOARD_XIAO_S3_SX1262` branch of `LoRaInterface.cpp`:

| Parameter | Value |
|---|---|
| Frequency | 915.0 MHz (US, FCC Part 15.247) |
| Bandwidth | 125 kHz |
| Spreading factor | 8 |
| Coding rate | 4/5 |
| TX power | 17 dBm |
| Sync word | private (0x12) |
| Regulator | **LDO** (not DC-DC; the wrong setting causes brownouts at high TX power) |
| TCXO | **3.0 V**, radio-controlled (the header-board variant uses 1.8 V; if init fails with -20 or -706, try 1.8) |
| TX/RX switching | internal, via DIO2 |

The 750 ft figure above was measured with a separate link-test sketch at **SF9 / CR 4/7 / 14 dBm** (RSSI around -123 dBm, SNR around -10.5 dB). This firmware runs SF8 / CR 4/5 / 17 dBm, so the two are not directly comparable. Set `spreading = 9` and `coding = 7` for an apples-to-apples comparison.

Higher spreading factors buy sensitivity at a large airtime cost. A 173-byte announce is roughly 1.2 s on air at SF9 and about 8.7 s at SF12. The radio is deaf while transmitting, so long airtimes also cost receive time and raise collision odds. **Check FCC Part 15.247 dwell-time rules for your configuration**; this project has not been verified for regulatory compliance.

---

## Known issues

### Path-table error (cosmetic)

```
[ERR] Failed to add destination <hash> to path table!
```

Expected. `RNS_PERSIST_KNOWN_DESTINATIONS=0` bypasses the broken known-destinations store, but the path table sits on the same storage engine and still complains. Announces dispatch correctly anyway. With `transport_enabled(false)` the path table is not used. **It would matter if multi-hop relaying were enabled.**

### Why `RNS_PERSIST_KNOWN_DESTINATIONS=0` exists

Root-caused with TRACE logging. `Identity::remember()` fails to write to the known-destinations store; the library then reads back from that same broken store via `recall_app_data()` just before invoking the announce handler, finds nothing, and delivers an announce with no `app_data`. The raw bytes survive transmission intact (confirmed in hex dumps). The flag keeps the lookup in memory. Removing it reintroduces silent `app_data` loss.

### BLE / WiFi radio contention

The S3 has one 2.4 GHz radio shared between WiFi and BLE. The original BLE scan was interval 100 / window 99 (a 99% duty cycle), leaving WiFi roughly 1% of the radio and producing **~0.7 frames/sec**. It is now 30% duty and passive, giving ~40 frames/sec. If `frames` in the heartbeat is low, check this first. Raising `BLE_SCAN_WINDOW` directly costs WiFi capture.

### Not yet implemented

- BLE Flock/Raven detection (upstream flock-you fingerprints Raven acoustic sensors by BLE service UUID)
- GPS (every usable pin is allocated, so a GPS module would have to be I2C)
- Multi-hop relaying (`transport_enabled(false)`)

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Serial monitor shows nothing; board looks dead | Missing `-DARDUINO_USB_CDC_ON_BOOT=1` / `-DARDUINO_USB_MODE=1`, or the monitor attached after boot. Press RESET with the monitor open |
| Monitor drops after a reset or crash | Native USB re-enumerates, often under a new port name. Use a reconnect loop |
| Hangs at boot, no `[enc]` or `[i2c]` lines | An I2C device was initialised before `Wire.begin(6, 44)`, probing D4/D5 (D4 is the LoRa RST line) |
| `LoRa init failed` or `CHIP NOT FOUND` | Wrong pin set (header-board numbers instead of B2B), or the B2B connector is not fully seated. Check for an even gap all round the seam |
| `SPI TIMEOUT` or `-706` at init | Try TCXO 1.8 V instead of 3.0 V |
| Radio clearly receiving (RSSI/SNR lines) but no `app_data` ever arrives | Two boards share one `NODE_PRIVATE_KEY_HEX` |
| Announces arrive but `app_data` is missing | `RNS_PERSIST_KNOWN_DESTINATIONS=0` was removed |
| `frames` only a few per second | BLE scan duty too high, or no antenna on the XIAO's own 2.4 GHz U.FL |
| Detections work once, then stop until reset | Something is blocking in the WiFi callback. Watch whether `frames` stops climbing |
| OLED garbled or horizontally shifted | The panel is an SH1106 sold as SSD1306. Use `Adafruit_SH110X` with the `SH1106G` class at the same address. Text running off the edge is a layout problem, not a driver one |
| No encoder found | The boot scan prints every device. If only `0x3C` shows, check VIN/GND continuity, SDA/SCL not swapped, and solder joints. The firmware sweeps `0x36`-`0x3B` in case the address jumpers moved it |
| Build fails with missing `LoRaInterface.h` | `lib/lora_interface/` is vendored and must be present |
| Build needs a download offline | First build fetches `lib_deps`. Build once while connected |

---

## Repository layout

```
.
├── platformio.ini
├── README.md
├── BUILD_GUIDE.md        wiring, antenna, battery, enclosure notes
├── LICENSE               Apache-2.0
├── NOTICE
├── THIRD_PARTY_NOTICES.md
├── .gitignore
├── include/
│   └── node_key.h.example   copy to node_key.h (gitignored) and set your key
├── src/
│   └── main.cpp          detection, Reticulum setup, display/encoder UI
├── lib/
│   └── lora_interface/
│       ├── LoRaInterface.h     vendored from microReticulum, 915 MHz
│       └── LoRaInterface.cpp   adds the BOARD_XIAO_S3_SX1262 branch
└── tools/                bring-up sketches, each its own PlatformIO project
    ├── lora_pingpong/
    ├── i2c_oled_test/
    └── encoder_test/
```

### Suggested bring-up order

Each step verifies one thing, so a failure points at one cause. The sketches for steps 1 and 2 are in [`tools/`](tools/README.md):

1. A minimal RadioLib send/receive sketch (`tools/lora_pingpong`) on two boards (confirms B2B pins, regulator/TCXO config, antennas)
2. An I2C bus scan with only the OLED (`tools/i2c_oled_test`), then with the encoder added (`tools/encoder_test`)
3. The full firmware on one board, watching the heartbeat for a healthy `frames` count
4. A second board with a different identity key, confirming `app_data` arrives
5. Field-test detection near a known camera

---

## Responsible use

- The detector is **passive**. It receives 802.11 frames and never transmits on WiFi. It reads frame addresses and headers, does not decrypt payloads, and keeps only the most recent hit in memory (plus whatever you choose to capture from the serial output).
- It still sees addresses from every device within range. If you log serial output, you are logging third parties' MAC addresses. Handle and share captures accordingly.
- LoRa transmission at 915 MHz is regulated. Check the rules that apply where you operate before changing power, bandwidth, spreading factor, or duty cycle.
- Detections are probabilistic matches against published research and community reports. They are not authoritative identifications.

---

## Credits

- **Reticulum**: Mark Qvist
- **microReticulum**: attermann
- **Flock detection research**: @NitekryDPaul (OUI list and the receiver-address technique), Michael / DeFlockJoplin (wildcard-probe signature and IE research), and [`colonelpanichacks/flock-you`](https://github.com/colonelpanichacks/flock-you), from which the tiered approach, descending channel hop, and IE fingerprint are ported
- **IE fingerprinting**: Lucia Pintor and Luigi Atzori, DOI 10.1109/GLOBECOM48099.2022.10001618
- **RadioLib**: Jan Gromeš

Full license details for everything above are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Third-party code keeps its own license: `lib/lora_interface/` is Apache-2.0 (modified from microReticulum), and the ported detection logic and OUI list are MIT (flock-you, nite-oui-collection). Details and the required notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
