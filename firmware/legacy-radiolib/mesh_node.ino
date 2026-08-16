/*
 * Community Mesh Alert Network - legacy RadioLib node
 *
 * Custom flood-relay mesh over LoRa. Superseded by the microReticulum
 * firmware in ../reticulum-node, retained as a known-good reference for
 * the radio wiring and as a fallback.
 *
 * WARNING: transmits in plaintext with no authentication. Anyone in range
 * can read traffic and inject forged alerts. Do not deploy.
 *
 * Board: ESP32-WROOM-32 (Arduino IDE)
 * Libraries: RadioLib (jgromes), Adafruit SSD1306, Adafruit GFX
 *
 * Packet format:  <packet_id>:<hop_count>:<message>
 */

#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---- pins ----
#define PIN_LORA_NSS   5
#define PIN_LORA_DIO0  26
#define PIN_LORA_RST   14
#define PIN_BUZZER     27
#define PIN_BUTTON     25
#define PIN_I2C_SDA    21
#define PIN_I2C_SCL    22

// ---- radio ----
// NOTE: modules sold as SX1278/RA-02 are frequently SX1276 silicon.
// Both report 0x12 in RegVersion. If begin() returns -2 despite correct
// wiring, swap this class. Verify per unit.
SX1276 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO0, PIN_LORA_RST, RADIOLIB_NC);

// Set to match your module's band AND your region.
// US ISM = 915.0, EU = 868.0, Region 1 low band = 433.0
#define LORA_FREQ 915.0

// ---- display ----
// NOTE: resolution differs between firmware tracks. Confirm your panel.
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---- interrupt flag ----
// Named rxInterruptEnabled, not enableInterrupt: ESP32 core 3.x defines
// its own enableInterrupt(uint8_t) and the names collide.
volatile bool receivedFlag = false;
volatile bool rxInterruptEnabled = true;

void setFlag(void) {
  if (!rxInterruptEnabled) return;
  receivedFlag = true;
}

// ---- dedup cache ----
#define SEEN_ID_CACHE_SIZE 20
uint32_t seenIds[SEEN_ID_CACHE_SIZE];
int seenIdIndex = 0;

bool alreadySeen(uint32_t id) {
  for (int i = 0; i < SEEN_ID_CACHE_SIZE; i++) {
    if (seenIds[i] == id) return true;
  }
  return false;
}

void markSeen(uint32_t id) {
  seenIds[seenIdIndex] = id;
  seenIdIndex = (seenIdIndex + 1) % SEEN_ID_CACHE_SIZE;
}

// ---- config ----
#define HOP_LIMIT          5
#define ALERT_DISPLAY_MS   5000
#define BUTTON_COOLDOWN_MS 30000

// ---- state ----
unsigned long alertShownAt = 0;
bool showingAlert = false;
unsigned long lastButtonSend = 0;
bool lastButtonState = HIGH;

void showStatus(String line1, String line2) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.display();
}

void alertBeep() {
  for (int i = 0; i < 3; i++) {
    tone(PIN_BUZZER, 2500, 150);
    delay(200);
  }
}

void sendReport(const char *message) {
  uint32_t newId = random(1, 2147483647);
  String packet = String(newId) + ":" + String(HOP_LIMIT) + ":" + String(message);

  markSeen(newId);   // don't beep at our own packet if it echoes back

  rxInterruptEnabled = false;
  showStatus("Sending", "report...");
  int state = radio.transmit(packet);
  radio.startReceive();
  rxInterruptEnabled = true;

  Serial.print("TX id=");
  Serial.print(newId);
  Serial.println(state == RADIOLIB_ERR_NONE ? " ok" : " FAILED");

  delay(1000);
  if (!showingAlert) showStatus("Node ready.", "Listening...");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  randomSeed(esp_random());

  for (int i = 0; i < SEEN_ID_CACHE_SIZE; i++) seenIds[i] = 0;

  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
    while (true) { delay(10); }
  }

  showStatus("Initializing", "radio...");

  int state = radio.begin(LORA_FREQ);
  if (state != RADIOLIB_ERR_NONE) {
    showStatus("RADIO INIT", "FAILED");
    Serial.print("radio.begin failed, code ");
    Serial.println(state);
    Serial.println("-2 = chip not found. Check wiring, or try the SX1278 class.");
    while (true) { delay(10); }
  }

  radio.setDio0Action(setFlag, RISING);
  radio.startReceive();

  showStatus("Node ready.", "Listening...");
  Serial.println("Node ready.");
}

void loop() {
  // revert the display after an alert has been up long enough
  if (showingAlert && millis() - alertShownAt > ALERT_DISPLAY_MS) {
    showStatus("Node ready.", "Listening...");
    showingAlert = false;
  }

  // ---- receive ----
  if (receivedFlag) {
    rxInterruptEnabled = false;
    receivedFlag = false;

    String str;
    int state = radio.readData(str);

    if (state == RADIOLIB_ERR_NONE) {
      int firstColon  = str.indexOf(':');
      int secondColon = str.indexOf(':', firstColon + 1);

      if (firstColon > 0 && secondColon > firstColon) {
        uint32_t packetId = str.substring(0, firstColon).toInt();
        int      hopCount = str.substring(firstColon + 1, secondColon).toInt();
        String   message  = str.substring(secondColon + 1);

        if (!alreadySeen(packetId)) {
          markSeen(packetId);

          Serial.print("RX new id=");
          Serial.print(packetId);
          Serial.print(" rssi=");
          Serial.print(radio.getRSSI());
          Serial.print(" snr=");
          Serial.print(radio.getSNR());
          Serial.print(" msg=");
          Serial.println(message);

          alertBeep();
          showStatus("ALERT:", message);
          alertShownAt = millis();
          showingAlert = true;

          // relay onward if hop budget remains.
          // random delay so nodes that heard the same packet don't collide.
          if (hopCount > 0) {
            delay(random(100, 500));
            radio.transmit(String(packetId) + ":" + String(hopCount - 1) + ":" + message);
          }
        } else {
          Serial.println("RX duplicate, ignoring.");
        }
      }
    }

    radio.startReceive();
    rxInterruptEnabled = true;
  }

  // ---- button: one press, one alert, then a cooldown lockout ----
  bool buttonState = digitalRead(PIN_BUTTON);
  if (buttonState == LOW && lastButtonState == HIGH) {
    delay(50);                                   // debounce
    if (digitalRead(PIN_BUTTON) == LOW) {
      if (millis() - lastButtonSend > BUTTON_COOLDOWN_MS || lastButtonSend == 0) {
        lastButtonSend = millis();
        sendReport("ALERT");
      } else {
        Serial.println("Button pressed but still in cooldown.");
      }
    }
  }
  lastButtonState = buttonState;
}
