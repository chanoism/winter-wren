# Wiring reference

One GPIO map across every node, both firmware tracks. Print this and pin
it up at the bench rather than re-reading sketch comments each build.

## Pinout

| Peripheral | Pin | ESP32 GPIO |
|---|---|---|
| **SX1276 (SPI)** | NSS / CS | 5 |
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

## Before powering on

**Attach the antenna.** Transmitting without one can reflect RF energy
back into the power amplifier and damage the chip. Every time, no
exceptions.

**Check the LoRa module is on 3V3, not 5V or VIN.** These modules are
3.3V only. Reversed or over-volted power is a common cause of a board
that was working and suddenly isn't enumerating over USB at all.

## Button wiring

No pull-up resistor needed. `INPUT_PULLUP` holds the pin high internally;
pressing the button connects it to ground and reads LOW. That's why the
firmware checks for LOW as "pressed."

Debounce in software — mechanical buttons make and break contact several
times in the first few milliseconds, so one press becomes two or three
packets without it.

## Bring-up order

Don't wire everything and flash the full firmware. Each diagnostic
isolates one layer, and skipping ahead means debugging three things at
once:

1. `firmware/diagnostics/01_i2c_scan.ino` — OLED present at `0x3C`?
2. `firmware/diagnostics/02_lora_chip_id.ino` — RegVersion returns `0x12`?
3. `firmware/diagnostics/03_lora_solo_test.ino` — RadioLib initializes and transmits?
4. Full firmware, two boards.

## Chip identification

Modules sold as **SX1278** or **RA-02** are frequently **SX1276**
silicon. Both return `0x12` from RegVersion, so diagnostic 2 passing
does not tell you which class to use — it only tells you the wiring is
good.

Under RadioLib, a mismatched class gives `-2`
(`RADIOLIB_ERR_CHIP_NOT_FOUND`) with perfectly correct wiring. If
diagnostic 2 reads `0x12` and `radio.begin()` still returns `-2`, swap
the class in the sketch.

Verify every unit. Seller listings are not reliable on this.

## Display resolution

⚠️ Unresolved. The legacy track uses SSD1306 **128x32** (0.91" panel);
the Reticulum track was confirmed as SSD1306 **128x64** against a
known-good sketch, after an SH1106 wrong turn.

Both answer at `0x3C`, and the driver was never the problem — the
mismatch was resolution. Either two different panels are in circulation
here or one track has the wrong constant and renders anyway. **Settle
this before a batch order.**

## Diagnostic quick reference

| Symptom | First thing to check |
|---|---|
| `radio.begin()` returns `-2` | Run diagnostic 2. `0x12` → swap the SX1278/SX1276 class. `0x00`/`0xFF` → wiring or power. |
| Radio init passes, never receives | DIO0. Diagnostic 2 doesn't test it, so a DIO0 fault passes there and breaks interrupt-driven receive. |
| OLED blank | Run diagnostic 1. Device found → resolution or driver. Nothing found → wiring or power. |
| OLED garbled or half-drawn | Resolution constant. Not the address. |
| Board stopped enumerating over USB | Check for a short and for reversed power on the LoRa module. Try a different cable — many are charge-only. |
| Upload stalls at `Connecting...` | Hold BOOT during upload; drop upload speed to 115200. |
| `termios.error (22)` on macOS | Close any open serial monitor, unplug/replug, reselect the port. |
