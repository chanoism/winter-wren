# Flock/Mesh Node — Build Guide

XIAO ESP32S3 + Wio-SX1262 kit, SSD1306 OLED, Seesaw rotary encoder, LiPo battery.

Covers the detection bug fix, antenna upgrade, wiring, power, and enclosure.

---

## 1. The "stops detecting until reset" bug

### What was wrong

`onFlockDetection()` was being called from inside the WiFi promiscuous
callback. That callback runs in the WiFi driver task and is expected to
return in microseconds. It was doing:

| Operation | Approximate cost |
|---|---|
| `Serial.printf()` | 1–5 ms |
| `buzzerBeep()` → `delay(150)` | 150 ms |
| `announce()` → blocking LoRa transmit at SF8 | ~600 ms |

Close to a second of blocking work inside a driver callback. This matches
your symptom exactly: detections work, then stop, and only a reset brings
them back. The first detection wedges the WiFi task, and nothing is seen
afterward.

### What changed

The callback now does one thing: copy the event into a 16-entry ring
buffer and return. `loop()` drains the buffer and does all the slow work.
This is the same pattern `colonelpanichacks/flock-you` uses — their source
comments explicitly that the WiFi task cannot safely call `Serial.print`
or `malloc`.

Also fixed alongside it:

- **Buzzer is non-blocking.** `buzzerBeep()` arms a timestamp;
  `buzzerService()` in `loop()` turns it off. The old `delay(150)` was
  also stalling LoRa receive polling.
- **Queue drops are counted, not blocked on.** A dropped duplicate
  detection costs nothing; a blocked WiFi task costs everything.
- **Drain is bounded** to 4 events per `loop()` iteration so a burst
  can't monopolise the loop.

### How to confirm it in the field

This is a hypothesis that fits the evidence, not a proven diagnosis. The
new heartbeat line tells you definitively. Every 10 seconds:

```
[hb] up=120s frames=48213 (mgmt=31002 data=17211) raw=3 hits=3 drops=0 ch=6 heap=142338
```

Read it like this:

| Observation | Meaning |
|---|---|
| `frames` climbing, `hits` climbing near a camera | Working. Bug fixed. |
| `frames` **stops climbing** while powered near traffic | WiFi callback wedged — the bug is NOT fixed, tell me |
| `frames` climbing, `hits` flat near a known camera | Callback alive; this is a *matching* problem, not a wedge. Completely different bug. |
| `drops` climbing | Detections arriving faster than `loop()` drains. Not fatal, but tell me. |
| `heap` falling steadily | Memory leak. Watch over 10+ minutes. |

That distinction is the whole point of the instrumentation. Previously
"it stopped working" could have meant either thing.

### Capturing a log while testing

```bash
# macOS
screen /dev/cu.usbmodem1101 115200 | tee flock_log.txt

# Linux
python3 -m serial.tools.miniterm /dev/ttyACM0 115200 | tee flock_log.txt
```

Detection lines are already CSV-ish and greppable:

```
[flock] hit AA:BB:CC:DD:EE:FF (IE_SIG) rssi=-61 ch=11 t=84213
```

---

## 2. Antenna and pigtail upgrade

### Which connector

The kit has **two** U.FL/IPEX connectors and they are not interchangeable:

- **On the Wio-SX1262 board** → LoRa, 915 MHz. This is the one to upgrade.
- **On the XIAO itself** → WiFi/BLE, 2.4 GHz. Leave the stock antenna on
  this one; it feeds your Flock detection.

Putting the 915 MHz antenna on the 2.4 GHz port will quietly degrade both
functions with no error message anywhere.

### What to buy

- **U.FL (IPEX-1) to SMA-female pigtail**, 10–15 cm. Bulkhead style with a
  nut and washer so it mounts through the enclosure wall.
- **915 MHz antenna**, SMA-male. You already have one.
- Check the gender: most pigtails are SMA-female bulkhead, matching an
  SMA-male antenna. **RP-SMA is a different standard** and won't mate
  properly — if the antenna has a pin in the centre it's SMA, if it has a
  socket it's RP-SMA.

### Fitting it

U.FL connectors are rated for roughly 30 mating cycles, and the board-side
connector is fragile.

1. Note the orientation of the existing antenna's connector before removing.
2. **Lift straight up**, never at an angle. A U.FL removal tool is ideal;
   a thin flathead levered evenly under both sides works.
3. To fit the pigtail: align it directly over the connector, confirm it's
   centred, then press straight down with a fingernail until it clicks.
4. **Never press at an angle** — that's what tears the connector off the pad.

### Safety

**Always have an antenna attached before transmitting.** Transmitting into
an open port reflects power back into the PA and damages it over time.
Connect the pigtail before power-on, not after.

### Measuring the improvement

You have a baseline: **750 ft, RSSI −123 dBm, SNR −10.5 dB, SF9, 14 dBm,
stock FPC antennas, receiver indoors.**

To compare meaningfully, change one thing at a time:

1. Pigtail + real antenna on **one** node only → measure at the same spot
2. Then both nodes → measure again
3. Then raise TX power to 22 dBm → measure again

Expect a few dB per end from the antennas. The bigger win is usually
**placement** — getting the fixed node outdoors, in a window, or up high
beats antenna gain in most real environments.

---

## 3. Wiring

### Pin budget

Everything the Wio-SX1262 consumes through the B2B connector:

| Signal | GPIO |
|---|---|
| NSS | 41 |
| DIO1 | 39 |
| RST | 42 |
| BUSY | 40 |
| RF_SW | 38 |
| SCK | 7 (D8) |
| MISO | 8 (D9) |
| MOSI | 9 (D10) |

Treat **D0–D4 (GPIO 1–5)** as unavailable — Seeed's BSP lists both a B2B
number and a header number for each control signal, implying they're
bonded to both.

That leaves three pins, and you need exactly three:

| Pin | GPIO | Use |
|---|---|---|
| D5 | 6 | I2C SDA |
| D7 | 44 | I2C SCL |
| D6 | 43 | Buzzer |

### Schematic

```
                    XIAO ESP32S3
                  (stacked on Wio-SX1262)
                 ┌──────────────────────┐
                 │                      │
                 │  3V3 ●               │
                 │  GND ●               │
                 │                      │
                 │  D5 (GPIO6)  ● SDA   │
                 │  D6 (GPIO43) ● BUZZ  │
                 │  D7 (GPIO44) ● SCL   │
                 └───┬───┬───┬───┬──────┘
                     │   │   │   │
        ┌────────────┘   │   │   └──────────────┐
        │   ┌────────────┘   └───────────┐      │
       3V3 GND                          SDA    SCL
        │   │                            │      │
        ├───┼────────────────────────────┼──────┤
        │   │                            │      │
   ┌────┴───┴────────────────────────────┴──────┴────┐
   │  VCC GND                           SDA    SCL   │
   │              SSD1306 OLED (0x3C)                │
   │  VCC GND                           SDA    SCL   │   ← second set of
   └────┬───┬────────────────────────────┬──────┬────┘      pads, or splice
        │   │                            │      │
   ┌────┴───┴────────────────────────────┴──────┴────┐
   │  VIN GND                           SDA    SCL   │
   │        Adafruit Seesaw Encoder (0x36)           │
   └──────────────────────────────────────────────────┘

   Buzzer:  D6 (GPIO43) ──── (+) buzzer (−) ──── GND
```

### Connection table

| From (XIAO) | To OLED | To Seesaw | Notes |
|---|---|---|---|
| 3V3 | VCC | VIN | **Not 5V** — S3 GPIOs are not 5V tolerant |
| GND | GND | GND | Common ground |
| D5 / GPIO6 | SDA | SDA | Shared bus |
| D7 / GPIO44 | SCL | SCL | Shared bus |
| D6 / GPIO43 | — | — | Buzzer + |

I2C is a bus: all devices sit on the same two wires. Daisy-chain
(XIAO → OLED → Seesaw) or star (both back to the XIAO) — electrically
identical, pick whichever suits your enclosure layout.

### Notes

- **Addresses don't collide.** OLED 0x3C, Seesaw 0x36.
- **Pull-ups:** both breakouts have onboard pull-ups (usually 4.7k or 10k).
  Two in parallel is fine. A third device is when it starts to matter.
- **GPIO43 is U0TXD** — the ROM bootloader prints its startup log there at
  every power-on, so you'll hear a brief click on the buzzer at boot.
  Harmless. It's on the buzzer rather than SCL deliberately: boot chatter
  on an I2C clock line would be worse.
- **Buzzer polarity:** active buzzers are polarised. Longer leg / `+`
  marking goes to GPIO43.
- **Buzzer current:** if yours draws more than ~20 mA, drive it through a
  small NPN transistor (2N3904 + 1k base resistor) rather than straight
  off the GPIO.

### Encoder behaviour

The firmware now supports a two-level UI, which is what makes a single
knob unambiguous:

| Mode | Rotate | Press |
|---|---|---|
| **AUTO** (default) | Takes over, halts rotation | Halts rotation |
| **BROWSE** | Change screen | Enter message list (screen 5) / resume auto |
| **SELECT** | Scroll messages | **Send highlighted message** |

- A small filled square in the top-right corner means rotation is halted.
- After 30 seconds with no input, auto-rotation resumes by itself — so
  bumping the knob and pocketing the device doesn't park the display
  forever.
- **If the encoder isn't wired yet, nothing breaks.** The firmware probes
  for it at 0x36 and falls back to auto-rotation, logging
  `[enc] no seesaw encoder at 0x36`.

### Screens

| # | Shows |
|---|---|
| 0 | RNS status, band |
| 1 | Current channel, hit count |
| 2 | Last detection: tier, MAC, seconds ago |
| 3 | Last received message (now word-wrapped) |
| 4 | **Message list** — scrollable, press to send |

Presets live in `PRESET_MESSAGES[]` near the top of the display section.
Edit freely; keep them under ~20 characters so they fit on one line.

The visual glitch you saw was text size 2 running off the panel edge. Both
the message list and the received-message screen now render at size 1 with
wrapping and a scrolling window.

---

## 4. Power and charging

### The good news

The Wio-SX1262 board has **lithium battery charge management built in**.
You do not need a separate TP4056 module. Plug in USB-C and it charges the
cell while running.

### Battery pads

`BAT+` and `BAT−` pads are on the **underside of the Wio-SX1262 board**
(not the XIAO). Your battery's JST connector won't mate directly — you'll
strip and solder, as you expected.

### Wiring it

> **Read this section fully before cutting anything.** A shorted LiPo
> vents, catches fire, and does so fast.

1. **Never cut both wires at once.** Cut one, strip and tin it, insulate
   it with heatshrink, *then* cut the other. A single slip of the cutters
   through both conductors is a dead short across the cell.
2. Identify polarity first — red is `+`, black is `−`. Confirm with a
   multimeter if there's any doubt; a fully charged cell reads ~4.2 V, a
   discharged one ~3.0 V.
3. Tin the board pads lightly before attaching the wires.
4. Solder **`−` first, then `+`**. If your iron slips while doing `+` and
   `−` isn't connected yet, nothing happens.
5. Heatshrink each joint individually, then sleeve both together for
   strain relief.
6. Before first power-on: check continuity between `BAT+` and `BAT−`. It
   should read open, not a short.

### Recommended addition

Put a **slide switch in the `+` line**. Every 3D case design for this kit
does this, and for good reason — without it the only way to power down is
unplugging the battery, which means opening the enclosure and flexing the
solder joints each time.

```
BAT+ ──── [SWITCH] ──── board BAT+
BAT− ─────────────────── board BAT−
```

A 2 mm SPDT slide switch is the common choice and fits most printed cases.

### Battery sizing

| Capacity | Typical runtime |
|---|---|
| 1100 mAh (703048) | ~10 hours |
| 2000 mAh | ~18 hours |

Those figures come from Meshtastic builds on this hardware. **Yours will
be shorter** — WiFi promiscuous mode plus continuous BLE scanning draws
considerably more than a Meshtastic node that mostly idles. Budget
roughly half, and measure it yourself before relying on it.

### Charging notes

- Charge current is set by the board; you don't control it.
- **Don't leave a fully charged pack on USB indefinitely.**
- The charge LED is on the Wio board. If your enclosure is opaque, either
  add a light pipe (a short piece of clear filament works) or a small hole.

---

## 5. Enclosure

### Measure first

I'm giving proportions and layout rather than exact numbers, because the
parts vary between suppliers. **Measure your actual parts with calipers
before designing.** Typical figures to check against:

| Part | Approximate size |
|---|---|
| XIAO + Wio stacked | 21 × 18 mm, ~12 mm tall with B2B |
| SSD1306 0.96" module | ~27 × 27 mm board, ~13 mm tall with pins |
| SSD1306 active area | ~22 × 11 mm |
| Seesaw encoder breakout | ~25 × 23 mm, ~5 mm + encoder body |
| EC11 encoder body | ~12 mm tall + shaft length |
| LiPo 1100 mAh (703048) | 7 × 30 × 48 mm |
| SMA bulkhead hole | 6.5 mm, plus flat for the nut |

### Layout

Two-part clamshell, parts stacked front to back:

```
        FRONT PANEL                        SIDE VIEW
   ┌─────────────────────┐          ┌──────────────────────┐
   │                     │          │ ▓▓ OLED              │  front
   │   ┌───────────┐     │          │ ──────────────────── │
   │   │   OLED    │     │          │ ░░ XIAO + Wio        │
   │   │  window   │     │          │ ──────────────────── │
   │   └───────────┘     │          │ ██ battery           │  back
   │                     │          └──────────────────────┘
   │         ( ● )       │  ← encoder shaft
   │                     │
   └─────────────────────┘

        LEFT EDGE                      RIGHT EDGE
   ┌─┐ USB-C cutout                ┌─┐ SMA bulkhead hole
   └─┘ 9 × 3.5 mm                  └─┘ 6.5 mm + nut flat
       slide switch                    (add later —
                                        IPEX stays inside for now)
```

### Design notes

**OLED window.** Cut to the *active area* (~22 × 11 mm), not the module
outline — the glass extends well past the lit region and a window sized to
the board looks sloppy. Leave 1 mm margin. Either leave it open or glue in
a thin acrylic/PETG sheet.

**Encoder mounting.** The shaft needs a 7 mm hole plus clearance for the
knurled nut. Decide **now** whether you want:

- **Front-facing knob** — simplest, standard EC11 mounting
- **Thumbwheel** (what you described) — EC11 mounted sideways, wheel
  protruding through a slot in the edge. Electrically identical, but you
  must wire the EC11 off-board to the Seesaw's pads and the slot needs
  designing in from the start

This decision constrains the whole case. Make it before modelling.

**USB-C cutout.** Measure from the actual board with the stack assembled.
Make it 1 mm oversize in each direction — cable housings vary a lot, and a
cutout that only fits the cable that came in the box is a recurring
annoyance.

**Antenna.** For now the IPEX antenna lives inside. Route it away from the
battery and along a wall rather than coiled — coiling detunes it. When you
add the SMA bulkhead later, put it on the opposite edge from the USB port
so you're not fighting two cables in one corner.

**Standoffs.** M2 heat-set inserts or printed posts for the XIAO stack.
Don't rely on the B2B connector to hold anything mechanically; it's an
electrical connector, not a structural one.

**Ventilation.** A few small slots near the XIAO. WiFi promiscuous plus
BLE plus LoRa generates real heat in a sealed box, and LiPo cells
genuinely dislike being warm.

### Existing models worth starting from

Rather than modelling from scratch, several published cases for this exact
kit are available on Printables and can be modified:

- Search "XIAO ESP32S3 Wio-SX1262 case" — one common design houses a
  1100 mAh LiPo (703048) and has a power switch, LED holes, and versions
  with and without an SMA cutout
- Most are parametric or at least simple enough to edit in
  Fusion/FreeCAD/OnShape

You'd be adding the OLED window and encoder hole, which is much less work
than starting over.

---

## 6. Assembly and test order

Do these in order. Each step verifies one thing, so a failure tells you
exactly what caused it.

1. **Flash the new firmware** to both boards. *Different
   `NODE_PRIVATE_KEY_HEX` per board (set in the gitignored
   `include/node_key.h`) — don't undo that.*
2. **Watch the heartbeat** for two minutes on USB. Confirm `frames` climbs
   steadily and `heap` is stable.
3. **Wire the OLED only.** Confirm screens render and rotate.
4. **Add the encoder.** Confirm `[enc] seesaw found at 0x36, product 4991`.
   Rotate → screen changes and the corner marker appears. Press on screen
   4 → message list. Press again → `[tx] preset message:` on serial.
5. **Confirm the other node receives it** — `[rns] app_data:` with your
   message text.
6. **Add the buzzer.** Confirm the boot click and a beep on send.
7. **Field test detection.** Drive to the camera that was missed. Watch
   whether `frames` keeps climbing after the first hit — that's the
   answer to the wedge question.
8. **Antenna upgrade**, one node at a time, measuring at a fixed spot.
9. **Battery**, with the switch. Measure actual runtime.
10. **Enclosure** last, once nothing else needs reworking.

### If the camera is still missed

With `frames` climbing but no hit, the detection logic isn't matching it.
Next step is to find out what the camera actually transmits, using the
existing `recon_logger.py` pipeline which logs *every* frame rather than
only matches:

1. Unstack the XIAO from the Wio board
2. Flash `esp32_recon.ino`
3. `python recon_logger.py --port /dev/ttyACM0 --out scan_log.csv`
4. Park near the camera for a few minutes
5. `python generate_report.py --in scan_log.csv --out report.html`
6. Look for MACs near the camera's location, check their OUIs against the
   33-entry table

If its OUI isn't in the list, add it. If it is in the list but was never
flagged, that's a logic bug worth bringing back to me with the capture.
