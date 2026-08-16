/*
 * Diagnostic 3 - single-board radio test
 *
 * Confirms RadioLib can initialize the chip and key up without errors.
 * Does NOT prove range or that anything can decode the packets - a
 * second node is required for that.
 *
 * ATTACH THE ANTENNA BEFORE RUNNING THIS. Transmitting without one can
 * reflect RF back into the power amplifier and damage the chip.
 */

#include <RadioLib.h>

#define PIN_LORA_NSS  5
#define PIN_LORA_DIO0 26
#define PIN_LORA_RST  14

#define LORA_FREQ 915.0   // match your module's band and your region

SX1276 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO0, PIN_LORA_RST, RADIOLIB_NC);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Initializing radio...");
  int state = radio.begin(LORA_FREQ);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("FAILED, code ");
    Serial.println(state);
    Serial.println("-2 = chip not found. Run 02_lora_chip_id first:");
    Serial.println("  0x12 there means wiring is fine, try the SX1278 class here.");
    while (true) { delay(10); }
  }

  Serial.println("SUCCESS - radio initialized.");

  for (int i = 0; i < 5; i++) {
    Serial.print("Sending test packet #");
    Serial.print(i);
    Serial.print("... ");
    int tx = radio.transmit("SOLO_TEST");
    if (tx == RADIOLIB_ERR_NONE) {
      Serial.println("OK");
    } else {
      Serial.print("failed, code ");
      Serial.println(tx);
    }
    delay(1000);
  }

  Serial.println("Solo test done.");
}

void loop() {}
