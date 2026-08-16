# Community Mesh Alert Network

> ## UNDER ACTIVE DEVELOPMENT. NOT READY FOR FIELD USE.
>
> Nothing here is finished, audited, or stable. Expect breaking changes. Two known blockers before any real deployment: the legacy firmware track transmits **unencrypted**, and the Reticulum track has an **unresolved data-loss bug at range** (see Open Bugs). Do not rely on this for anything where interception or a dropped alert would cause harm.

An off-grid LoRa mesh alert network built from inexpensive, widely available parts. Field devices act like pagers: someone sends a short alert, and it hops device to device until everyone in range gets a beep and an on-screen notification. No cell service, no internet, no accounts, no third-party platform.

The repo covers three subsystems sharing one hardware platform:

| Subsystem | Purpose |
|---|---|
| **Mesh nodes** | Handheld pager-style alert devices. ESP32 plus SX1276. Two firmware tracks, described below. |
| **Base stations** | Fixed, residence-hosted relay and logging sites. Higher-gain antenna, mains power, Raspberry Pi backend. |
| **Passive scanner** | ESP32 WiFi/BLE reconnaissance for surveillance-infrastructure mapping. Standalone, and also integrated into the Reticulum node firmware. |

Base stations were originally scoped as a separate project and are not yet integrated with either node firmware track.

---

## Two firmware tracks

The project is mid-migration. Both trees are live and neither is retired yet.

| | **Legacy, custom RadioLib** | **Current, microReticulum** |
|---|---|---|
| Build | Arduino IDE | PlatformIO |
| Routing | Hand-rolled flood relay, ID dedup, hop count | Reticulum stack (RNS) |
| Encryption | None, plaintext | Yes, end to end by default |
| Authentication | None, forged alerts trivial | Yes, public-key identities |
| Store and forward | No, fire and forget | Yes, via LXMF |
| Flock scanner | Not present | Yes, OUI table and heuristics live here |
| Field-tested | 2 nodes, bench | 2 nodes, bench plus 300ft range test |
| State | Working, superseded | Working, with open bugs |

The legacy track is kept as a known-good reference for the radio wiring and as a fallback if the Reticulum track stalls. New work goes into the Reticulum track.

### Why the migration

The custom firmware was the right way to start: minimal, no telemetry, no vendor dependencies, and building it from zero meant genuinely understanding the stack rather than trusting a black box. But it has no encryption and no message authentication. Anyone in range with a compatible receiver can read the traffic, and anyone can inject forged alerts. For a system whose purpose is time-sensitive alerting, both are disqualifying.

We also deliberately avoid **Meshtastic**. It is the obvious off-the-shelf option, but its default behavior broadcasts persistent node identity and telemetry on-air, creating exactly the kind of durable, correlatable radio fingerprint we want to avoid.

[Reticulum](https://reticulum.network/) (RNS) is a full cryptographic networking stack, comparable in scope to what TCP/IP does for the internet but designed for exactly this deployment. Encryption is on by default with no unencrypted mode to accidentally ship. Destinations are cryptographic keys rather than static IDs broadcast in the clear, so no persistent plaintext identifier ties a device to a person across sessions. Forged alerts stop being viable. LXMF adds store and forward, so messages reach nodes that were out of range or powered off. And it requires no internet, DNS, central server, or addressing authority, so a fully airgapped local mesh is a first-class use case rather than a workaround.

[microReticulum](https://github.com/attermann/microReticulum) is the C++ port that runs the stack standalone on the ESP32, preserving the self-contained pager form factor. That is what we are building on.

---

## Hardware

Per node, roughly $15 to $25:

| Part | Notes |
|---|---|
| ESP32-WROOM-32 dev board | Plain WROOM-32, not S3 |
| SX1276 LoRa module | Generic module. See the identification note below. |
| SSD1306 OLED (I2C) | See the resolution note below |
| Passive piezo buzzer | GPIO 27 |
| 3.7V LiPo battery | 1500mAh tested |
| USB-C LiPo charging module | |
| Antenna | **Must match the band. See the 915MHz item in Open Bugs.** |

> **Always attach the antenna before powering the radio.** Transmitting without one can reflect RF energy back into the power amplifier and damage the chip.

### Wiring

| Peripheral | Pin | ESP32 GPIO |
|---|---|---|
| **SX1276 (SPI)** | NSS/CS | 5 |
| | SCK | 18 |
| | MOSI | 23 |
| | MISO | 19 |
| | RST | 14 |
| | DIO0 | 26 |
| | DIO1 | unwired |
| | VCC | 3V3 |
| | GND | GND |
| **OLED (I2C)** | SDA | 21 |
| | SCL | 22 |
| | VCC | 3V3 |
| | GND | GND |
| **Buzzer** | Signal | 27 |
| | Other leg | GND |
| **Button** | Signal | 25 (`INPUT_PULLUP`) |
| | Other leg | GND |

The LoRa module is **3.3V only**. Do not power it from 5V or VIN.

### Chip identification gotcha

Modules sold as "SX1278" or "RA-02" frequently are **SX1276** silicon. Both return `0x12` from the version register (`0x42`), so a raw SPI read will not distinguish them. Under RadioLib, a mismatched constructor gives `-2` (`RADIOLIB_ERR_CHIP_NOT_FOUND`) despite correct wiring, so swap `SX1278` for `SX1276`. Do not trust seller listings. Verify per unit.

### OLED resolution differs by track

- Legacy track: SSD1306 **128x32**, the 0.91 inch panel
- Reticulum track: SSD1306 **128x64**, confirmed against a known-good sketch

Both are SSD1306 at `0x3C`. The driver was never the issue, resolution was. Check which panel is physically in front of you before flashing either tree, and before ordering a batch.

---

## Firmware, Reticulum track (current)

PlatformIO. Layout:

```
platformio.ini
src/main.cpp
lib/lora_interface/LoRaInterface.{h,cpp}   # vendored from microReticulum
```

`LoRaInterface` comes from microReticulum's own reference examples, pulled from source rather than reimplemented, extended with a `BOARD_CUSTOM_WROOM_SX1276` branch matching the wiring above.

Setup follows the upstream `examples/lora_announce/src/main.cpp` pattern. Note that `RNS::Reticulum`, `Identity`, `Destination`, and `Interface` are lightweight handle objects, and `reticulum.start()` and `.loop()` are **instance** methods, not static.

**Fixed identities.** Each board is flashed with the same firmware but a distinct hardcoded private key (`RNS::Identity(false)` plus `load_private_key()`), so destination hashes stay stable across reflashes instead of randomizing every boot.

**Display.** Four screens auto-cycling every 3s, no buttons: RNS status, channel and hit count, last Flock detection, last received RNS message.

**Buzzer.** Beeps on a confirmed Flock OUI hit.

**Serial input.** `checkSerialInput()` lets you type a line plus Enter on a USB-connected board to broadcast it as an RNS announce with the text in `app_data`. This is the range-testing harness.

**Persistence.** Known destinations are in memory only, per boot, by design for now.

### Required build flags

```ini
board_build.partitions = huge_app.csv
upload_speed = 115200
build_flags =
    -DRNS_PERSIST_KNOWN_DESTINATIONS=0
```

Each of these is load-bearing. See Solved Issues.

### Integrated scanner

**This is the only place Flock detection exists.** The standalone `esp32_recon.ino` is a raw capture tool with no OUI table and no matching logic. `src/main.cpp` holds the 31-entry `FLOCK_OUIS` table and is the source of truth for it.

WiFi promiscuous plus BLE detection: 31-entry OUI table, dedup cache, wildcard-probe heuristic. Independently written, implementing the same publicly documented detection concepts as the oui-spy and flock-you research rather than deriving from those codebases.

**Passive receive only.** It reads frames already being broadcast. It does not connect to, probe, or interact with any network or device.

---

## Open bugs

**1. `app_data` loss at range. Unresolved, highest priority.**
At roughly 300ft, announces arrive but `app_data` does not reach the receiving handler. This is *not* the persistence bug fixed earlier (see Solved Issues), since that fix is in place and fixed the bench case. Leading theory: a marginal link corrupts or drops trailing packet bytes while still passing CRC as valid.
*Plan:* stepped-distance retest at 10, 100, 200, and 300ft with TRACE logging enabled, correlating loss against RSSI and SNR to see whether it tracks signal quality.

**2. Wrong ISM band.**
Currently running at **433MHz**, which is Region 1 (EU). The US unlicensed band is **915MHz**. Plan is a genuine HopeRF RFM95W, the same SX1276 silicon and pin-compatible with the wiring above. The only firmware change is the `frequency` constant in the vendored `LoRaInterface.h`, from `433.0` to `915.0`.

**3. Missed camera detection.**
A real-world test found a camera the firmware did not flag. One theory is that this camera's OUI is not in the 31-entry `FLOCK_OUIS` table, but see bug 4 first: if the test used the standalone sketch, no WiFi frames were captured at all and no matching code exists there either way.
*Plan:* fix bug 4, re-test, and only then get the camera's actual MAC and diff it against the current list.

**4. Standalone scanner captures no WiFi.**
`firmware/scanner-standalone/esp32_recon.ino` still uses `WIFI_MODE_NULL` in `setup()`, which silently yields zero WiFi rows. The fix (`WiFi.mode(WIFI_STA)` plus `WiFi.disconnect()`) went into the Reticulum firmware and was never backported. In practice that sketch is BLE-only right now. The sample capture is 69 BLE rows and 0 WiFi rows.

**5. BLE detection is a stub.**
Dedup only, no matching. Deferred deliberately, since WiFi OUI detection is the field-tested path.

---

## Solved issues

Keep these. They cost real time.

| Symptom | Cause | Fix |
|---|---|---|
| Compile error on RNS includes | Missing headers | Add `microStore/FileSystem.h` and `microStore/Adapters/UniversalFileSystem.h` |
| Flash overflow, roughly 2.1MB firmware | `no_ota.csv` gives only 2MB | `huge_app.csv`, roughly a 3MB app partition |
| Garbled or wrong OLED output | Driver and resolution mismatch, **not** wiring or address. An I2C scan showed `0x3C` throughout. | Confirm the panel against a known-good sketch before trusting the constant |
| `app_data` empty at the handler | `Identity::remember()` failing to write to the persistent known-destinations store, so `recall_app_data()` found nothing right before dispatch. Raw bytes were intact in the over-the-air hex dump the entire time. | `-DRNS_PERSIST_KNOWN_DESTINATIONS=0` |
| `termios.error` on upload (macOS) | Some CH340 and CP210x macOS driver versions choke at high speed | `upload_speed = 115200` |
| "text section exceeds available space" (Arduino IDE) | Default partition scheme | *Huge APP (3MB No OTA/1MB SPIFFS)* |
| Serial permission denied (Linux) | CH340, VID `1A86` PID `7523` | udev: `SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666"` |
| Arduino IDE AppImage will not launch (Arch-family) | Missing FUSE 2 | Install `fuse2`, or run `--appimage-extract-and-run` |

**Debugging note:** TRACE logging needs *both* a runtime `RNS::loglevel()` call and the compile-time flag `-DRNS_LOG_LEVEL=RNS_LOG_LEVEL_TRACE`. Log macros compile to no-ops below that level, so the runtime call alone silently does nothing.

---

## Laptop-side tooling

**Reticulum test harness.** Python RNS scripts for exercising the protocol layer over `AutoInterface` (local network multicast), independent of LoRa hardware. Useful for isolating whether a problem is in the protocol or in the radio.

| File | Purpose |
|---|---|
| `listener.py` | Receives announces |
| `send_test_announce.py` | Sends a test announce |
| `config_example` | RNS config template |

Requires UDP **29716** and **29717** for discovery and **42671** for data, open in the firewall. On CachyOS that is `ufw`.

**Scanner tooling**

| File | Purpose |
|---|---|
| `recon_logger.py` | pyserial listener, writes the HTML report directly |
| `generate_report.py` | Offline HTML report with sorting, filtering, live search, and match highlighting |
| `oui_lookup.csv` | IEEE OUI registry, 37,925 entries |

Note that these two tools do not currently chain together. See `tools/scanner/README.md`.

---

## Firmware, legacy RadioLib track

Retained as reference. Arduino IDE, libraries: RadioLib (jgromes), Adafruit SSD1306, Adafruit GFX.

Board manager URL:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

**Packet format:** `<packet_id>:<hop_count>:<message>`, a random 32-bit ID, a hop count decremented per relay and dropped at 0, and a plaintext message.

**Relay logic:** a rolling cache of the last 20 packet IDs. Already seen means drop silently. New means cache, beep, display, and rebroadcast with `hop_count - 1` after a random delay of 100 to 500ms. The delay prevents nodes that heard the same packet from colliding.

**Known gotcha:** ESP32 core 3.x defines its own `enableInterrupt()`. Our flag is named `rxInterruptEnabled` to avoid the collision.

---

## Base stations

Fixed sites hosted in local residences. Not yet built. Three things distinguish them from field nodes: mains power, antenna height and gain, and a Raspberry Pi backend for logging and a map interface.

**Antenna gain is the real range lever.** It is more effective than raising TX power, and it improves receive sensitivity too. A 5 to 8 dBi fiberglass base antenna mounted high, with a short LMR-195-or-better coax run. Coax loss adds up fast at 915MHz.

| Component | Spec |
|---|---|
| Compute | 2 or 3 Raspberry Pi 4B/5, 4 to 8GB RAM, running OpenMediaVault |
| Storage | 1 SSD per node, USB3 or NVMe HAT |
| Networking | Small unmanaged switch per site |
| Radio bridge | 1 ESP32 plus SX1276, USB to a Pi node |
| Power | AC-DC wall supply plus buck converter for the ESP32 rail |
| Backup power | 12V SLA (7Ah) on float charge |

OMV rather than TrueNAS because TrueNAS does not support ARM. SLA rather than lithium for backup because it tolerates permanent float charge indefinitely, which LiPo does not, the same reason alarm panels have used it for decades. Put decoupling caps near the SX1276 if it shares a rail, since LoRa modules are sensitive to supply noise during TX current spikes.

**Software:** Traccar in Docker on OMV, an open-source GPS tracking server with a built-in OSM frontend, live positions, history playback, and geofencing. It accepts HTTP GET reports via its OsmAnd protocol, which a Python and pyserial bridge calls per received beacon.

**Open question, tile serving.** Public OSM servers versus fully self-hosted offline is undecided and changes the architecture. If self-hosting, `overv/openstreetmap-tile-server` lacks solid ARM64 support, so **MapTiler Server** with its official ARM64 images, or a pre-rendered regional MBTiles extract, are better routes. Pull a regional extract from Geofabrik, not the full planet.

**Open question, field device location.** Either GPS (u-blox NEO-6M or NEO-M8N, UART, TinyGPS++ for NMEA) beaconing `id:lat:lon:timestamp:battery_mv`, or pre-assigned short zone codes per device with no GPS at all. GPS costs current draw and a cold-start delay of 30s or more, which is bad for a press-and-it-works alert button. Zone codes are cheaper and instant but coarse. With many devices beaconing, either approach needs randomized per-node jitter or a slotted schedule or packets will collide. Undecided.

---

## Device roles

- **Reporter units.** Full TX and RX. Carried by people submitting reports.
- **Listener units.** Receive only, enforced in firmware. Better battery life, and no RF emissions at all, so no radio signature to detect or direction-find.
- **Base stations.** Fixed, high-gain antenna, mains powered.

LoRa chips are transceivers, so there is no cheaper receive-only part. The listener role is a firmware restriction chosen for power draw and RF signature, not cost.

**Range:** a few hundred meters to a few kilometers, heavily dependent on terrain, elevation, and obstructions. See Open Bug 1 before trusting anything past bench distance.

---

## Roadmap

**Immediate**
- [ ] Diagnose `app_data` loss at range with a stepped distance test and RSSI/SNR correlation
- [ ] Source RFM95W modules, move to 915MHz
- [ ] Fix the buzzer drive on the Reticulum track, since a passive piezo needs `tone()` rather than `digitalWrite()`
- [ ] Test whether the stack-local `filesystem` in `reticulum_setup()` is the real cause of the store write failures
- [ ] Backport the `WIFI_MODE_NULL` fix to `esp32_recon.ino`
- [ ] Re-test the missed camera with working WiFi capture before assuming a missing OUI
- [ ] Decide between `recon_logger.py` and `generate_report.py`, which no longer chain

**Near term**
- [ ] Wire and validate the physical report button, one press per alert, with a 30 to 60s cooldown lockout so a stuck button cannot flood the mesh
- [ ] Validate multi-hop relay with 3 or more nodes
- [ ] Integrate the LiPo and charging module, then measure real battery life
- [ ] Implement and test the listener-only firmware variant
- [ ] Implement BLE-side detection matching

**Medium term**
- [ ] Field range testing across the actual coverage area
- [ ] First base station build
- [ ] Resolve the OSM tile-serving and GPS-versus-zone-code questions
- [ ] Decide message categories beyond a single generic alert
- [ ] Decide alert dismissal behavior. The legacy track auto-clears after 5s; button-dismiss may be safer.

**Long term**
- [ ] Retire the legacy RadioLib track
- [ ] Persistent known-destinations store, currently disabled as a bug workaround
- [ ] Enclosure design
- [ ] Documentation for non-technical users

---

## Repository layout

```
firmware/
  reticulum-node/          # PlatformIO, current track
    platformio.ini
    src/main.cpp
    lib/lora_interface/    # vendored microReticulum LoRaInterface
  legacy-radiolib/         # Arduino IDE, reference and fallback
  scanner-standalone/      # esp32_recon.ino
  diagnostics/             # I2C scanner, raw SPI chip ID, solo radio test
tools/
  rns/                     # listener.py, send_test_announce.py, config_example
  scanner/                 # recon_logger.py, generate_report.py, oui_lookup.csv
docs/
  wiring.md
```

Every folder carries a README describing its contents and gotchas. `basestation/` is not in the tree at all, since that subsystem is still a design rather than code.

---

## Excluded on purpose

`scan_log.csv` and `report.html` are not committed. Captures contain real MACs, device names, and timestamps that together imply where and when you were scanning. `.gitignore` covers both patterns.

---

## Contributing

Early-stage work on a system people may eventually depend on. Two requests:

1. **Do not open issues containing operational details** such as deployment locations, who has which devices, or coverage areas. Keep this repo technical.
2. **Open Bug 1, `app_data` loss at range, is the highest-value contribution right now.** Radio-layer and Reticulum experience are both welcome.

---

## License

Apache License 2.0. See [`LICENSE`](LICENSE).

Apache 2.0 was chosen over MIT for the explicit patent grant: contributors grant patent rights along with copyright, so nobody can contribute code and later assert a patent against people using it. The attribution and notice-of-changes requirements also fit a project where knowing which fork you are running matters.

Third-party components keep their own licenses. `lib/lora_interface/` is vendored from [microReticulum](https://github.com/attermann/microReticulum) (MIT). RadioLib and the Adafruit libraries are MIT. `oui_lookup.csv` is derived from the public IEEE OUI registry.
