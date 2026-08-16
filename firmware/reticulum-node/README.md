# Reticulum node firmware

**Current track.** Real Reticulum (RNS via microReticulum) over LoRa, with
the passive WiFi/BLE scanner integrated, on a plain ESP32-WROOM-32.

## Contents

| Path | Status |
|---|---|
| `platformio.ini` | ✅ |
| `src/main.cpp` | ✅ |
| `src/node_identity.example.h` | ✅ template |
| `src/node_identity.h` | ⬜ **you create this** — gitignored |
| `lib/lora_interface/LoRaInterface.h` | ✅ vendored |
| `lib/lora_interface/LoRaInterface.cpp` | ✅ vendored |

## First-time setup

```sh
cd firmware/reticulum-node
cp src/node_identity.example.h src/node_identity.h
python3 -c "import secrets; print(secrets.token_hex(65))"
# paste that into src/node_identity.h
pio run -t upload
```

**Generate a different key for every board.** Two boards sharing a key
announce as the same identity and the mesh cannot tell them apart.

## Architecture

`src/main.cpp` runs four things concurrently on one ESP32:

- **Reticulum stack** — `reticulum_setup()` registers a `microStore`
  filesystem, brings up `LoRaInterface` in `MODE_GATEWAY`, starts RNS,
  loads the fixed identity, and creates an `IN`/`SINGLE` destination
  under the `flockwatch` app name with the `detection` aspect.
- **WiFi promiscuous scanner** — channel hop 1–13 at 300ms, management
  frames only (subtypes 4/5/8), 31-entry OUI table checked against both
  `addr2` and `addr1`, plus a wildcard-probe heuristic.
- **BLE scanner** — active scan, dedup only. Matching is a deliberate stub.
- **OLED** — 4 screens auto-rotating every 3s, redrawn every 1s.

A confirmed hit calls `onFlockDetection()`, which beeps and immediately
announces `FLOCK <mac> <label>` in `app_data`. There's also a periodic
keepalive announce every 60s, and `checkSerialInput()` broadcasts any
line you type over USB serial — that's the range-test harness.

`lib/lora_interface/` is vendored from microReticulum's
`examples/common/lora_interface/`, with one added board branch
(`BOARD_CUSTOM_WROOM_SX1276`) and one changed constant (frequency).
DIO1 is deliberately unwired — `loop()` polls the IRQ status register
over SPI rather than needing a hardware interrupt line, which frees
GPIO 27 for the buzzer.

## Known issues in this tree

**1. Buzzer drive is wrong for the hardware.** `buzzerBeep()` uses
`digitalWrite(HIGH/LOW)`, which drives an *active* buzzer. The hardware
is a *passive* piezo — it needs a tone/PWM signal and will be silent or
barely audible on a DC level. The legacy track already does this
correctly. Fix:

```cpp
void buzzerBeep(uint16_t durationMs = 150) {
  tone(PIN_BUZZER, 2500, durationMs);
  delay(durationMs);
}
```

**2. `filesystem` is a stack local.** In `reticulum_setup()`,
`microStore::FileSystem filesystem{...}` is declared inside the `try`
block and registered via `register_filesystem(filesystem)`, then goes
out of scope when the function returns. If that registration keeps a
reference rather than taking ownership, every later store operation
touches a destroyed object. This is a plausible suspect for the
known-destinations write failures that `-DRNS_PERSIST_KNOWN_DESTINATIONS=0`
currently works around, and possibly for the range bug too. Worth making
it `static` or a file-scope global and retesting with persistence back
on before assuming the store engine itself is broken.

**3. Wrong ISM band.** `frequency` in `LoRaInterface.h` is `433.0`
(Region 1 / EU). US unlicensed LoRa is **915MHz**. RFM95W modules are
the same SX1276 silicon and pin-compatible; this constant is the only
firmware change the swap needs.

**4. `app_data` loss at ~300ft.** Unresolved. See the root README.

**5. Display overflow.** Screen 3 prints `lastReceivedText` (up to 48
chars) at `setTextSize(2)`, which fits roughly 10 characters per line on
a 128x64 panel. Longer messages run off the screen.

## Build flags

Every flag in `platformio.ini` is load-bearing and commented in place.
The two most easily lost:

- `board_build.partitions = huge_app.csv` — the combined firmware is
  ~2.1MB and overflows the default and `no_ota.csv` schemes.
- `upload_speed = 115200` — some CH340/CP210x macOS drivers throw
  `termios.error` above this.

For TRACE logging you need **both** `-DRNS_LOG_LEVEL=RNS_LOG_LEVEL_TRACE`
at compile time (the macros compile to no-ops otherwise) and a runtime
`RNS::loglevel(RNS::LOG_TRACE)` call in `setup()`. Currently set to INFO
in both places.
