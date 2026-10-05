// SPDX-License-Identifier: Apache-2.0
// LoRa ping-pong / beacon link test: Seeed XIAO ESP32S3 + Wio-SX1262 KIT.
//
// Flash the same sketch to two boards. Each one sends a beacon every ~4 s
// (with random jitter so they don't collide) and listens the rest of the
// time. Each received packet prints the sender ID, RSSI and SNR.
//
// Pins are for the KIT (B2B connector), NOT the separate header board.
#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>

#define LORA_SCK   7
#define LORA_MISO  8
#define LORA_MOSI  9
#define LORA_NSS   41
#define LORA_DIO1  39
#define LORA_RST   42
#define LORA_BUSY  40

// 915 MHz US band. SF9 / CR 4/7 / 14 dBm is the setting the original
// 750 ft baseline was measured with. The main firmware uses SF8 / CR 4/5 / 17 dBm.
#define LORA_FREQ_MHZ 915.0
#define LORA_BW_KHZ   125.0
#define LORA_SF       9
#define LORA_CR       7
#define LORA_SYNC     0x12
#define LORA_POWER    14
#define LORA_PREAMBLE 8
#define LORA_TCXO_V   3.0

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool gotPacket = false;
uint16_t nodeId = 0;
uint32_t counter = 0;
uint32_t nextTxMs = 0;

#if defined(ESP8266) || defined(ESP32)
ICACHE_RAM_ATTR
#endif
void onDio1() { gotPacket = true; }

static void explainState(int16_t s) {
  switch (s) {
    case RADIOLIB_ERR_NONE:               Serial.println(F("OK")); break;
    case RADIOLIB_ERR_CHIP_NOT_FOUND:     Serial.println(F("chip not found: check pins / B2B seating")); break;
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT:    Serial.println(F("SPI command timeout: BUSY pin wrong or module unpowered")); break;
    case RADIOLIB_ERR_SPI_CMD_FAILED:     Serial.println(F("SPI command failed")); break;
    case RADIOLIB_ERR_INVALID_FREQUENCY:  Serial.println(F("invalid frequency")); break;
    case RADIOLIB_ERR_INVALID_OUTPUT_POWER: Serial.println(F("invalid output power")); break;
    case RADIOLIB_ERR_INVALID_SPREADING_FACTOR: Serial.println(F("invalid spreading factor")); break;
    case RADIOLIB_ERR_INVALID_CODING_RATE: Serial.println(F("invalid coding rate")); break;
    default: Serial.print(F("error code ")); Serial.println(s); break;
  }
}

static void scheduleNextTx() { nextTxMs = millis() + 4000 + (uint32_t)random(0, 1500); }

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);  // native USB: wait for the host
  delay(300);

  // Use bits 47..32 of the MAC. The low 16 bits are identical across boards
  // from the same batch (both boards once printed 4F7C).
  nodeId = (uint16_t)(ESP.getEfuseMac() >> 32);
  randomSeed((uint32_t)ESP.getEfuseMac());
  Serial.printf("\n=== LoRa ping-pong | node %04X ===\n", nodeId);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  Serial.print(F("radio.begin ... "));
  int16_t st = radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_SYNC,
                           LORA_POWER, LORA_PREAMBLE, LORA_TCXO_V, false /* LDO, not DC-DC */);
  explainState(st);
  if (st != RADIOLIB_ERR_NONE) { while (true) delay(1000); }

  // The Wio-SX1262 drives its antenna switch from DIO2.
  radio.setDio2AsRfSwitch(true);
  radio.setDio1Action(onDio1);

  st = radio.startReceive();
  Serial.print(F("startReceive ... "));
  explainState(st);
  scheduleNextTx();
}

void loop() {
  if (gotPacket) {
    gotPacket = false;
    String msg;
    int16_t st = radio.readData(msg);
    if (st == RADIOLIB_ERR_NONE) {
      Serial.printf("[RX] \"%s\"  RSSI %.1f dBm  SNR %.1f dB\n",
                    msg.c_str(), radio.getRSSI(), radio.getSNR());
    } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println(F("[RX] CRC error"));
    } else {
      Serial.print(F("[RX] read failed: ")); explainState(st);
    }
    radio.startReceive();
  }

  if ((int32_t)(millis() - nextTxMs) >= 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), "node %04X #%lu", nodeId, (unsigned long)counter++);
    int16_t st = radio.transmit(buf);
    Serial.printf("[TX] %s -> ", buf);
    explainState(st);

    // DIO1 fires on TxDone as well as RxDone. Without clearing the flag here the
    // sketch reports its own transmission as a received packet (empty payload,
    // RSSI/SNR identical to the previous packet).
    gotPacket = false;
    radio.startReceive();
    scheduleNextTx();
  }
}
