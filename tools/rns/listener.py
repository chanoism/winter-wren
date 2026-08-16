#!/usr/bin/env python3
"""
listener.py -- Python RNS announce listener matching your ESP32
firmware's destination: app_name="flockwatch", aspect="detection".

Run this, and any node (real hardware or another Python instance)
announcing on "flockwatch.detection" will show up here with its
app_data decoded -- which is exactly what your firmware's
onFlockDetection() and maybeAnnounce() send.

IMPORTANT CAVEAT: this only hears announces that actually reach your
laptop over whatever interface(s) are enabled in your Reticulum
config (see config_example below). Your ESP32 firmware only has a
LoRa interface right now, so unless your laptop also has a LoRa radio
attached and configured, this script will only hear announces from
OTHER Python RNS instances on the same network (via AutoInterface),
not from the ESP32 itself. That's still a useful sanity check of the
protocol/format before you add hardware into the mix.
"""

import RNS
import time

APP_NAME = "flockwatch"
ASPECT = "detection"

def announce_received(destination_hash, announced_identity, app_data):
    print("=" * 60)
    print(f"Announce received from: {RNS.prettyhexrep(destination_hash)}")
    print(f"Identity hash:          {RNS.prettyhexrep(announced_identity.hash)}")
    if app_data:
        try:
            print(f"app_data (text):        {app_data.decode('utf-8')}")
        except UnicodeDecodeError:
            print(f"app_data (hex):         {app_data.hex()}")
    else:
        print("app_data:                (none)")
    print("=" * 60)

class FlockwatchAnnounceHandler:
    # aspect_filter uses the full dotted path: app_name.aspect
    # matching APP_NAME/"detection" in your firmware's
    # RNS::Destination(... "flockwatch", "detection") call.
    aspect_filter = f"{APP_NAME}.{ASPECT}"

    def received_announce(self, destination_hash, announced_identity, app_data):
        announce_received(destination_hash, announced_identity, app_data)

if __name__ == "__main__":
    # Uses whatever's in ~/.reticulum/config -- see config_example
    # in this folder for a minimal AutoInterface-only setup.
    reticulum = RNS.Reticulum()

    handler = FlockwatchAnnounceHandler()
    RNS.Transport.register_announce_handler(handler)

    print(f"Listening for announces on '{APP_NAME}.{ASPECT}'...")
    print("Press Ctrl+C to exit.\n")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nExiting.")
