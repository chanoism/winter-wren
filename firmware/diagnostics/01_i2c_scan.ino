/*
 * Diagnostic 1 - I2C bus scan
 *
 * Confirms the OLED is present and responding before you fight with
 * driver or resolution settings. SSD1306 panels normally answer at 0x3C
 * (occasionally 0x3D).
 *
 * If nothing is found: wiring or power, not code.
 */

#include <Wire.h>

#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Serial.println("Scanning I2C bus...");
}

void loop() {
  int found = 0;

  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.print("Device found at 0x");
      Serial.println(address, HEX);
      found++;
    }
  }

  if (found == 0) {
    Serial.println("No I2C devices found - check wiring and power.");
  }

  delay(3000);
}
