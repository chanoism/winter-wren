/*
 * ESP32 WROOM-32 -- Real Reticulum (via microReticulum) over an
 * SX1276 LoRa module at 433MHz + Flock Safety WiFi/BLE detection,
 * with SSD1306 OLED status screen and buzzer alert.
 *
 * ---------------------------------------------------------------
 * STATUS: the Reticulum section below is now checked against the
 * actual source of microReticulum's examples/lora_announce/src/main.cpp
 * (pulled directly from the repo), not guessed. Specific corrections
 * from the previous draft:
 *   - Reticulum::start() and Reticulum::loop() are INSTANCE methods
 *     on an RNS::Reticulum object, not static calls.
 *   - There's a ready-made LoRaInterface class (RadioLib-backed)
 *     rather than a hand-rolled Interface subclass -- vendored into
 *     lib/lora_interface/ with a new board branch for your exact
 *     wiring (BOARD_CUSTOM_WROOM_SX1276, defined in platformio.ini).
 *   - RNS::Identity/Destination/Interface/Reticulum are lightweight
 *     handle objects (the library's own "implicit object sharing"
 *     pattern) -- default-construct with {RNS::Type::NONE} then
 *     assign the real object, matching the reference example.
 *
 * Still worth a sanity check on first flash: the announce/packet
 * flow (destination direction, app_data encoding) is adapted from
 * the reference example for this project's use case (auto-announce
 * on a timer since you have no buttons, plus an announce on a
 * confirmed Flock hit) rather than copied verbatim, so double check
 * behavior against the Reticulum docs if announces aren't showing
 * up on another node.
 * ---------------------------------------------------------------
 */

#include <WiFi.h>
#include "esp_wifi.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <microReticulum.h>
#include <LoRaInterface.h>

// ============================================================
// ---- OLED CONFIG (SSD1306, I2C -- confirmed by working prior sketch) ----
// ============================================================
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C
// WROOM-32 default I2C pins (Wire.begin() with no args uses
// SDA=21, SCL=22 on a stock ESP32 -- matches your wiring diagram).
// Confirmed SSD1306 128x64 -- matches your working prior sketch
// exactly. Earlier garbling was from wrong driver + wrong resolution
// guesses, not a real hardware/wiring problem (I2C scan confirmed
// the device at 0x3C is fine).
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ============================================================
// ---- BUZZER ----
// ============================================================
#define PIN_BUZZER 27

void buzzerBeep(uint16_t durationMs = 150)
{
  // Simple active-buzzer style beep. If yours is passive (needs a
  // tone/PWM signal rather than just HIGH/LOW), swap this for
  // ledcWriteTone()/tone() at your buzzer's rated frequency instead.
  digitalWrite(PIN_BUZZER, HIGH);
  delay(durationMs);
  digitalWrite(PIN_BUZZER, LOW);
}

// Note: the SX1276 pin map (NSS5/SCK18/MOSI23/MISO19/RST14/DIO0 26)
// now lives in lib/lora_interface/LoRaInterface.cpp under the
// BOARD_CUSTOM_WROOM_SX1276 branch, not here -- LoRaInterface owns
// its own SPI.begin() and RadioLib setup internally.

// ============================================================
// ---- FLOCK SAFETY DETECTION (ported from esp32_recon.ino) ----
// ============================================================
#define CHANNEL_HOP_INTERVAL_MS 300
#define DEDUP_WINDOW_MS 30000
#define MAX_DEDUP_ENTRIES 256

#define NUM_FLOCK_OUIS 31
const uint8_t FLOCK_OUIS[NUM_FLOCK_OUIS][3] = {
    {0x70, 0xC9, 0x4E}, {0x3C, 0x91, 0x80}, {0xD8, 0xF3, 0xBC}, {0x80, 0x30, 0x49}, {0xB8, 0x35, 0x32}, {0x14, 0x5A, 0xFC}, {0x74, 0x4C, 0xA1}, {0x08, 0x3A, 0x88}, {0x9C, 0x2F, 0x9D}, {0xC0, 0x35, 0x32}, {0x94, 0x08, 0x53}, {0xE4, 0xAA, 0xEA}, {0xF4, 0x6A, 0xDD}, {0xF8, 0xA2, 0xD6}, {0x24, 0xB2, 0xB9}, {0x00, 0xF4, 0x8D}, {0xD0, 0x39, 0x57}, {0xE8, 0xD0, 0xFC}, {0xE0, 0x4F, 0x43}, {0xB8, 0x1E, 0xA4}, {0x70, 0x08, 0x94}, {0x58, 0x8E, 0x81}, {0xEC, 0x1B, 0xBD}, {0x3C, 0x71, 0xBF}, {0x58, 0x00, 0xE3}, {0x90, 0x35, 0xEA}, {0x5C, 0x93, 0xA2}, {0x64, 0x6E, 0x69}, {0x48, 0x27, 0xEA}, {0xA4, 0xCF, 0x12}, {0x82, 0x6B, 0xF2}};

bool matchesFlockOUI(const uint8_t *mac)
{
  for (int i = 0; i < NUM_FLOCK_OUIS; i++)
  {
    if (mac[0] == FLOCK_OUIS[i][0] && mac[1] == FLOCK_OUIS[i][1] && mac[2] == FLOCK_OUIS[i][2])
    {
      return true;
    }
  }
  return false;
}
bool isMulticast(const uint8_t *mac) { return (mac[0] & 0x01) != 0; }
bool isLocallyAdministered(const uint8_t *mac) { return (mac[0] & 0x02) != 0; }

struct SeenEntry
{
  char mac[18];
  unsigned long lastSeen;
  bool used;
};
SeenEntry dedupCache[MAX_DEDUP_ENTRIES];

bool shouldEmit(const char *mac)
{
  unsigned long now = millis();
  for (int i = 0; i < MAX_DEDUP_ENTRIES; i++)
  {
    if (dedupCache[i].used && strcmp(dedupCache[i].mac, mac) == 0)
    {
      if (now - dedupCache[i].lastSeen < DEDUP_WINDOW_MS)
        return false;
      dedupCache[i].lastSeen = now;
      return true;
    }
  }
  int oldestIdx = 0;
  unsigned long oldestTime = ULONG_MAX;
  for (int i = 0; i < MAX_DEDUP_ENTRIES; i++)
  {
    if (!dedupCache[i].used)
    {
      strncpy(dedupCache[i].mac, mac, sizeof(dedupCache[i].mac));
      dedupCache[i].lastSeen = now;
      dedupCache[i].used = true;
      return true;
    }
    if (dedupCache[i].lastSeen < oldestTime)
    {
      oldestTime = dedupCache[i].lastSeen;
      oldestIdx = i;
    }
  }
  strncpy(dedupCache[oldestIdx].mac, mac, sizeof(dedupCache[oldestIdx].mac));
  dedupCache[oldestIdx].lastSeen = now;
  dedupCache[oldestIdx].used = true;
  return true;
}

// ---- shared "last detection" state, read by the OLED screen ----
volatile uint32_t flockHitCount = 0;
char lastFlockMac[18] = "--:--:--:--:--:--";
char lastFlockType[24] = "none yet";
unsigned long lastFlockMillis = 0;

uint8_t currentChannel = 1;
unsigned long lastHop = 0;

typedef struct
{
  uint8_t frame_ctrl[2];
  uint8_t duration[2];
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  uint8_t seq_ctrl[2];
  uint8_t payload[];
} wifi_mgmt_hdr_t;

void macToStr(const uint8_t *mac, char *out)
{
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void extractSSID(const uint8_t *payload, int payloadLen, char *ssidOut, int ssidMax)
{
  ssidOut[0] = '\0';
  int i = 0;
  while (i + 2 <= payloadLen)
  {
    uint8_t tagNum = payload[i];
    uint8_t tagLen = payload[i + 1];
    if (i + 2 + tagLen > payloadLen)
      break;
    if (tagNum == 0)
    {
      int len = tagLen < ssidMax - 1 ? tagLen : ssidMax - 1;
      memcpy(ssidOut, &payload[i + 2], len);
      ssidOut[len] = '\0';
      return;
    }
    i += 2 + tagLen;
  }
  strcpy(ssidOut, "<hidden>");
}

// Forward-declared -- defined after the Reticulum section below,
// since it announces a detection over RNS.
void onFlockDetection(const char *macStr, const char *label);

void wifiSnifferCallback(void *buf, wifi_promiscuous_pkt_type_t type)
{
  if (type != WIFI_PKT_MGMT)
    return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  uint8_t *payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;

  uint16_t frameControl = (payload[1] << 8) | payload[0];
  uint8_t subtype = (frameControl & 0x00F0) >> 4;
  if (subtype != 8 && subtype != 4 && subtype != 5)
    return;
  if (len < 36)
    return;

  wifi_mgmt_hdr_t *hdr = (wifi_mgmt_hdr_t *)payload;

  bool flockHit = false;
  const char *flockMatchSide = "";
  if (!isMulticast(hdr->addr2) && matchesFlockOUI(hdr->addr2))
  {
    flockHit = true;
    flockMatchSide = "addr2";
  }
  else if (!isMulticast(hdr->addr1) && !isLocallyAdministered(hdr->addr1) && matchesFlockOUI(hdr->addr1))
  {
    flockHit = true;
    flockMatchSide = "addr1";
  }

  char macStr[18];
  macToStr(hdr->addr2, macStr);
  if (!shouldEmit(macStr))
    return;

  char ssid[33];
  int fixedFieldsLen = (subtype == 4) ? 0 : 12;
  int taggedStart = 24 + fixedFieldsLen;
  extractSSID(payload + taggedStart, len - taggedStart, ssid, sizeof(ssid));

  bool wildcardProbeHit = flockHit && subtype == 4 && strcmp(ssid, "<hidden>") == 0;

  if (wildcardProbeHit)
  {
    onFlockDetection(macStr, "WILDCARD_PROBE");
  }
  else if (flockHit)
  {
    onFlockDetection(macStr, flockMatchSide);
  }
}

void wifiChannelHop()
{
  unsigned long now = millis();
  if (now - lastHop >= CHANNEL_HOP_INTERVAL_MS)
  {
    currentChannel = (currentChannel % 13) + 1;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    lastHop = now;
  }
}

BLEScan *pBLEScan;
class BLEAdvCallback : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice) override
  {
    char macStr[18];
    strncpy(macStr, advertisedDevice.getAddress().toString().c_str(), sizeof(macStr));
    for (int i = 0; macStr[i]; i++)
      macStr[i] = toupper(macStr[i]);
    if (!shouldEmit(macStr))
      return;
    // BLE-side Flock/Raven heuristics intentionally left out of this
    // pass -- your WiFi OUI match is already field-tested; BLE name/
    // manufacturer-ID matching is a good next addition once this
    // combined build is confirmed working end-to-end.
  }
};

// ============================================================
// ---- RETICULUM SETUP ----
// Matches the shape of microReticulum's own
// examples/lora_announce/src/main.cpp (fetched directly from the
// repo). See reticulum_setup()/reticulum_loop() below.
// ============================================================
const char *APP_NAME = "flockwatch";

RNS::Reticulum reticulum({RNS::Type::NONE});
RNS::Interface lora_interface({RNS::Type::NONE});
// ---------------------------------------------------------------
// Fixed node identity -- the key itself lives in node_identity.h,
// which is gitignored. Copy node_identity.example.h to
// node_identity.h and generate a key per board. See that file.
// ---------------------------------------------------------------
#include "node_identity.h"

RNS::Identity nodeIdentity({RNS::Type::NONE});
RNS::Destination announceDestination({RNS::Type::NONE});
bool rnsReady = false;

// Logs any announce heard from another real Reticulum node on the mesh.
// ---- shared "last received" state, read by the OLED screen ----
char lastReceivedText[48] = "(none yet)";
unsigned long lastReceivedMillis = 0;

class FlockwatchAnnounceHandler : public RNS::AnnounceHandler
{
public:
  FlockwatchAnnounceHandler(const char *aspect_filter = nullptr) : AnnounceHandler(aspect_filter) {}
  virtual ~FlockwatchAnnounceHandler() {}
  virtual void received_announce(const RNS::Bytes &destination_hash, const RNS::Identity &announced_identity, const RNS::Bytes &app_data)
  {
    Serial.printf("[rns] announce from %s\n", destination_hash.toHex().c_str());
    if (app_data)
    {
      std::string text = app_data.toString();
      Serial.printf("[rns] app_data: %s\n", text.c_str());
      strncpy(lastReceivedText, text.c_str(), sizeof(lastReceivedText) - 1);
      lastReceivedText[sizeof(lastReceivedText) - 1] = '\0';
      lastReceivedMillis = millis();
    }
    else
    {
      Serial.println("[rns] (announce had no app_data)");
    }
  }
};
RNS::HAnnounceHandler announce_handler(new FlockwatchAnnounceHandler());

// Logs any data packet addressed directly to our destination.
void onPacket(const RNS::Bytes &data, const RNS::Packet &packet)
{
  Serial.printf("[rns] packet received: %s\n", data.toString().c_str());
}

void reticulum_setup()
{
  Serial.println("[rns] setting up Reticulum...");

  try
  {
    // Filesystem is required -- microReticulum throws "FileSystem
    // has not been registered" at start() if this isn't set up,
    // regardless of the RNS_USE_FS build flag. The path-table/
    // identity-store write errors we saw earlier are a separate,
    // apparently non-fatal issue -- reverted to keep this registered
    // since removing it broke setup entirely.
    microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
    filesystem.init();
    RNS::Utilities::OS::register_filesystem(filesystem);

    // LoRa interface (SX1276, 433MHz, your wiring -- see
    // lib/lora_interface/LoRaInterface.cpp).
    lora_interface = new LoRaInterface();
    lora_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(lora_interface);
    lora_interface.start();

    // Bring the Reticulum stack up.
    reticulum = RNS::Reticulum();
    reticulum.transport_enabled(false);
    reticulum.probe_destination_enabled(true);
    reticulum.start();

    // Fixed identity -- loads the hardcoded private key above
    // instead of generating a new random one each boot, so this
    // node's destination hash stays stable across reboots/reflashes.
    nodeIdentity = RNS::Identity(false);
    RNS::Bytes nodePrivateKey;
    nodePrivateKey.assignHex(NODE_PRIVATE_KEY_HEX);
    nodeIdentity.load_private_key(nodePrivateKey);

    // IN destination: this node's own detector, announced to the
    // mesh so other real Reticulum nodes can see and reach it.
    announceDestination = RNS::Destination(
        nodeIdentity, RNS::Type::Destination::IN, RNS::Type::Destination::SINGLE,
        APP_NAME, "detection");
    announceDestination.set_packet_callback(onPacket);
    announceDestination.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

    RNS::Transport::register_announce_handler(announce_handler);

    rnsReady = true;
    Serial.println("[rns] Reticulum stack started");
  }
  catch (const std::exception &e)
  {
    Serial.printf("[rns] setup FAILED: %s\n", e.what());
  }
}

void reticulum_loop()
{
  if (!rnsReady)
    return;
  reticulum.loop();
}

// Periodic announce so the node stays visible on the mesh even with
// no buttons to trigger one manually.
unsigned long lastAnnounceMillis = 0;
#define ANNOUNCE_INTERVAL_MS 60000

void maybeAnnounce()
{
  if (!rnsReady)
    return;
  if (millis() - lastAnnounceMillis < ANNOUNCE_INTERVAL_MS)
    return;
  RNS::Bytes payload = RNS::bytesFromString("online");
  Serial.printf("[tx] periodic payload size: %lu bytes\n", (unsigned long)payload.size());
  announceDestination.announce(payload);
  lastAnnounceMillis = millis();
  Serial.println("[rns] periodic announce sent");
}

// ---- Serial terminal for range testing ----
// Type a line in the serial monitor and hit Enter -- it goes out
// over LoRa as an announce carrying your text in app_data. The
// receiving node's FlockwatchAnnounceHandler (already in this same
// file) prints it the moment it arrives. This is a broadcast, not
// addressed to a specific peer, so no pairing/path discovery is
// needed -- ideal for "type on node A, watch node B's serial, walk
// away and see how far it still gets through" range testing.
String serialLineBuffer = "";

void checkSerialInput()
{
  while (Serial.available())
  {
    char c = Serial.read();
    if (c == '\n' || c == '\r')
    {
      if (serialLineBuffer.length() > 0)
      {
        if (rnsReady)
        {
          RNS::Bytes payload = RNS::bytesFromString(serialLineBuffer.c_str());
          Serial.printf("[tx] payload size before send: %lu bytes\n", (unsigned long)payload.size());
          announceDestination.announce(payload);
          Serial.printf("[tx] sent: %s\n", serialLineBuffer.c_str());
          lastAnnounceMillis = millis(); // don't let the periodic announce fire right after
        }
        else
        {
          Serial.println("[tx] RNS not ready yet -- message dropped");
        }
        serialLineBuffer = "";
      }
    }
    else
    {
      serialLineBuffer += c;
    }
  }
}

void onFlockDetection(const char *macStr, const char *label)
{
  flockHitCount++;
  strncpy(lastFlockMac, macStr, sizeof(lastFlockMac));
  strncpy(lastFlockType, label, sizeof(lastFlockType));
  lastFlockMillis = millis();

  Serial.printf("[flock] hit %s (%s)\n", macStr, label);
  buzzerBeep();

  // Announce immediately on a confirmed hit, with the detection
  // encoded in app_data, so any paired node on the mesh sees it as
  // soon as it happens rather than waiting for the periodic announce.
  if (rnsReady)
  {
    char appData[48];
    snprintf(appData, sizeof(appData), "FLOCK %s %s", macStr, label);
    announceDestination.announce(RNS::bytesFromString(appData));
    lastAnnounceMillis = millis();
  }
}

// ============================================================
// ---- OLED: rolling 2-line status, no buttons ----
// Refreshes every second; rotates between screens every 3 seconds.
// ============================================================
unsigned long lastDisplayUpdate = 0;
#define DISPLAY_INTERVAL_MS 1000
#define ROTATE_INTERVAL_MS 3000
#define NUM_SCREENS 4

void updateDisplay()
{
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  int screen = (millis() / ROTATE_INTERVAL_MS) % NUM_SCREENS;

  switch (screen)
  {
  case 0:
    display.println(rnsReady ? "RNS: up" : "RNS: init");
    display.println("433 SX1276");
    break;
  case 1:
    display.print("ch: ");
    display.println(currentChannel);
    display.print("hits: ");
    display.println(flockHitCount);
    break;
  case 2:
    if (flockHitCount > 0)
    {
      unsigned long secsAgo = (millis() - lastFlockMillis) / 1000;
      display.println(lastFlockType);
      display.print(secsAgo);
      display.println("s ago");
    }
    else
    {
      display.println("no");
      display.println("detections");
    }
    break;
  case 3:
  default:
    // Just the message -- no header/label, no timer, per request.
    display.setTextSize(2);
    display.println(lastReceivedText);
    break;
  }

  display.display();
}

// ============================================================
// ---- SETUP / LOOP ----
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(500);
  memset(dedupCache, 0, sizeof(dedupCache));

  RNS::loglevel(RNS::LOG_INFO);

  // Buzzer
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    Serial.println("[oled] SSD1306 init failed -- check I2C wiring/address");
  }
  else
  {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("booting...");
    display.display();
  }

  // WiFi promiscuous mode -- same reliable STA-mode trick as your
  // working esp32_recon.ino (WIFI_MODE_NULL silently fails on some
  // core versions, so we avoid it).
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

  // BLE
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new BLEAdvCallback(), true);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  pBLEScan->start(0, nullptr, false);

  // Reticulum + SX1276
  reticulum_setup();

  Serial.println("# combined RNS+flock firmware online");
}

void loop()
{
  wifiChannelHop();
  reticulum_loop();
  maybeAnnounce();
  checkSerialInput();

  static unsigned long lastBleRestart = 0;
  if (millis() - lastBleRestart > 10000)
  {
    pBLEScan->clearResults();
    lastBleRestart = millis();
  }

  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL_MS)
  {
    updateDisplay();
    lastDisplayUpdate = millis();
  }
}
