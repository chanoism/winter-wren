/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portions derived from colonelpanichacks/flock-you (MIT, Copyright (c) 2026
 * colonelpanichacks) and the OUI list from nitekry/nite-oui-collection (MIT,
 * Copyright (c) 2026 nitekry). See THIRD_PARTY_NOTICES.md for the notices.
 *
 * Seeed XIAO ESP32S3 + Wio-SX1262 kit -- Real Reticulum (via
 * microReticulum) over SX1262 LoRa at 915MHz + Flock Safety WiFi/BLE
 * detection, with SSD1306 OLED status screen and buzzer alert.
 *
 * ---------------------------------------------------------------
 * PORTED from the ESP32 WROOM-32 + hand-wired SX1276 @ 433MHz build.
 * Hardware changes, all verified on the bench before this port:
 *   - LoRa moved to the B2B connector (NSS41/DIO1 39/RST42/BUSY40,
 *     SPI 7/8/9). Pin map confirmed by a standalone RadioLib link
 *     test: two boards, 750 ft, clean packets, no CRC errors.
 *   - I2C moved from 21/22 to D5/D7 (GPIO6/44), since D4 is taken
 *     by the LoRa module. Confirmed by I2C scan, OLED found at 0x3C.
 *   - Buzzer moved from GPIO27 to D6/GPIO43 (not yet populated).
 *
 * UNVERIFIED on this hardware: WiFi promiscuous mode + BLE + LoRa
 * running simultaneously on an ESP32-S3. It worked on the WROOM-32,
 * but this combination hasn't been proven on the S3. If something
 * misbehaves on first boot, that's the first place to look.
 * ---------------------------------------------------------------
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
 *     wiring (BOARD_XIAO_S3_SX1262, defined in platformio.ini).
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
#include <Adafruit_seesaw.h>

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

// I2C pins for the XIAO ESP32S3 + Wio-SX1262 kit.
//
// These are NOT the XIAO's default I2C pins. The default bus is D4/D5,
// but D4 is consumed by the Wio-SX1262's B2B connection, so the bus is
// moved to D5/D7. Wire.begin() with NO ARGUMENTS will not work here
//
// Confirmed working on this hardware by I2C scan (device found at 0x3C,
// genuine SSD1306, no SH1106 substitution this time).
#define PIN_I2C_SDA 6  // D5
#define PIN_I2C_SCL 44 // D7

// ---- Adafruit Seesaw rotary encoder (product 4991 / 5880) ----
//
// Shares the same two I2C wires as the OLED -- costs ZERO extra GPIOs,
// which is the entire reason this part was chosen. The Seesaw does the
// quadrature decoding onboard, so there is no interrupt handling or
// debounce logic here.
//
// Default address 0x36. No collision with the OLED at 0x3C.
#define SEESAW_ADDR 0x36
#define SEESAW_SWITCH_PIN 24 // the push-switch, per Adafruit's pinout
Adafruit_seesaw encoder;
bool encoderPresent = false;
int32_t encoderLastPos = 0;
bool encoderBtnWasDown = false;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ============================================================
// ---- BUZZER ----
// ============================================================
// D6 / GPIO43 — the only pin left after LoRa (B2B) and I2C (D5/D7).
//
// Note: GPIO43 is U0TXD, so the ROM bootloader emits its startup log on
// this line at every power-on. On an active buzzer that's a brief click
// at boot. Harmless. It's on the buzzer rather than on SCL deliberately —
// boot chatter on an I2C clock line would be the worse of the two.
//
// NOT YET POPULATED: buzzer hardware isn't attached yet. The pin is
// driven anyway, which is harmless with nothing connected.
#define PIN_BUZZER 43

// ---- Non-blocking buzzer ----
//
// CHANGED: this used to be `digitalWrite(HIGH); delay(durationMs);
// digitalWrite(LOW);`. That delay() was called from onFlockDetection(),
// which ran inside the WiFi promiscuous callback -- so every detection
// stalled the WiFi driver task for 150ms AND stalled the LoRa receive
// polling in loop(). Now it just arms a timestamp; buzzerService() in
// loop() turns it off.
unsigned long buzzerOffAt = 0;

void buzzerBeep(uint16_t durationMs = 150)
{
  digitalWrite(PIN_BUZZER, HIGH);
  buzzerOffAt = millis() + durationMs;
}

void buzzerService()
{
  if (buzzerOffAt != 0 && (long)(millis() - buzzerOffAt) >= 0)
  {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerOffAt = 0;
  }
}

// Note: the SX1262 pin map (NSS41/DIO1 39/RST42/BUSY40, SPI 7/8/9)
// now lives in lib/lora_interface/LoRaInterface.cpp under the
// BOARD_XIAO_S3_SX1262 branch, not here -- LoRaInterface owns
// its own SPI.begin() and RadioLib setup internally.

// ============================================================
// ---- FLOCK SAFETY DETECTION (ported from esp32_recon.ino) ----
// ============================================================
// ---- Channel hopping ----
//
// CHANGED: was ascending 1..13 at 300ms. Flock cameras sweep channels in
// ASCENDING order emitting wildcard probes roughly every 0.125s. Sweeping
// ascending alongside them means you can travel with their sweep and keep
// missing the probes. colonelpanichacks/flock-you hops DESCENDING across
// 11/6/1 at 250ms specifically to cut across an ascending sweep and
// maximise intercept rate.
//
// 11/6/1 are the non-overlapping 2.4GHz channels -- a camera sweeping
// 1..13 still passes through all three.
#define CHANNEL_HOP_INTERVAL_MS 250
const uint8_t CHANNEL_SEQUENCE[] = {11, 6, 1};
#define NUM_CHANNELS (sizeof(CHANNEL_SEQUENCE) / sizeof(CHANNEL_SEQUENCE[0]))

#define DEDUP_WINDOW_MS 30000
#define MAX_DEDUP_ENTRIES 256

// Drop frames weaker than this. Matches flock-you's RSSI_MIN. Very weak
// frames are more likely to be corrupt, and a corrupted MAC that happens
// to land on a Flock OUI is a false positive.
#define RSSI_MIN -95

// ---- BLE / WiFi radio sharing ----
// Both share one 2.4GHz radio. These are in units of 0.625ms per the BLE
// spec, but the Arduino BLE wrapper takes them as-is, so window/interval
// is what matters: 30/100 = 30% of radio time to BLE, 70% to WiFi.
// If `frames` in the heartbeat is low, lower BLE_SCAN_WINDOW further.
#define BLE_SCAN_INTERVAL 100
#define BLE_SCAN_WINDOW 30

// ---- Flock alerting ----
// Separate from the general dedup cache. A camera probing every 0.125s
// shouldn't beep continuously, but 30s is too long to wait for a tier
// upgrade. 5s matches flock-you's ALERT_COOLDOWN_MS.
#define FLOCK_ALERT_COOLDOWN_MS 5000
#define MAX_FLOCK_ENTRIES 32

// ---- Confidence tiers ----
// Higher = more specific signature. A higher-tier hit on a MAC already
// seen at a lower tier re-alerts immediately, so an upgrade is never
// swallowed by the cooldown.
#define FLOCK_TIER_NONE 0
#define FLOCK_TIER_ADDR1 1    // receiver-side OUI: noisiest. A nearby AP
                              // replying to a Flock probe can land here.
#define FLOCK_TIER_ADDR2 2    // transmitter-side OUI on any frame
#define FLOCK_TIER_PROBE 3    // probe request from a Flock OUI
#define FLOCK_TIER_WILDCARD 4 // probe request + zero-length SSID IE +
                              // Flock OUI on addr2. The DeFlockJoplin
                              // signature: 11/12 cameras in field test,
                              // 2 false positives.
#define FLOCK_TIER_IE_SIG 5   // all of the above PLUS the Information
                              // Element fingerprint matching. Highest
                              // confidence available.

#define NUM_FLOCK_OUIS 33
const uint8_t FLOCK_OUIS[NUM_FLOCK_OUIS][3] = {
    // 30 from @NitekryDPaul's promiscuous-mode research
    {0x70, 0xC9, 0x4E},
    {0x3C, 0x91, 0x80},
    {0xD8, 0xF3, 0xBC},
    {0x80, 0x30, 0x49},
    {0xB8, 0x35, 0x32},
    {0x14, 0x5A, 0xFC},
    {0x74, 0x4C, 0xA1},
    {0x08, 0x3A, 0x88},
    {0x9C, 0x2F, 0x9D},
    {0xC0, 0x35, 0x32},
    {0x94, 0x08, 0x53},
    {0xE4, 0xAA, 0xEA},
    {0xF4, 0x6A, 0xDD},
    {0xF8, 0xA2, 0xD6},
    {0x24, 0xB2, 0xB9},
    {0x00, 0xF4, 0x8D},
    {0xD0, 0x39, 0x57},
    {0xE8, 0xD0, 0xFC},
    {0xE0, 0x4F, 0x43},
    {0xB8, 0x1E, 0xA4},
    {0x70, 0x08, 0x94},
    {0x58, 0x8E, 0x81},
    {0xEC, 0x1B, 0xBD},
    {0x3C, 0x71, 0xBF},
    {0x58, 0x00, 0xE3},
    {0x90, 0x35, 0xEA},
    {0x5C, 0x93, 0xA2},
    {0x64, 0x6E, 0x69},
    {0x48, 0x27, 0xEA},
    {0xA4, 0xCF, 0x12},
    // 31st: Michael / DeFlockJoplin, Joplin MO field testing
    {0x82, 0x6B, 0xF2},
    // ADDED: two more from flock-you issue #28, reported by a user who
    // found them on Flock Falcons in their area and said adding them took
    // him from catching one or two cameras to most of them.
    //
    // CAVEAT: community-reported, not maintainer-verified, and not in the
    // official 31-entry dataset. If you start seeing false positives,
    // these two are the first suspects -- the reporter also mentioned a
    // camera on a Raspberry Pi Foundation MAC that he deliberately left
    // out as too uncertain.
    {0xB4, 0x1E, 0x52},
    {0xE0, 0x0A, 0xF6}};

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

// ---- Flock-specific alert cache ----
// Deliberately separate from dedupCache. In the previous version the
// general dedup check ran BEFORE the Flock decision, which meant a camera
// whose MAC had already been seen on any unrelated frame in the last 30s
// was silently discarded -- including its wildcard probes. That is a real
// miss mechanism, and it gets worse now that data frames are processed
// too. Flock evaluation now happens first, against this cache.
struct FlockEntry
{
  char mac[18];
  unsigned long lastAlert;
  uint8_t bestTier;
  bool used;
};
FlockEntry flockCache[MAX_FLOCK_ENTRIES];

// Returns true if this hit should alert: either the cooldown has expired,
// or this is a higher-confidence tier than we have previously seen for
// this MAC (so an upgrade is never swallowed by the cooldown).
bool shouldAlertFlock(const char *mac, uint8_t tier)
{
  unsigned long now = millis();
  int freeIdx = -1;
  int oldestIdx = 0;
  unsigned long oldestTime = ULONG_MAX;

  for (int i = 0; i < MAX_FLOCK_ENTRIES; i++)
  {
    if (flockCache[i].used && strcmp(flockCache[i].mac, mac) == 0)
    {
      bool cooled = (now - flockCache[i].lastAlert) >= FLOCK_ALERT_COOLDOWN_MS;
      bool upgrade = tier > flockCache[i].bestTier;
      if (tier > flockCache[i].bestTier)
        flockCache[i].bestTier = tier;
      if (cooled || upgrade)
      {
        flockCache[i].lastAlert = now;
        return true;
      }
      return false;
    }
    if (!flockCache[i].used && freeIdx < 0)
      freeIdx = i;
    if (flockCache[i].used && flockCache[i].lastAlert < oldestTime)
    {
      oldestTime = flockCache[i].lastAlert;
      oldestIdx = i;
    }
  }

  int idx = (freeIdx >= 0) ? freeIdx : oldestIdx;
  strncpy(flockCache[idx].mac, mac, sizeof(flockCache[idx].mac) - 1);
  flockCache[idx].mac[sizeof(flockCache[idx].mac) - 1] = '\0';
  flockCache[idx].lastAlert = now;
  flockCache[idx].bestTier = tier;
  flockCache[idx].used = true;
  return true;
}

const char *tierLabel(uint8_t tier)
{
  switch (tier)
  {
  case FLOCK_TIER_IE_SIG:
    return "IE_SIG";
  case FLOCK_TIER_WILDCARD:
    return "WILDCARD_PROBE";
  case FLOCK_TIER_PROBE:
    return "PROBE_OUI";
  case FLOCK_TIER_ADDR2:
    return "OUI_addr2";
  case FLOCK_TIER_ADDR1:
    return "OUI_addr1";
  default:
    return "none";
  }
}

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

uint8_t currentChannel = 11; // first entry of CHANNEL_SEQUENCE (descending)
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

// ============================================================
// ---- INFORMATION ELEMENT FINGERPRINT ----
//
// Ported from colonelpanichacks/flock-you main.cpp. Research credit:
// Michael / DeFlockJoplin for the signature itself; Lucia Pintor & Luigi
// Atzori for the underlying IE-fingerprinting work
// (DOI 10.1109/GLOBECOM48099.2022.10001618).
//
// The idea: a probe request carries a series of Information Elements
// (tag-length-value triples). Which tags a device emits, in what order,
// and the contents of its vendor-specific elements, are characteristic of
// the firmware that built the frame. Flock cameras produce one specific
// arrangement. Combined with an OUI match on the transmitter this is a
// far tighter signature than either signal alone.
//
// The fingerprint is built as a comma-separated string: non-vendor tags
// become their decimal tag number; vendor elements (tag 221) become
// "221:" plus up to 8 payload bytes in hex. The SSID element is skipped
// entirely since it's empty on a wildcard probe.
//
// NOTE ON IRAM: the upstream marks these IRAM_ATTR for callback latency.
// Omitted here -- this firmware carries Reticulum, BLE, RadioLib and the
// display alongside the sniffer, and IRAM is tight. The promiscuous
// callback runs in the WiFi task rather than a true ISR, so executing
// from flash is safe; worst case is slightly more jitter.
// ============================================================

// ============================================================
// ---- DETECTION QUEUE ----
//
// THE FIX for the "stops detecting until reset" bug.
//
// Previously the WiFi promiscuous callback called onFlockDetection()
// directly, which did Serial.printf(), a 150ms blocking delay() for the
// buzzer, and a blocking LoRa transmit for the announce. That is roughly
// a second of work inside a driver callback that is expected to return in
// microseconds, and it can leave the WiFi task wedged -- which matches
// the observed symptom exactly: detections work, then stop, and only a
// reset brings them back.
//
// The callback now does nothing but copy the event into this ring buffer
// and return. loop() drains it and does all the slow work. This is the
// same pattern colonelpanichacks/flock-you uses, and their source is
// explicit that the WiFi task cannot safely call Serial.print or malloc.
// ============================================================

struct FlockEvent
{
  char mac[18];
  uint8_t tier;
  int8_t rssi;
  uint8_t channel;
};

#define FLOCK_QUEUE_SIZE 16
static FlockEvent flockQueue[FLOCK_QUEUE_SIZE];
static volatile uint8_t flockQHead = 0;
static volatile uint8_t flockQTail = 0;
static portMUX_TYPE flockQMux = portMUX_INITIALIZER_UNLOCKED;

// ---- Instrumentation ----
//
// These exist to prove or disprove the wedge theory in the field. The
// heartbeat in loop() prints them every 10s. If `frames` stops climbing
// while the device is still powered and near traffic, the WiFi callback
// has stopped firing -- that is the wedge, and it is not a detection
// logic problem. If `frames` keeps climbing but no hits appear, the
// callback is alive and the issue is in matching.
static volatile uint32_t statFramesSeen = 0;
static volatile uint32_t statMgmtFrames = 0;
static volatile uint32_t statDataFrames = 0;
static volatile uint32_t statQueueDrops = 0;
static volatile uint32_t statFlockRaw = 0;
static uint32_t statHitsProcessed = 0;

// Called from the WiFi callback. Must stay fast and allocation-free.
static void enqueueFlockEvent(const char *mac, uint8_t tier, int8_t rssi, uint8_t channel)
{
  portENTER_CRITICAL_ISR(&flockQMux);
  uint8_t next = (flockQHead + 1) % FLOCK_QUEUE_SIZE;
  if (next == flockQTail)
  {
    // Queue full. Drop rather than block -- a dropped duplicate detection
    // costs nothing, a blocked WiFi task costs everything.
    statQueueDrops++;
    portEXIT_CRITICAL_ISR(&flockQMux);
    return;
  }
  FlockEvent *e = &flockQueue[flockQHead];
  strncpy(e->mac, mac, sizeof(e->mac) - 1);
  e->mac[sizeof(e->mac) - 1] = '\0';
  e->tier = tier;
  e->rssi = rssi;
  e->channel = channel;
  flockQHead = next;
  portEXIT_CRITICAL_ISR(&flockQMux);
}

static bool dequeueFlockEvent(FlockEvent *out)
{
  bool got = false;
  portENTER_CRITICAL(&flockQMux);
  if (flockQTail != flockQHead)
  {
    *out = flockQueue[flockQTail];
    flockQTail = (flockQTail + 1) % FLOCK_QUEUE_SIZE;
    got = true;
  }
  portEXIT_CRITICAL(&flockQMux);
  return got;
}

// The drive-tested allowlist entry. An exact string match against this is
// what earns tier 5.
static const char FLOCK_PROBE_IE_SIG_PRIMARY[] =
    "2,12,127,221:506f9a16030103,45,191,221:0050f208000000";
// Vendor OUI 50:6f:9a (LiteON / the Flock WiFi stack). Used as an anchor
// to recover a signature whose leading tags were lost to parse skew.
static const char FLOCK_LITEON_IE_SIG_PREFIX[] = "221:506f9a16030103";

#define FY_IE_SSID 0
#define FY_IE_VENDOR 221
#define FY_PHANTOM_SKIP_CAP 16
#define FY_TLV_RESYNC_MAX 64

// Returns:
//    1 = wildcard SSID element present (tag 0, length 0) -> Flock-style probe
//    0 = SSID element present with a real name -> directed probe, not ours
//   -1 = no SSID element found -> caller may retry with the FCS stripped
//
// Replaces the old `strcmp(ssid, "<hidden>")` test, which couldn't tell
// case 0 from case -1 -- extractSSID() returns "<hidden>" for both a
// zero-length SSID and a missing one. That conflation mattered.
static int isWildcardProbeIE(const uint8_t *body, int len)
{
  if (!body || len < 2)
    return -1;
  while (len >= 2)
  {
    uint8_t id = body[0];
    uint8_t elen = body[1];
    if ((int)elen + 2 > len)
      break;
    if (id == 0)
      return (elen == 0) ? 1 : 0;
    body += elen + 2;
    len -= elen + 2;
  }
  return -1;
}

static void fyHexNibbles(char *dst, const uint8_t *b, int n)
{
  static const char hd[] = "0123456789abcdef";
  for (int i = 0; i < n; i++)
  {
    dst[i * 2] = hd[b[i] >> 4];
    dst[i * 2 + 1] = hd[b[i] & 0x0f];
  }
}

// True when ies[pos] starts vendor element 221 with OUI 50:6f:9a.
static bool fyLiteonVendorAt(const uint8_t *ies, int len, int pos)
{
  return pos + 9 <= len && ies[pos] == FY_IE_VENDOR && ies[pos + 1] == 7 &&
         ies[pos + 2] == 0x50 && ies[pos + 3] == 0x6f && ies[pos + 4] == 0x9a;
}

// Look ahead past a bogus element header for a real LiteON vendor element.
// Finding one means we hit a driver length/FCS skew artefact rather than
// the genuine end of the frame.
static bool fyPhantomLiteonAhead(const uint8_t *ies, int len, int pos)
{
  int end = pos + 2 + 32;
  if (end > len - 1)
    end = len - 1;
  for (int j = pos + 2; j < end; j++)
  {
    if (fyLiteonVendorAt(ies, len, j))
      return true;
  }
  return false;
}

static bool fyIsPhantomOverflow(const uint8_t *ies, int len, uint8_t id, int elen, int i)
{
  if (i + 2 + elen <= len)
    return false;
  if (elen > 200)
    return true;
  return id == 64 && elen == 128 && fyPhantomLiteonAhead(ies, len, i);
}

// After a parse failure, slide forward looking for the next plausible
// element header.
static int fyTlvResync(const uint8_t *ies, int len, int start)
{
  int end = start + FY_TLV_RESYNC_MAX;
  if (end > len - 1)
    end = len - 1;
  for (int j = start; j < end; j++)
  {
    int elen = (int)ies[j + 1];
    if (elen <= 200 && j + 2 + elen <= len)
      return j;
  }
  return -1;
}

static bool fySigAppend(char *out, size_t cap, size_t *pos, const char *part)
{
  size_t plen = strlen(part);
  if (*pos != 0)
  {
    if (*pos + 1 >= cap)
      return false;
    out[(*pos)++] = ',';
  }
  if (*pos + plen >= cap)
    return false;
  memcpy(out + *pos, part, plen);
  *pos += plen;
  out[*pos] = '\0';
  return true;
}

static bool fySigAppendTag(char *out, size_t cap, size_t *pos, uint8_t id)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", (unsigned)id);
  return fySigAppend(out, cap, pos, buf);
}

// Vendor elements are encoded as "221:" plus up to 8 payload bytes in hex.
static bool fySigAppendVendor(char *out, size_t cap, size_t *pos, const uint8_t *body, int elen)
{
  char buf[24];
  int take = elen < 8 ? elen : 8;
  buf[0] = '2';
  buf[1] = '2';
  buf[2] = '1';
  buf[3] = ':';
  fyHexNibbles(buf + 4, body, take);
  buf[4 + take * 2] = '\0';
  return fySigAppend(out, cap, pos, buf);
}

// Walk the element list and build the fingerprint string.
static bool fyBuildFlockIeSigFromIes(const uint8_t *ies, int len, char *out, size_t cap, bool *complete)
{
  if (!ies || len < 2 || !out || cap < 2)
    return false;
  size_t pos = 0;
  out[0] = '\0';
  int i = 0;
  uint8_t phantomSkips = 0;
  while (i + 2 <= len)
  {
    uint8_t id = ies[i];
    int elen = (int)ies[i + 1];
    if (i + 2 + elen > len)
    {
      if (phantomSkips < FY_PHANTOM_SKIP_CAP && fyIsPhantomOverflow(ies, len, id, elen, i))
      {
        phantomSkips++;
        i += 2;
        continue;
      }
      int j = fyTlvResync(ies, len, i);
      if (j > i)
      {
        i = j;
        continue;
      }
      return false;
    }
    i += 2;
    if (id == FY_IE_SSID)
    {
      if (elen == 0)
      {
        while (i + 2 <= len && ies[i] == 0 && ies[i + 1] == 0)
          i += 2;
      }
      else
      {
        i += elen;
      }
      continue;
    }
    if (id == FY_IE_VENDOR && elen >= 4)
    {
      if (!fySigAppendVendor(out, cap, &pos, ies + i, elen))
        return false;
    }
    else
    {
      if (!fySigAppendTag(out, cap, &pos, id))
        return false;
    }
    i += elen;
  }
  if (complete)
    *complete = (i == len);
  return pos > 0;
}

// Recover a signature whose leading tags were truncated, by re-anchoring
// on the LiteON vendor element.
static void fyCanonicalizeFlockIeSig(char *sig, size_t cap)
{
  if (!sig || cap < 8)
    return;
  if (strncmp(sig, "2,12,127,", 9) == 0 && strstr(sig, FLOCK_LITEON_IE_SIG_PREFIX) != nullptr)
    return;
  const char *anchor = strstr(sig, FLOCK_LITEON_IE_SIG_PREFIX);
  if (!anchor)
    return;
  char tmp[128];
  int n = snprintf(tmp, sizeof(tmp), "2,12,127,%s", anchor);
  if (n > 0 && (size_t)n < cap)
    memcpy(sig, tmp, (size_t)n + 1);
}

static bool fyPickBetterSig(const char *a, bool aComplete, const char *b, bool bComplete,
                            char *out, size_t cap)
{
  if (!a[0] && !b[0])
    return false;
  if (a[0] && !b[0])
  {
    strncpy(out, a, cap - 1);
    out[cap - 1] = '\0';
    return true;
  }
  if (!a[0] && b[0])
  {
    strncpy(out, b, cap - 1);
    out[cap - 1] = '\0';
    return true;
  }
  const char *pick = a;
  if (aComplete && !bComplete)
    pick = a;
  else if (!aComplete && bComplete)
    pick = b;
  else if (strlen(b) > strlen(a))
    pick = b;
  strncpy(out, pick, cap - 1);
  out[cap - 1] = '\0';
  return true;
}

// Parse the body twice -- once as-is, once skipping a leading empty SSID
// element pair -- and keep whichever produced the better signature.
static bool fyBuildFlockIeSigFromProbeBody(const uint8_t *body, int bodyLen, char *out, size_t cap)
{
  if (!body || bodyLen < 2 || !out || cap < 16)
    return false;
  char sigA[128] = {0};
  char sigB[128] = {0};
  bool completeA = false, completeB = false;
  bool okA = fyBuildFlockIeSigFromIes(body, bodyLen, sigA, sizeof(sigA), &completeA);
  bool okB = false;
  if (bodyLen >= 2 && body[0] == 0 && body[1] == 0)
  {
    okB = fyBuildFlockIeSigFromIes(body + 2, bodyLen - 2, sigB, sizeof(sigB), &completeB);
  }
  char merged[128] = {0};
  if (!fyPickBetterSig(okA ? sigA : "", completeA, okB ? sigB : "", completeB, merged, sizeof(merged)))
    return false;
  fyCanonicalizeFlockIeSig(merged, sizeof(merged));
  strncpy(out, merged, cap - 1);
  out[cap - 1] = '\0';
  return out[0] != '\0';
}

static bool fyFlockIeSigIsPrimary(const char *sig)
{
  return sig && strcmp(sig, FLOCK_PROBE_IE_SIG_PRIMARY) == 0;
}

// Try the full body, then again with 4 bytes trimmed in case the driver
// included the FCS trailer in sig_len.
static bool fyProbeBodyFlockIeSigPrimary(const uint8_t *body, int bodyLen)
{
  char ieSig[128];
  if (fyBuildFlockIeSigFromProbeBody(body, bodyLen, ieSig, sizeof(ieSig)) &&
      fyFlockIeSigIsPrimary(ieSig))
    return true;
  if (bodyLen > 4 && fyBuildFlockIeSigFromProbeBody(body, bodyLen - 4, ieSig, sizeof(ieSig)) &&
      fyFlockIeSigIsPrimary(ieSig))
    return true;
  return false;
}

void onFlockDetection(const char *macStr, const char *label);

void wifiSnifferCallback(void *buf, wifi_promiscuous_pkt_type_t type)
{
  // CHANGED: was `if (type != WIFI_PKT_MGMT) return;` -- data frames were
  // discarded entirely. Flock stations sleep most of their duty cycle and
  // during those windows never transmit, but nearby APs still address data
  // frames TO them. Those frames carry the station in addr1. Discarding
  // data frames threw away every one of those detections.
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)
    return;

  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  uint8_t *payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  int8_t rssi = pkt->rx_ctrl.rssi;

  statFramesSeen++;
  if (type == WIFI_PKT_MGMT)
    statMgmtFrames++;
  else
    statDataFrames++;

  // Weak frames are more likely to be corrupt, and a corrupted MAC that
  // happens to land on a Flock OUI is a false positive.
  if (rssi < RSSI_MIN)
    return;

  // Every 802.11 frame has addr1/addr2/addr3 at the same offsets, so the
  // mgmt header struct is safe to use for address extraction on data
  // frames too. A 3-address header is 24 bytes.
  if (len < 24)
    return;

  uint16_t frameControl = (payload[1] << 8) | payload[0];
  uint8_t subtype = (frameControl & 0x00F0) >> 4;
  bool isMgmt = (type == WIFI_PKT_MGMT);

  // For management frames, restrict to the subtypes that carry useful
  // information. Data frames are taken as-is for the addr1/addr2 check.
  if (isMgmt && subtype != 8 && subtype != 4 && subtype != 5)
    return;

  wifi_mgmt_hdr_t *hdr = (wifi_mgmt_hdr_t *)payload;

  // Use the channel the driver actually received on. With 250ms hopping
  // the global currentChannel can already have moved on by the time this
  // frame is processed, which would mislabel the detection.
  uint8_t rxChannel = (uint8_t)pkt->rx_ctrl.channel;

  // ---- Tiered Flock evaluation ----
  //
  // Evaluated BEFORE the general dedup check. Previously dedup ran first,
  // so a camera seen recently on any unrelated frame was dropped before
  // ever being tested. Order matters here.
  uint8_t tier = FLOCK_TIER_NONE;
  const uint8_t *flockMac = nullptr;

  bool addr2Match = !isMulticast(hdr->addr2) &&
                    !isLocallyAdministered(hdr->addr2) &&
                    matchesFlockOUI(hdr->addr2);

  if (addr2Match)
  {
    tier = FLOCK_TIER_ADDR2;
    flockMac = hdr->addr2;
  }
  else if (!isMulticast(hdr->addr1) &&
           !isLocallyAdministered(hdr->addr1) &&
           matchesFlockOUI(hdr->addr1))
  {
    // Receiver-side match (@NitekryDPaul's technique). Lowest tier: a
    // nearby AP answering a Flock probe request can also land here, which
    // is why flock-you treats this path as the noisiest.
    tier = FLOCK_TIER_ADDR1;
    flockMac = hdr->addr1;
  }

  // NOTE: the old extractSSID() call that used to live here is gone.
  // Wildcard detection is now done by isWildcardProbeIE() inside the
  // probe-request branch below, which walks the element list directly and
  // can distinguish "empty SSID" from "no SSID element present" --
  // something extractSSID() could not do, since it returns "<hidden>" for
  // both. extractSSID() is still used elsewhere and is left in place.

  // Tier upgrades for probe requests from a Flock-OUI transmitter.
  if (addr2Match && isMgmt && subtype == 4)
  {
    tier = FLOCK_TIER_PROBE;

    // A probe request has no fixed fields, so the element list starts
    // immediately after the 24-byte MAC header.
    const uint8_t *body = payload + 24;
    int bodyLen = len - 24;

    int wildcard = (bodyLen > 0) ? isWildcardProbeIE(body, bodyLen) : -1;

    // FCS-trailer retry, but ONLY when the first parse found no SSID
    // element at all (-1). A found-but-non-empty result (0) means a
    // legitimate directed probe -- retrying there would misclassify it.
    if (wildcard == -1 && bodyLen > 4)
      wildcard = isWildcardProbeIE(body, bodyLen - 4);

    if (wildcard == 1)
    {
      tier = FLOCK_TIER_WILDCARD;
      // Top tier: the IE fingerprint also matches the drive-tested
      // signature. A wildcard probe from a Flock OUI whose IE fields
      // DON'T match stays at tier 4 -- it's either a camera running
      // firmware nobody has fingerprinted yet, or an unrelated device
      // that happens to share the OUI. Worth hearing either way, worth
      // telling apart.
      if (fyProbeBodyFlockIeSigPrimary(body, bodyLen))
        tier = FLOCK_TIER_IE_SIG;
    }
  }

  char macStr[18];

  if (tier != FLOCK_TIER_NONE)
  {
    // Report the MAC that actually matched, not always addr2. On an
    // addr1-only hit addr2 is some unrelated AP, and logging that was
    // actively misleading when trying to identify a camera.
    macToStr(flockMac, macStr);
    statFlockRaw++;
    if (shouldAlertFlock(macStr, tier))
    {
      // Queue it and get out. All the slow work (serial, buzzer, LoRa
      // announce, display) happens in loop(). Calling onFlockDetection()
      // from here is what wedged the WiFi task.
      enqueueFlockEvent(macStr, tier, rssi, rxChannel);
    }
    return;
  }

  // ---- Non-Flock traffic: general dedup, no alert ----
  macToStr(hdr->addr2, macStr);
  if (!shouldEmit(macStr))
    return;
}

void wifiChannelHop()
{
  unsigned long now = millis();
  if (now - lastHop >= CHANNEL_HOP_INTERVAL_MS)
  {
    // CHANGED: descending 11 -> 6 -> 1 instead of ascending 1..13.
    // Cameras sweep ascending; going the other way cuts across their
    // sweep instead of travelling alongside it.
    static uint8_t channelIndex = 0;
    channelIndex = (channelIndex + 1) % NUM_CHANNELS;
    currentChannel = CHANNEL_SEQUENCE[channelIndex];
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
// Fixed node identity -- use a DIFFERENT key on every board.
// Without this, RNS::Identity() below would generate a brand-new
// random identity every single boot, meaning this node's destination
// hash changes on every reflash/reset and nobody (including you)
// can reliably recognize "this is the same node as last time."
//
// Generate a fresh one per board with:
//   python3 -c "import secrets; print(secrets.token_hex(65))"
// Board A and Board B MUST use different keys, or they'll both
// announce as the exact same identity.
// ---------------------------------------------------------------
// The key lives in include/node_key.h, which is gitignored. Copy
// include/node_key.h.example to include/node_key.h and paste in a key.
#if __has_include("node_key.h")
#include "node_key.h"
#else
#error "Missing include/node_key.h -- copy include/node_key.h.example to include/node_key.h and set NODE_PRIVATE_KEY_HEX (see README, 'Per-board identity')."
#endif
// 65 bytes = 130 hex characters. Catches the placeholder and typos at compile time.
static_assert(sizeof(NODE_PRIVATE_KEY_HEX) - 1 == 130,
              "NODE_PRIVATE_KEY_HEX must be exactly 130 hex characters (65 bytes)");

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

    // LoRa interface (SX1262, 915MHz, B2B connector -- see
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
  strncpy(lastFlockMac, macStr, sizeof(lastFlockMac) - 1);
  lastFlockMac[sizeof(lastFlockMac) - 1] = '\0';
  strncpy(lastFlockType, label, sizeof(lastFlockType) - 1);
  lastFlockType[sizeof(lastFlockType) - 1] = '\0';
  lastFlockMillis = millis();

  buzzerBeep();

  if (rnsReady)
  {
    char appData[48];
    snprintf(appData, sizeof(appData), "FLOCK %s %s", macStr, label);
    announceDestination.announce(RNS::bytesFromString(appData));
    lastAnnounceMillis = millis();
  }
}

// Drains the detection queue. Called from loop(), NOT from the WiFi
// callback -- this is where the blocking work is allowed to happen.
void drainFlockQueue()
{
  FlockEvent e;
  // Bounded per iteration so a burst can't monopolise loop() and stall
  // LoRa receive polling.
  int processed = 0;
  while (processed < 4 && dequeueFlockEvent(&e))
  {
    processed++;
    statHitsProcessed++;
    const char *label = tierLabel(e.tier);

    // CSV-friendly line: parseable with `screen ... | tee flock_log.csv`
    // or any serial capture, while staying readable on screen.
    Serial.printf("[flock] hit %s (%s) rssi=%d ch=%u t=%lu\n",
                  e.mac, label, e.rssi, e.channel, (unsigned long)millis());

    onFlockDetection(e.mac, label);
  }
}

// ============================================================
// ---- OLED: rolling 2-line status, no buttons ----
// Refreshes every second; rotates between screens every 3 seconds.
// ============================================================
unsigned long lastDisplayUpdate = 0;
#define DISPLAY_INTERVAL_MS 250 // was 1000 -- the UI needs to feel
                                // responsive to the encoder now
#define ROTATE_INTERVAL_MS 3000
#define NUM_SCREENS 5 // was 4; screen 4 is the message list

#define SCREEN_RNS 0
#define SCREEN_STATS 1
#define SCREEN_FLOCK 2
#define SCREEN_RXMSG 3
#define SCREEN_MESSAGES 4

// After this long with no encoder input, auto-rotation resumes on its
// own. Prevents the display being parked forever if you bump the knob
// and pocket the device.
#define UI_IDLE_TIMEOUT_MS 30000

// ---- Preset messages ----
//
// Edit freely. Keep them short: an announce payload has room, but at
// text size 1 roughly 20 characters per line is what fits cleanly.
// Keep these SHORT. At text size 2 the panel fits ~10 characters per
// line and the screen shows at most two lines, so ~20 characters total.
// Longer strings get truncated rather than wrapped to a third line --
// better a clipped message than a garbled screen.
const char *PRESET_MESSAGES[] = {
    "OK",
    "CHECK IN",
    "ON MY WAY",
    "ARRIVED",
    "ALL CLEAR",
    "STANDBY",
    "CAMERA",
    "NEED HELP",
};
#define NUM_PRESET_MESSAGES (sizeof(PRESET_MESSAGES) / sizeof(PRESET_MESSAGES[0]))

// ---- UI state ----
//
// Two levels, which is what keeps the encoder unambiguous:
//
//   BROWSE  rotate = change screen
//           press  = enter the message list (on screen 4) or resume
//                   auto-rotation (anywhere else)
//   SELECT  rotate = scroll the message list
//           press  = send the highlighted message, return to BROWSE
//
// Without the two levels, rotation on the message screen would have to
// mean both "next screen" and "next message" at once.
enum UiMode
{
  UI_AUTO,   // rotating on a timer, as before
  UI_BROWSE, // manual screen selection, rotation halted
  UI_SELECT  // inside the message list
};

UiMode uiMode = UI_AUTO;
int uiScreen = SCREEN_RNS;
int msgIndex = 0;
unsigned long lastUiInput = 0;
char lastSentMsg[24] = "";
unsigned long lastSentMillis = 0;

void sendPresetMessage(int idx);

// Polls the encoder. Returns immediately if no encoder is attached, so
// the firmware runs fine before you wire one up.
void serviceEncoder()
{
  if (!encoderPresent)
    return;

  // Poll at ~50Hz rather than every loop() iteration. Each call is two
  // I2C transactions, and hammering the bus competes with the OLED
  // refresh for no benefit -- nobody turns a knob faster than this.
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < 20)
    return;
  lastPoll = millis();

  // ---- rotation ----
  int32_t pos = encoder.getEncoderPosition();
  if (pos != encoderLastPos)
  {
    int delta = (int)(pos - encoderLastPos);
    encoderLastPos = pos;
    lastUiInput = millis();

    if (uiMode == UI_AUTO)
    {
      // First touch of the knob takes over from auto-rotation, landing
      // on whatever screen was already showing rather than jumping.
      uiMode = UI_BROWSE;
      uiScreen = (int)((millis() / ROTATE_INTERVAL_MS) % NUM_SCREENS);
    }
    else if (uiMode == UI_SELECT)
    {
      msgIndex += delta;
      while (msgIndex < 0)
        msgIndex += NUM_PRESET_MESSAGES;
      msgIndex %= NUM_PRESET_MESSAGES;
      return;
    }

    uiScreen += delta;
    while (uiScreen < 0)
      uiScreen += NUM_SCREENS;
    uiScreen %= NUM_SCREENS;
  }

  // ---- push switch (active low) ----
  bool btnDown = !encoder.digitalRead(SEESAW_SWITCH_PIN);
  if (btnDown && !encoderBtnWasDown)
  {
    lastUiInput = millis();

    if (uiMode == UI_SELECT)
    {
      sendPresetMessage(msgIndex);
      uiMode = UI_BROWSE;
    }
    else if (uiScreen == SCREEN_MESSAGES)
    {
      uiMode = UI_SELECT;
    }
    else
    {
      // Press anywhere else toggles auto-rotation back on/off.
      uiMode = (uiMode == UI_AUTO) ? UI_BROWSE : UI_AUTO;
    }
  }
  encoderBtnWasDown = btnDown;

  // ---- idle timeout ----
  if (uiMode != UI_AUTO && (millis() - lastUiInput) > UI_IDLE_TIMEOUT_MS)
  {
    uiMode = UI_AUTO;
  }
}

// Draws the message screen -- ONE message at a time, large.
//
// CHANGED: this used to show a 4-row scrolling list with an inverted
// highlight bar. On a 0.96" 128x64 panel that is far too dense: four rows
// of 12px text plus a header leaves no margins, and the full-width
// inverted bar makes the whole screen read as noise. One message at a
// time with a position counter is legible at arm's length and at a
// glance, which is what actually matters on a device you check while
// walking.
void drawMessageScreen()
{
  // Header line: mode and position, small.
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  if (uiMode == UI_SELECT)
    display.print("SEND?");
  else
    display.print("MSG");

  // Position counter, right-aligned. 6px per character at size 1.
  char pos[12];
  snprintf(pos, sizeof(pos), "%d/%d", msgIndex + 1, (int)NUM_PRESET_MESSAGES);
  int posWidth = strlen(pos) * 6;
  display.setCursor(128 - posWidth - 6, 0);
  display.print(pos);

  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // The message itself, size 2, wrapped across up to two lines.
  // 128px / 12px per char = 10 characters per line at size 2.
  const char *msg = PRESET_MESSAGES[msgIndex];
  const int charsPerLine = 10;
  int len = strlen(msg);

  display.setTextSize(2);

  if (len <= charsPerLine)
  {
    // Fits on one line -- centre it vertically for a cleaner look.
    display.setCursor(0, 26);
    display.print(msg);
  }
  else
  {
    // Break on a space if there's one in a sensible place, so words
    // aren't split mid-way.
    int brk = charsPerLine;
    for (int b = charsPerLine; b > 3; b--)
    {
      if (msg[b] == ' ')
      {
        brk = b;
        break;
      }
    }
    display.setCursor(0, 18);
    for (int i = 0; i < brk && i < charsPerLine; i++)
      display.write(msg[i]);

    int i = brk;
    while (msg[i] == ' ')
      i++;
    display.setCursor(0, 38);
    for (int k = 0; i + k < len && k < charsPerLine; k++)
      display.write(msg[i + k]);
  }

  // Footer hint, small.
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(uiMode == UI_SELECT ? "press to send" : "press to pick");
}

// Word-wraps text at size 1 instead of running off the right edge --
// the other half of the visual glitch on the received-message screen.
void drawWrapped(const char *text, int y)
{
  display.setTextSize(1);
  display.setCursor(0, y);
  const int charsPerLine = 21;
  int len = strlen(text);
  int line = 0;
  int i = 0;
  while (i < len && line < 4)
  {
    int take = (len - i < charsPerLine) ? (len - i) : charsPerLine;
    // Prefer breaking on a space if one is within reach.
    if (i + take < len)
    {
      for (int b = take; b > charsPerLine / 2; b--)
      {
        if (text[i + b] == ' ')
        {
          take = b;
          break;
        }
      }
    }
    display.setCursor(0, y + line * 9);
    for (int k = 0; k < take; k++)
      display.write(text[i + k]);
    i += take;
    while (i < len && text[i] == ' ')
      i++;
    line++;
  }
}

void updateDisplay()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  int screen;
  if (uiMode == UI_AUTO)
    screen = (int)((millis() / ROTATE_INTERVAL_MS) % NUM_SCREENS);
  else
    screen = uiScreen;

  // A single-pixel marker in the top-right corner: filled means manual
  // (rotation halted), absent means auto. Small enough not to intrude,
  // enough to answer "why isn't it rotating".
  bool manual = (uiMode != UI_AUTO);

  if (screen == SCREEN_MESSAGES)
  {
    drawMessageScreen();
  }
  else
  {
    display.setTextSize(2);
    display.setCursor(0, 0);

    switch (screen)
    {
    case SCREEN_RNS:
      display.println(rnsReady ? "RNS: up" : "RNS: init");
      display.println("915 SX1262");
      break;

    case SCREEN_STATS:
      display.print("ch: ");
      display.println(currentChannel);
      display.print("hits: ");
      display.println(flockHitCount);
      break;

    case SCREEN_FLOCK:
      if (flockHitCount > 0)
      {
        unsigned long secsAgo = (millis() - lastFlockMillis) / 1000;
        display.setTextSize(1);
        display.println(lastFlockType);
        display.println(lastFlockMac);
        display.print(secsAgo);
        display.println("s ago");
      }
      else
      {
        display.println("no");
        display.println("detections");
      }
      break;

    case SCREEN_RXMSG:
    default:
      if (lastSentMillis > 0 && (millis() - lastSentMillis) < 4000)
      {
        display.setTextSize(1);
        display.println("SENT:");
        drawWrapped(lastSentMsg, 12);
      }
      else
      {
        drawWrapped(lastReceivedText, 0);
      }
      break;
    }
  }

  // Manual-mode marker. On the message screen the header already says
  // MSG/SEND?, and a dot in the corner there collided with the position
  // counter -- so only draw it on the other screens.
  if (manual && screen != SCREEN_MESSAGES)
    display.fillRect(124, 0, 4, 4, SSD1306_WHITE);

  display.display();
}

// Sends a preset message as an RNS announce with the text in app_data --
// the same path the periodic announce and the serial-input path use.
void sendPresetMessage(int idx)
{
  if (idx < 0 || idx >= (int)NUM_PRESET_MESSAGES)
    return;

  const char *msg = PRESET_MESSAGES[idx];
  strncpy(lastSentMsg, msg, sizeof(lastSentMsg) - 1);
  lastSentMsg[sizeof(lastSentMsg) - 1] = '\0';
  lastSentMillis = millis();

  Serial.printf("[tx] preset message: \"%s\"\n", msg);

  if (rnsReady)
  {
    announceDestination.announce(RNS::bytesFromString(msg));
    lastAnnounceMillis = millis();
    buzzerBeep(60);
  }
  else
  {
    Serial.println("[tx] RNS not ready -- message NOT sent");
  }
}

// ============================================================
// ---- SETUP / LOOP ----
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(500);
  memset(dedupCache, 0, sizeof(dedupCache));
  memset(flockCache, 0, sizeof(flockCache));

  RNS::loglevel(RNS::LOG_INFO);

  // Buzzer
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // OLED
  // MUST remap the I2C bus before display.begin(). The XIAO's default
  // I2C pins are not available on this board (D4 goes to the LoRa module),
  // and Wire.begin() with no arguments would silently talk to the wrong
  // pins and find nothing.
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  // Bound every I2C transaction. Without this, a device holding SDA low
  // (bad wiring, half-seated jumper) can block loop() indefinitely --
  // which looks exactly like "the screen froze".
  Wire.setTimeOut(50);

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
  // ---- I2C bus scan ----
  // Prints every responding address at boot. Cheap, and it answers
  // "is the device even on the bus?" without reflashing a test sketch.
  {
    Serial.println("[i2c] scanning bus...");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++)
    {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0)
      {
        found++;
        Serial.printf("[i2c]   0x%02X", addr);
        if (addr == 0x3C || addr == 0x3D)
          Serial.print("  <- OLED");
        if (addr >= 0x36 && addr <= 0x3D)
          Serial.print("  (seesaw range 0x36-0x3D)");
        Serial.println();
      }
    }
    Serial.printf("[i2c] %d device(s)\n", found);
  }

  // ---- Rotary encoder (optional) ----
  //
  // MUST come after Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL) above.
  // Previously this ran before the bus remap, so it probed the XIAO's
  // DEFAULT I2C pins (D4/D5) -- and D4 is the LoRa module's RST line.
  // It hung there and never printed anything, which is why no [enc]
  // line appeared at all, not even the not-found one.
  // Same I2C bus as the OLED. If it isn't wired yet, the firmware runs
  // normally with auto-rotating screens -- nothing here is fatal.
  // Try the full seesaw address range. The breakout's default is 0x36,
  // but the two solder jumpers on the back shift it up to 0x39 -- and a
  // blob of stray solder there is an easy thing to miss.
  uint8_t encAddr = 0;
  for (uint8_t a = 0x36; a <= 0x3D; a++)
  {
    if (a == 0x3C || a == 0x3D)
      continue; // OLED lives here
    if (encoder.begin(a))
    {
      encAddr = a;
      break;
    }
  }

  if (encAddr != 0)
  {
    uint32_t ver = ((encoder.getVersion() >> 16) & 0xFFFF);
    Serial.printf("[enc] seesaw found at 0x%02X, product %lu\n",
                  encAddr, (unsigned long)ver);
    if (ver != 4991)
      Serial.println("[enc] note: product id is not 4991 - check you have the encoder breakout");
    encoder.pinMode(SEESAW_SWITCH_PIN, INPUT_PULLUP);
    encoderLastPos = encoder.getEncoderPosition();
    encoderPresent = true;
  }
  else
  {
    Serial.println("[enc] no seesaw found in 0x36-0x3B - screens will auto-rotate");
    encoderPresent = false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_promiscuous(true);
  // CHANGED: explicitly allow DATA frames through the promiscuous filter
  // as well as MGMT. The addr1 receiver-side catch depends on seeing data
  // frames addressed to a sleeping Flock station.
  wifi_promiscuous_filter_t filter = {
      .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
  esp_wifi_set_promiscuous_filter(&filter);

  esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);

  // Start on the first channel of the descending sequence.
  currentChannel = CHANNEL_SEQUENCE[0];
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

  // BLE
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new BLEAdvCallback(), true);
  // ---- BLE scan duty cycle ----
  //
  // CHANGED: was setInterval(100) / setWindow(99) -- a 99% duty cycle.
  //
  // The ESP32-S3 has ONE 2.4GHz radio, shared between WiFi and BLE by a
  // coexistence arbiter. A 99% BLE duty cycle leaves WiFi promiscuous
  // mode roughly 1% of the radio, which is why the heartbeat showed ~0.7
  // frames/sec when a normal environment produces hundreds per second.
  // Almost nothing was reaching the Flock matcher -- `raw=0` in the
  // heartbeat means no frame ever even got as far as the OUI comparison.
  //
  // 30% gives WiFi the majority of the radio while still catching BLE
  // advertisements, which repeat every 20-100ms on most devices. Raise
  // BLE_SCAN_WINDOW back up only if BLE detection matters more than
  // WiFi -- and watch the `frames` counter when you do.
  //
  // Also switched to PASSIVE scanning. Active scan transmits a scan
  // request for every advertisement heard, taking the radio for TX as
  // well as RX. The BLE Flock/Raven heuristics are still a stub, so the
  // device names that active scanning buys aren't being used anyway.
  pBLEScan->setActiveScan(false);
  pBLEScan->setInterval(BLE_SCAN_INTERVAL);
  pBLEScan->setWindow(BLE_SCAN_WINDOW);
  pBLEScan->start(0, nullptr, false);

  // Reticulum + SX1262
  reticulum_setup();

  Serial.println("# combined RNS+flock firmware online");
}

void loop()
{
  wifiChannelHop();
  reticulum_loop();

  // Drain detections BEFORE the announce/serial work, so a hit is acted
  // on as promptly as possible now that it's no longer handled inline.
  drainFlockQueue();

  buzzerService();
  serviceEncoder();

  maybeAnnounce();
  checkSerialInput();

  static unsigned long lastBleRestart = 0;
  if (millis() - lastBleRestart > 10000)
  {
    pBLEScan->clearResults();
    lastBleRestart = millis();
  }

  // ---- Heartbeat / wedge detector ----
  //
  // This is the instrument for the "stops detecting until reset" bug.
  // Watch the `frames` counter: if it stops climbing while the device is
  // still powered and near WiFi traffic, the promiscuous callback has
  // stopped firing and the WiFi task is wedged. If it keeps climbing but
  // `hits` stays flat near a known camera, the callback is alive and the
  // problem is in matching instead. Those are completely different bugs
  // and this line tells them apart without a laptop full of logs.
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 10000)
  {
    lastHeartbeat = millis();
    Serial.printf("[hb] up=%lus frames=%lu (mgmt=%lu data=%lu) raw=%lu hits=%lu drops=%lu ch=%u heap=%lu\n",
                  (unsigned long)(millis() / 1000),
                  (unsigned long)statFramesSeen,
                  (unsigned long)statMgmtFrames,
                  (unsigned long)statDataFrames,
                  (unsigned long)statFlockRaw,
                  (unsigned long)statHitsProcessed,
                  (unsigned long)statQueueDrops,
                  currentChannel,
                  (unsigned long)ESP.getFreeHeap());
  }

  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL_MS)
  {
    updateDisplay();
    lastDisplayUpdate = millis();
  }
}
