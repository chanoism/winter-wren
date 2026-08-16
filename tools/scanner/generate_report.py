#!/usr/bin/env python3
"""
generate_report.py
--------------------
Reads scan_log.csv (produced by recon_logger.py) and generates a single
self-contained HTML file you can open in any browser with no internet
connection required — all CSS/JS is inline, nothing loads from a CDN.

Usage:
    python generate_report.py [--in scan_log.csv] [--out report.html]

Features in the generated report:
    - Sortable columns (click any header)
    - Live text filter box (type to filter across all fields)
    - Type filter buttons (All / WiFi / BLE / Flock matches)
    - Rows with a Flock OUI match (from the firmware's "extra" column)
      are highlighted and flagged with a badge
    - Summary counts at the top (total rows, unique MACs, unique vendors,
      Flock match count)
"""

import argparse
import csv
import html
import json
import os
import sys


def load_rows(path: str):
    if not os.path.exists(path):
        print(f"[error] {path} not found", file=sys.stderr)
        sys.exit(1)

    with open(path, newline="", encoding="utf-8", errors="ignore") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        rows = [row for row in reader if row]
    return header, rows


def build_html(header, rows) -> str:
    unique_macs = set()
    unique_vendors = set()
    wifi_count = 0
    ble_count = 0
    flock_count = 0

    for row in rows:
        if len(row) < 3:
            continue
        row_type = row[1]
        mac = row[2]
        vendor = row[7] if len(row) > 7 else ""
        extra = row[6] if len(row) > 6 else ""
        unique_macs.add(mac)
        if vendor:
            unique_vendors.add(vendor)
        if row_type.startswith("WIFI"):
            wifi_count += 1
        elif row_type == "BLE":
            ble_count += 1
        if extra and "FLOCK" in extra:
            flock_count += 1

    data_json = json.dumps(rows)
    header_json = json.dumps(header if header else [])

    escaped_header = header if header else []

    table_head = "".join(
        f'<th onclick="sortTable({i})">{html.escape(col)}</th>'
        for i, col in enumerate(escaped_header)
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
    --flock: #ff6b6b;
    --flock-bg: #2a1414;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    margin: 0;
    padding: 24px;
  }}
  h1 {{
    font-size: 20px;
    margin: 0 0 4px 0;
  }}
  .subtitle {{
    color: var(--muted);
    font-size: 13px;
    margin-bottom: 20px;
  }}
  .summary {{
    display: flex;
    gap: 12px;
    margin-bottom: 20px;
    flex-wrap: wrap;
  }}
  .stat {{
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 10px 16px;
    min-width: 120px;
  }}
  .stat.flock-stat {{
    border-color: var(--flock);
  }}
  .stat .num {{
    font-size: 22px;
    font-weight: 600;
  }}
  .stat.flock-stat .num {{
    color: var(--flock);
  }}
  .stat .label {{
    font-size: 12px;
    color: var(--muted);
    margin-top: 2px;
  }}
  .controls {{
    display: flex;
    gap: 8px;
    margin-bottom: 16px;
    flex-wrap: wrap;
    align-items: center;
  }}
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
  button.active {{
    border-color: var(--accent);
    color: var(--accent);
  }}
  button#btnFlock.active {{
    border-color: var(--flock);
    color: var(--flock);
  }}
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
  th {{
    background: #1c2029;
    cursor: pointer;
    user-select: none;
    color: var(--muted);
    font-weight: 600;
  }}
  th:hover {{ color: var(--text); }}
  tr:hover td {{ background: #1c2029; }}
  tr.flock-row td {{ background: var(--flock-bg); }}
  tr.flock-row:hover td {{ background: #3a1a1a; }}
  .type-wifi {{ color: var(--wifi); }}
  .type-ble {{ color: var(--ble); }}
  .flock-badge {{
    display: inline-block;
    background: var(--flock);
    color: #1a0808;
    font-size: 10px;
    font-weight: 700;
    padding: 2px 6px;
    border-radius: 4px;
    margin-left: 6px;
  }}
  #rowcount {{
    color: var(--muted);
    font-size: 12px;
    margin-bottom: 8px;
  }}
</style>
</head>
<body>
  <h1>Recon Scan Report</h1>
  <div class="subtitle">Generated offline from scan_log.csv &mdash; no internet required</div>

  <div class="summary">
    <div class="stat"><div class="num">{len(rows)}</div><div class="label">Total rows</div></div>
    <div class="stat"><div class="num">{len(unique_macs)}</div><div class="label">Unique MACs</div></div>
    <div class="stat"><div class="num">{len(unique_vendors)}</div><div class="label">Identified vendors</div></div>
    <div class="stat"><div class="num">{wifi_count}</div><div class="label">WiFi events</div></div>
    <div class="stat"><div class="num">{ble_count}</div><div class="label">BLE events</div></div>
    <div class="stat flock-stat"><div class="num">{flock_count}</div><div class="label">Flock OUI matches</div></div>
  </div>

  <div class="controls">
    <input type="text" id="filterBox" placeholder="Filter (MAC, vendor, SSID...)" oninput="applyFilter()">
    <button id="btnAll" class="active" onclick="setTypeFilter('all')">All</button>
    <button id="btnWifi" onclick="setTypeFilter('wifi')">WiFi only</button>
    <button id="btnBle" onclick="setTypeFilter('ble')">BLE only</button>
    <button id="btnFlock" onclick="setTypeFilter('flock')">Flock matches only</button>
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

function isFlockRow(row) {{
  const extra = row[6] || '';
  return extra.indexOf('FLOCK') !== -1;
}}

function renderRows(rows) {{
  const tbody = document.getElementById('tableBody');
  tbody.innerHTML = '';
  const frag = document.createDocumentFragment();
  for (const row of rows) {{
    const tr = document.createElement('tr');
    if (isFlockRow(row)) tr.className = 'flock-row';
    row.forEach((cell, i) => {{
      const td = document.createElement('td');
      td.textContent = cell;
      if (i === 1) td.className = typeClass(cell);
      if (i === 6 && isFlockRow(row)) {{
        const badge = document.createElement('span');
        badge.className = 'flock-badge';
        badge.textContent = 'FLOCK?';
        td.appendChild(badge);
      }}
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
    if (currentTypeFilter === 'flock' && !isFlockRow(row)) return false;
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
  document.getElementById('btnFlock').classList.toggle('active', t === 'flock');
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

// initial render
renderRows(ALL_ROWS);
</script>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description="Generate an offline HTML report from scan_log.csv")
    parser.add_argument("--in", dest="input_path", default="scan_log.csv", help="Input CSV path")
    parser.add_argument("--out", dest="output_path", default="report.html", help="Output HTML path")
    args = parser.parse_args()

    header, rows = load_rows(args.input_path)
    html_content = build_html(header, rows)

    with open(args.output_path, "w", encoding="utf-8") as f:
        f.write(html_content)

    print(f"[report] wrote {len(rows)} rows to {args.output_path}")
    print(f"[report] open it directly in any browser, no internet needed")


if __name__ == "__main__":
    main()
