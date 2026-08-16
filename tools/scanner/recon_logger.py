#!/usr/bin/env python3
"""
recon_logger.py
----------------
Reads CSV lines from an ESP32 running the WiFi+BLE recon sniffer sketch,
tags each row with the manufacturer looked up from the MAC's OUI, and
writes everything directly to a single self-contained HTML report
(report.html by default) — no CSV file, no intermediate step. The HTML
uses no CDN or external resources, so it opens and works completely
offline in any browser.

The report file is rewritten periodically as new detections come in, so
you can just refresh it in your browser while the logger keeps running.

Requires oui_lookup.csv (generated from the IEEE OUI registry) in the
same directory, or pass a custom path with --oui.

Usage:
    python recon_logger.py [--port /dev/ttyUSB0] [--baud 115200]
                            [--out report.html] [--oui oui_lookup.csv]
                            [--write-interval 5]
"""

import argparse
import csv
import html
import json
import os
import sys
import time

import serial


def load_oui_table(path: str) -> dict:
    """Loads oui_lookup.csv into a dict: {'AABBCC': 'Vendor Name'}"""
    table = {}
    if not os.path.exists(path):
        print(f"[warn] OUI lookup file not found at {path} — vendor column will be blank", file=sys.stderr)
        return table

    with open(path, newline="", encoding="utf-8", errors="ignore") as f:
        reader = csv.reader(f)
        next(reader, None)  # skip header
        for row in reader:
            if len(row) < 2:
                continue
            oui, vendor = row[0].strip().upper(), row[1].strip()
            table[oui] = vendor

    print(f"[laptop] loaded {len(table)} OUI entries from {path}")
    return table


def lookup_vendor(mac: str, oui_table: dict) -> str:
    if not mac or mac.count(":") != 5:
        return ""
    oui = mac.replace(":", "").upper()[:6]
    return oui_table.get(oui, "")


HEADER = ["timestamp", "type", "mac", "name_or_ssid", "rssi", "channel", "extra", "vendor"]


def build_html(rows) -> str:
    unique_macs = set()
    unique_vendors = set()
    wifi_count = 0
    ble_count = 0

    for row in rows:
        if len(row) < 3:
            continue
        row_type = row[1]
        mac = row[2]
        vendor = row[7] if len(row) > 7 else ""
        unique_macs.add(mac)
        if vendor:
            unique_vendors.add(vendor)
        if row_type.startswith("WIFI"):
            wifi_count += 1
        elif row_type == "BLE":
            ble_count += 1

    data_json = json.dumps(rows)
    header_json = json.dumps(HEADER)

    table_head = "".join(
        f'<th onclick="sortTable({i})">{html.escape(col)}</th>'
        for i, col in enumerate(HEADER)
    )

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Recon Scan Report</title>
<style>
  :root {{
    --bg: #0f1115;
    --panel: #171a21;
    --border: #2a2f3a;
    --text: #e6e8eb;
    --muted: #8b93a1;
    --accent: #5eb1ff;
    --wifi: #7ee787;
    --ble: #ffb454;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    margin: 0;
    padding: 24px;
  }}
  h1 {{ font-size: 20px; margin: 0 0 4px 0; }}
  .subtitle {{ color: var(--muted); font-size: 13px; margin-bottom: 20px; }}
  .summary {{ display: flex; gap: 12px; margin-bottom: 20px; flex-wrap: wrap; }}
  .stat {{
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 10px 16px;
    min-width: 120px;
  }}
  .stat .num {{ font-size: 22px; font-weight: 600; }}
  .stat .label {{ font-size: 12px; color: var(--muted); margin-top: 2px; }}
  .controls {{ display: flex; gap: 8px; margin-bottom: 16px; flex-wrap: wrap; align-items: center; }}
  input[type="text"] {{
    background: var(--panel);
    border: 1px solid var(--border);
    color: var(--text);
    padding: 8px 12px;
    border-radius: 6px;
    font-size: 13px;
    min-width: 240px;
  }}
  button {{
    background: var(--panel);
    border: 1px solid var(--border);
    color: var(--text);
    padding: 8px 14px;
    border-radius: 6px;
    font-size: 13px;
    cursor: pointer;
  }}
  button.active {{ border-color: var(--accent); color: var(--accent); }}
  table {{
    width: 100%;
    border-collapse: collapse;
    font-size: 13px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    overflow: hidden;
  }}
  th, td {{
    text-align: left;
    padding: 8px 10px;
    border-bottom: 1px solid var(--border);
    white-space: nowrap;
    max-width: 280px;
    overflow: hidden;
    text-overflow: ellipsis;
  }}
  th {{ background: #1c2029; cursor: pointer; user-select: none; color: var(--muted); font-weight: 600; }}
  th:hover {{ color: var(--text); }}
  tr:hover td {{ background: #1c2029; }}
  .type-wifi {{ color: var(--wifi); }}
  .type-ble {{ color: var(--ble); }}
  #rowcount {{ color: var(--muted); font-size: 12px; margin-bottom: 8px; }}
</style>
</head>
<body>
  <h1>Recon Scan Report</h1>
  <div class="subtitle">Live log from ESP32 recon sniffer &mdash; fully offline, no internet required. Refresh this page to see new detections.</div>

  <div class="summary">
    <div class="stat"><div class="num">{len(rows)}</div><div class="label">Total rows</div></div>
    <div class="stat"><div class="num">{len(unique_macs)}</div><div class="label">Unique MACs</div></div>
    <div class="stat"><div class="num">{len(unique_vendors)}</div><div class="label">Identified vendors</div></div>
    <div class="stat"><div class="num">{wifi_count}</div><div class="label">WiFi events</div></div>
    <div class="stat"><div class="num">{ble_count}</div><div class="label">BLE events</div></div>
  </div>

  <div class="controls">
    <input type="text" id="filterBox" placeholder="Filter (MAC, vendor, SSID...)" oninput="applyFilter()">
    <button id="btnAll" class="active" onclick="setTypeFilter('all')">All</button>
    <button id="btnWifi" onclick="setTypeFilter('wifi')">WiFi only</button>
    <button id="btnBle" onclick="setTypeFilter('ble')">BLE only</button>
  </div>

  <div id="rowcount"></div>
  <table id="dataTable">
    <thead><tr>{table_head}</tr></thead>
    <tbody id="tableBody"></tbody>
  </table>

<script>
const HEADER = {header_json};
const ALL_ROWS = {data_json};
let currentTypeFilter = 'all';
let sortCol = null;
let sortAsc = true;

function typeClass(t) {{
  if (t.startsWith('WIFI')) return 'type-wifi';
  if (t === 'BLE') return 'type-ble';
  return '';
}}

function renderRows(rows) {{
  const tbody = document.getElementById('tableBody');
  tbody.innerHTML = '';
  const frag = document.createDocumentFragment();
  for (const row of rows) {{
    const tr = document.createElement('tr');
    row.forEach((cell, i) => {{
      const td = document.createElement('td');
      td.textContent = cell;
      if (i === 1) td.className = typeClass(cell);
      tr.appendChild(td);
    }});
    frag.appendChild(tr);
  }}
  tbody.appendChild(frag);
  document.getElementById('rowcount').textContent = rows.length + ' rows shown';
}}

function getFiltered() {{
  const q = document.getElementById('filterBox').value.toLowerCase();
  return ALL_ROWS.filter(row => {{
    if (currentTypeFilter === 'wifi' && !row[1].startsWith('WIFI')) return false;
    if (currentTypeFilter === 'ble' && row[1] !== 'BLE') return false;
    if (!q) return true;
    return row.some(cell => (cell || '').toLowerCase().includes(q));
  }});
}}

function applyFilter() {{
  let rows = getFiltered();
  if (sortCol !== null) rows = sortRows(rows, sortCol, sortAsc);
  renderRows(rows);
}}

function setTypeFilter(t) {{
  currentTypeFilter = t;
  document.getElementById('btnAll').classList.toggle('active', t === 'all');
  document.getElementById('btnWifi').classList.toggle('active', t === 'wifi');
  document.getElementById('btnBle').classList.toggle('active', t === 'ble');
  applyFilter();
}}

function sortRows(rows, col, asc) {{
  return [...rows].sort((a, b) => {{
    const av = a[col] || '';
    const bv = b[col] || '';
    const an = parseFloat(av), bn = parseFloat(bv);
    let cmp;
    if (!isNaN(an) && !isNaN(bn) && av !== '' && bv !== '') {{
      cmp = an - bn;
    }} else {{
      cmp = av.localeCompare(bv);
    }}
    return asc ? cmp : -cmp;
  }});
}}

function sortTable(col) {{
  if (sortCol === col) {{
    sortAsc = !sortAsc;
  }} else {{
    sortCol = col;
    sortAsc = true;
  }}
  applyFilter();
}}

renderRows(ALL_ROWS);
</script>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description="ESP32 recon serial logger, writes directly to an offline HTML report")
    parser.add_argument("--port", default="/dev/ttyUSB0", help="Serial device for the ESP32")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--out", default="report.html", help="Output HTML file path")
    parser.add_argument("--oui", default="oui_lookup.csv", help="Path to OUI lookup CSV")
    parser.add_argument("--write-interval", type=float, default=5.0,
                         help="Seconds between HTML file rewrites (default: 5)")
    args = parser.parse_args()

    oui_table = load_oui_table(args.oui)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"[error] could not open {args.port}: {e}", file=sys.stderr)
        print("        check `dmesg | grep tty` to confirm the right device, "
              "and that your user has permission to access it.", file=sys.stderr)
        sys.exit(1)

    print(f"[laptop] connected to {args.port} @ {args.baud} baud")
    print(f"[laptop] writing live report to {args.out} (Ctrl+C to stop)")
    print(f"[laptop] open {args.out} in a browser and refresh it periodically to see new data")

    rows = []
    row_count = 0
    last_write = 0.0

    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode(errors="ignore").strip()
            if not line:
                continue
            if line.startswith("#"):
                print(f"[esp32] {line}")
                continue

            fields = line.split(",")
            mac = fields[1] if len(fields) > 1 else ""
            vendor = lookup_vendor(mac, oui_table)

            while len(fields) < 6:
                fields.append("")

            rows.append([f"{time.time():.3f}"] + fields[:6] + [vendor])
            row_count += 1

            now = time.time()
            if now - last_write >= args.write_interval:
                with open(args.out, "w", encoding="utf-8") as f:
                    f.write(build_html(rows))
                last_write = now
                print(f"[laptop] {row_count} rows logged, report updated")

    except KeyboardInterrupt:
        # final write on exit so nothing since the last interval is lost
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(build_html(rows))
        print(f"\n[laptop] stopped. {row_count} rows logged to {args.out}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
