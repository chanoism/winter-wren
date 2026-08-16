# Standalone passive scanner

ESP32 WiFi/BLE sniffer that dumps every management frame and BLE
advertisement it hears as CSV over serial.

**Passive receive only.** Reads frames already being broadcast. Does not
connect to, probe, or interact with any network or device.

| Path | Status |
|---|---|
| `esp32_recon.ino` | ✅ |

## What it actually does

- WiFi promiscuous mode, channel hop 1–13 at 300ms dwell
- Management frames only: beacon (8), probe request (4), probe response (5)
- BLE active scan, results cleared every 10s
- Shared MAC dedup cache, 256 entries, 30s cooldown, used by both radios
- CSV over serial at 115200, commas in SSIDs and BLE names rewritten to
  semicolons so the format survives

Output columns (the logger prepends a timestamp and appends a vendor):

```
type,mac,name_or_ssid,rssi,channel,extra
```

## ⚠️ Two things this sketch does NOT do

**1. It has no Flock detection.** There is no OUI table, no
`matchesFlockOUI()`, no wildcard-probe heuristic in this file. It is a
raw capture tool. All of the detection logic lives in
`../reticulum-node/src/main.cpp` — that file, not this one, is the
source of truth for the 31-entry `FLOCK_OUIS` table.

A consequence worth knowing: `../../tools/scanner/generate_report.py`
has a "Flock matches only" filter that looks for `FLOCK` in the `extra`
column. This firmware never writes that string, so that filter will
always show zero rows against captures from this sketch.

**2. WiFi capture is currently broken here.** `setup()` uses
`WIFI_MODE_NULL`, which silently yields zero WiFi rows on current ESP32
core versions. This was diagnosed and fixed in the Reticulum firmware
but never backported to this sketch.

Fix — replace the `WIFI_MODE_NULL` lines with what `main.cpp` uses:

```cpp
WiFi.mode(WIFI_STA);
WiFi.disconnect();
esp_wifi_set_storage(WIFI_STORAGE_RAM);
esp_wifi_set_promiscuous(true);
esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);
esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
```

Until that's done, this sketch is BLE-only in practice.

## Build

Arduino IDE. Partition scheme must be *Huge APP (3MB No OTA/1MB SPIFFS)*
or you get "text section exceeds available space."
