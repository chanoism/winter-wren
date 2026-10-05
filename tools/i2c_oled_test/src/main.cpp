// SPDX-License-Identifier: Apache-2.0
// I2C scan + OLED check. Expect the OLED at 0x3C.
// The B2B LoRa module occupies D4, so I2C is remapped to D5 (GPIO6, SDA)
// and D7 (GPIO44, SCL).
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SDA_PIN 6
#define SCL_PIN 44
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledOk = false;
uint32_t lastScan = 0, ticks = 0;

static int scanBus() {
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found device at 0x%02X\n", a);
      found++;
    }
  }
  if (!found) Serial.println("  no devices found: check SDA/SCL, power and pull-ups");
  return found;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);
  Serial.println("\n=== I2C / OLED test ===");

  Wire.begin(SDA_PIN, SCL_PIN);   // must come before display.begin()
  Wire.setTimeOut(50);
  scanBus();

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  Serial.println(oledOk ? "SSD1306 init OK" : "SSD1306 init FAILED");
  if (oledOk) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("I2C OK");
    display.setTextSize(1);
    display.println();
    display.printf("SDA %d  SCL %d\n", SDA_PIN, SCL_PIN);
    display.display();
  }
}

void loop() {
  if (millis() - lastScan > 5000) {
    lastScan = millis();
    Serial.println("rescan:");
    scanBus();
    if (oledOk) {
      display.fillRect(0, 48, 128, 16, SSD1306_BLACK);
      display.setCursor(0, 52);
      display.printf("alive %lu", (unsigned long)++ticks);
      display.display();
    }
  }
}
