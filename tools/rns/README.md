# Reticulum test harness (laptop-side)

Python RNS scripts for exercising the protocol layer over
`AutoInterface` (local network multicast), with no LoRa hardware
involved.

The point is isolation. When an announce doesn't arrive, this tells you
whether the problem is in Reticulum/your destination setup or in the
radio path. Run the same announce flow over the LAN: if it works there
and fails over LoRa, the bug is in the radio.

| Path | Status |
|---|---|
| `listener.py` | ✅ |
| `send_test_announce.py` | ✅ |
| `config_example` | ✅ |
| `requirements.txt` | ✅ |

## What these will and won't hear

**They will not hear the ESP32.** These scripts only receive announces
that reach your laptop over the interfaces in your Reticulum config,
and `config_example` enables `AutoInterface` only — local network
multicast. The firmware has a LoRa interface and nothing else, so
unless the laptop has its own LoRa radio configured, these two scripts
can only talk to each other and to other Python RNS instances on the
same LAN.

That's still worth having: it validates the destination naming, the
announce flow, and the `app_data` encoding before radio variables enter
the picture.

## Matching the firmware

Both scripts target the same destination the firmware creates:

| | Value |
|---|---|
| App name | `flockwatch` |
| Aspect | `detection` |
| Filter | `flockwatch.detection` |

If you rename either in `src/main.cpp`, change it in both scripts too or
the handler silently matches nothing.

## Usage

```sh
python3 -m venv .venv
source .venv/bin/activate          # fish: source .venv/bin/activate.fish
pip install -r requirements.txt

cp config_example ~/.reticulum/config    # back up any existing config first
```

Then two terminals:

```sh
python3 listener.py             # terminal 1
python3 send_test_announce.py   # terminal 2
```

The listener should print an announce with `app_data` reading
`TEST hit#1`, then a new one every 10s.

## Firewall

`AutoInterface` uses multicast discovery and will silently find nothing
if these are blocked — no error, just no peers. UDP:

| Port | Purpose |
|---|---|
| 29716 | discovery |
| 29717 | discovery |
| 42671 | data |

On CachyOS or anything else running `ufw`:

```sh
sudo ufw allow 29716/udp
sudo ufw allow 29717/udp
sudo ufw allow 42671/udp
```

`config_example` also sets `shared_instance_port = 37428` and
`instance_control_port = 37429`, but those are loopback-local and don't
need firewall rules.

## Notes

`send_test_announce.py` mints a fresh `RNS.Identity()` on every run, so
its destination hash changes each time. That's deliberate for a sanity
test, and it's the opposite of what the firmware does — nodes there load
a fixed key from `node_identity.h` so their hash stays stable. Don't
copy this script's pattern onto hardware.

`config_example` sets `enable_transport = False`. These instances
participate but don't route for others, which is right for a test
harness. The firmware likewise sets `transport_enabled(false)` while
registering its LoRa interface as `MODE_GATEWAY`.
