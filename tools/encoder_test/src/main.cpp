// SPDX-License-Identifier: Apache-2.0
// Seesaw rotary encoder isolation test. Wiring (same bus as the OLED):
//   encoder VIN -> 3V3, GND -> GND, SDA -> GPIO6 (D5), SCL -> GPIO44 (D7)
// Expect: a device at 0x36 and product id 4991.
#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_seesaw.h"

#define SDA_PIN 6
#define SCL_PIN 44
#define SEESAW_ADDR 0x36
#define SS_SWITCH 24            // encoder push switch pin on the Seesaw

Adafruit_seesaw ss(&Wire);
bool encOk = false;
int32_t lastPos = 0;
bool lastPressed = false;
uint32_t lastScan = 0;
int lastCount = -1;

static int scanSeesawRange(bool verbose) {
  int found = 0;
  for (uint8_t a = 0x36; a <= 0x3D; a++) {
    if (a == 0x3C) continue;    // OLED
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (verbose) Serial.printf("  device at 0x%02X\n", a);
      found++;
    }
  }
  return found;
}

static void tryInit() {
  if (!ss.begin(SEESAW_ADDR)) { encOk = false; return; }
  uint32_t ver = (ss.getVersion() >> 16) & 0xFFFF;
  Serial.printf("seesaw up, product id %lu (expect 4991)\n", (unsigned long)ver);
  ss.pinMode(SS_SWITCH, INPUT_PULLUP);
  lastPos = ss.getEncoderPosition();
  encOk = (ver == 4991);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);
  Serial.println("\n=== encoder test ===");

  // Remap BEFORE touching the encoder: begin() on the default pins hangs with no output.
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(50);

  Serial.println("boot scan:");
  int n = scanSeesawRange(true);
  if (!n) Serial.println("  nothing at 0x36-0x3D: wiring fault (SDA/SCL swapped, VIN/GND, loose joint)");
  lastCount = n;
  tryInit();
}

void loop() {
  if (!encOk) {
    if (millis() - lastScan > 2000) {
      lastScan = millis();
      int n = scanSeesawRange(false);
      if (n != lastCount) { Serial.printf("device count changed: %d\n", n); lastCount = n; }
      if (n) tryInit();
    }
    return;
  }

  int32_t pos = ss.getEncoderPosition();
  if (pos != lastPos) {
    Serial.printf("[turn] %s  pos %ld\n", pos > lastPos ? "CW " : "CCW", (long)pos);
    lastPos = pos;
  }
  bool pressed = !ss.digitalRead(SS_SWITCH);
  if (pressed != lastPressed) {
    if (pressed) Serial.println("[press]");
    lastPressed = pressed;
  }
  delay(10);
}
