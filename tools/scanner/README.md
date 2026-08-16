# Scanner tooling (laptop-side)

| Path | Status |
|---|---|
| `recon_logger.py` | ✅ |
| `generate_report.py` | ✅ |
| `oui_lookup.csv` | ✅ 37,925 entries |

## ⚠️ These two tools don't currently chain together

They were written at different times and the pipeline between them is
broken:

- **`recon_logger.py`** reads serial and writes `report.html` **directly**.
  It does not write a CSV at all — its docstring says so explicitly.
- **`generate_report.py`** reads **`scan_log.csv`** and writes
  `report.html`.

So `generate_report.py` has no input unless you produce `scan_log.csv`
some other way, and `recon_logger.py` makes `generate_report.py`
redundant. Pick one:

- Keep `recon_logger.py` alone (simplest — one step, live-refreshing
  report) and delete `generate_report.py`, **or**
- Add a `--csv` output back to `recon_logger.py` so the raw capture is
  retained and `generate_report.py` becomes the rendering step.

The second is probably worth it: `recon_logger.py` currently keeps every
row in memory and rewrites the whole HTML file every 5 seconds, so a
long capture gets slow and a crash loses everything. A CSV append is
cheap and durable.

`generate_report.py` is also the better of the two reports — it has the
Flock-match highlighting, badge, and filter that `recon_logger.py`'s
inline version lacks.

## Usage

```sh
python3 -m venv .venv && source .venv/bin/activate
pip install pyserial

python3 recon_logger.py --port /dev/ttyUSB0 --out report.html
# or, against an existing CSV:
python3 generate_report.py --in scan_log.csv --out report.html
```

Both reports are fully self-contained — inline CSS and JS, no CDN, no
network calls. They open offline anywhere.

## `oui_lookup.csv`

IEEE OUI registry, `oui,vendor`, 6-hex-digit prefixes with no
separators. Most observed MACs are randomized or locally administered
and will not resolve — that's expected. In the sample capture, 3 vendors
resolved out of 23 unique MACs.

## Output files are gitignored

`scan_log.csv` and `report*.html` contain real MAC addresses, BLE device
names, and timestamps — which together imply where and when you were
scanning. `.gitignore` covers `scan_*.csv` and `report*.html`. Keep it
that way, and don't paste captures into issues.

## Serial permissions on Linux

CH340, VID `1A86`, PID `7523`. `/etc/udev/rules.d/99-esp32.rules`:

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666"
```

Then `sudo udevadm control --reload-rules && sudo udevadm trigger`.
