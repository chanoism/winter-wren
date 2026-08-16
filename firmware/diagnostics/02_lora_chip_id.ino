/*
 * Diagnostic 2 - raw SPI read of the LoRa RegVersion register
 *
 * Bypasses RadioLib entirely. Talks straight to the chip over SPI and
 * reads register 0x42, which every SX127x returns 0x12 from.
 *
 * Use this when radio.begin() fails: it separates "the wiring is broken"
 * from "the wiring is fine but the library class is wrong."
 *
 *   0x12         chip is alive and wiring is good. If RadioLib still
 *                fails, swap the SX1278/SX1276 class in your sketch.
 *   0x00 / 0xFF  nothing responding. Power, ground, or an SPI line.
 *   anything else  unexpected - possibly not an SX127x at all.
 *
 * Note this test never touches DIO0, so a DIO0 fault will pass here and
 * still break interrupt-driven receive later.
 */

#include <SPI.h>

#define PIN_LORA_NSS  5
#define PIN_LORA_RST  14
#define PIN_LORA_SCK  18
#define PIN_LORA_MISO 19
#define PIN_LORA_MOSI 23

#define REG_VERSION 0x42

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_LORA_NSS, OUTPUT);
  pinMode(PIN_LORA_RST, OUTPUT);
  digitalWrite(PIN_LORA_NSS, HIGH);

  // hardware reset
  digitalWrite(PIN_LORA_RST, LOW);
  delay(10);
  digitalWrite(PIN_LORA_RST, HIGH);
  delay(10);

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

  digitalWrite(PIN_LORA_NSS, LOW);
  SPI.transfer(REG_VERSION & 0x7F);   // MSB low = read
  byte version = SPI.transfer(0x00);
  digitalWrite(PIN_LORA_NSS, HIGH);

  Serial.print("RegVersion = 0x");
  Serial.println(version, HEX);

  if (version == 0x12) {
    Serial.println("OK - SX127x responding. Wiring and SPI are good.");
  } else if (version == 0x00 || version == 0xFF) {
    Serial.println("NO RESPONSE - check power, ground, and the SPI lines.");
  } else {
    Serial.println("Unexpected value - may not be an SX127x.");
  }
}

void loop() {}
