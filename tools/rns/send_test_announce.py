#!/usr/bin/env python3
"""
send_test_announce.py -- creates a test identity/destination and
sends announces, mimicking what your ESP32 firmware does. Useful for
confirming listener.py actually works before hardware is involved:

    Terminal 1: python listener.py
    Terminal 2: python send_test_announce.py

Both need the same Reticulum config with AutoInterface enabled (see
config_example) and need to be reachable over your local network
(same LAN/WiFi) for AutoInterface's local multicast discovery to find
each other. This does NOT touch LoRa/radio at all -- it's purely
validating the RNS protocol side.
"""

import RNS
import time
import sys

APP_NAME = "flockwatch"
ASPECT = "detection"

if __name__ == "__main__":
    reticulum = RNS.Reticulum()

    # New identity each run -- fine for this sanity test.
    identity = RNS.Identity()

    destination = RNS.Destination(
        identity,
        RNS.Destination.IN,
        RNS.Destination.SINGLE,
        APP_NAME,
        ASPECT,
    )

    print(f"Test destination hash: {RNS.prettyhexrep(destination.hash)}")
    print("Sending an announce every 10s. Press Ctrl+C to stop.\n")

    count = 0
    try:
        while True:
            count += 1
            app_data = f"TEST hit#{count}".encode("utf-8")
            destination.announce(app_data=app_data)
            print(f"Sent announce #{count}: {app_data.decode()}")
            time.sleep(10)
    except KeyboardInterrupt:
        print("\nExiting.")
        sys.exit(0)
