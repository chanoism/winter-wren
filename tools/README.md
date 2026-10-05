# tools/

Standalone bring-up sketches. Each is its own PlatformIO project. They were used, in this order, to prove the hardware before the full firmware went on.

| Folder | Proves | Pass criteria |
|---|---|---|
| `lora_pingpong/` | B2B pin map, TCXO/LDO config, antennas, link | Two boards print `[RX] "node XXXX #n"` from each other with RSSI/SNR, and the two node IDs differ |
| `i2c_oled_test/` | I2C remap (SDA 6 / SCL 44) and the OLED | Bus scan lists `0x3C`; screen shows "I2C OK" |
| `encoder_test/` | Seesaw encoder wiring | Boot scan lists `0x36`, product id `4991`, `[turn]` and `[press]` lines appear |

```bash
cd tools/lora_pingpong     # or i2c_oled_test / encoder_test
pio run -t upload
pio device monitor
```

The native-USB S3 re-enumerates on every reset, so the serial port can vanish and return. Reopen the monitor if that happens.

## Gotchas these sketches already handle

- **Phantom receive in ping-pong:** DIO1 fires on TxDone as well as RxDone. The sketch clears `gotPacket` before `startReceive()` after transmitting, otherwise it reports its own packet as received.
- **Duplicate node IDs:** the low 16 bits of the MAC matched on both boards. The ID uses bits 47..32.
- **Encoder hang:** `Wire.begin(6, 44)` must run before `encoder.begin()` or `display.begin()`.
- **Kit vs header board:** `lora_pingpong` pins are for the **KIT** (B2B). The separate header board uses different pins (NSS 4, RST 3, BUSY 2, DIO1 1).

ping-pong radio settings are SF9 / CR 4/7 / 14 dBm (the 750 ft baseline). The main firmware uses SF8 / CR 4/5 / 17 dBm, so change one of them if you want an apples-to-apples range comparison.
