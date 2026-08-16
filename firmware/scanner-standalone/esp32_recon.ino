

#include <WiFi.h>
#include "esp_wifi.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ---------- Config ----------
#define CHANNEL_HOP_INTERVAL_MS   300     // how long to dwell per WiFi channel
#define DEDUP_WINDOW_MS           30000   // re-emit same MAC only after this long
#define MAX_DEDUP_ENTRIES         256     // ring buffer size for seen-MAC cache
#define BLE_SCAN_INTERVAL_MS      100     // BLE scan window params (ms)
#define BLE_SCAN_WINDOW_MS        99

// ---------- Dedup cache (shared by WiFi + BLE, keyed by MAC string) ----------
struct SeenEntry {
  char mac[18];       // "AA:BB:CC:DD:EE:FF\0"
  unsigned long lastSeen;
  bool used;
};

SeenEntry dedupCache[MAX_DEDUP_ENTRIES];

bool shouldEmit(const char* mac) {
  unsigned long now = millis();

  for (int i = 0; i < MAX_DEDUP_ENTRIES; i++) {
    if (dedupCache[i].used && strcmp(dedupCache[i].mac, mac) == 0) {
      if (now - dedupCache[i].lastSeen < DEDUP_WINDOW_MS) {
        return false; // seen recently, suppress
      }
      dedupCache[i].lastSeen = now;
      return true; // cooldown expired, re-emit
    }
  }

  int oldestIdx = 0;
  unsigned long oldestTime = ULONG_MAX;
  for (int i = 0; i < MAX_DEDUP_ENTRIES; i++) {
    if (!dedupCache[i].used) {
      strncpy(dedupCache[i].mac, mac, sizeof(dedupCache[i].mac));
      dedupCache[i].lastSeen = now;
      dedupCache[i].used = true;
      return true;
    }
    if (dedupCache[i].lastSeen < oldestTime) {
      oldestTime = dedupCache[i].lastSeen;
      oldestIdx = i;
    }
  }
  strncpy(dedupCache[oldestIdx].mac, mac, sizeof(dedupCache[oldestIdx].mac));
  dedupCache[oldestIdx].lastSeen = now;
  dedupCache[oldestIdx].used = true;
  return true;
}

// ---------- WiFi promiscuous mode ----------
uint8_t currentChannel = 1;
unsigned long lastHop = 0;

typedef struct {
  uint8_t frame_ctrl[2];
  uint8_t duration[2];
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  uint8_t seq_ctrl[2];
  uint8_t payload[];
} wifi_mgmt_hdr_t;

void macToStr(const uint8_t* mac, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void extractSSID(const uint8_t* payload, int payloadLen, char* ssidOut, int ssidMax) {
  ssidOut[0] = '\0';
  int i = 0;
  while (i + 2 <= payloadLen) {
    uint8_t tagNum = payload[i];
    uint8_t tagLen = payload[i + 1];
    if (i + 2 + tagLen > payloadLen) break;
    if (tagNum == 0) {
      int len = tagLen < ssidMax - 1 ? tagLen : ssidMax - 1;
      memcpy(ssidOut, &payload[i + 2], len);
      ssidOut[len] = '\0';
      for (int j = 0; j < len; j++) if (ssidOut[j] == ',') ssidOut[j] = ';';
      return;
    }
    i += 2 + tagLen;
  }
  strcpy(ssidOut, "<hidden>");
}

void wifiSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  uint8_t* payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;

  uint16_t frameControl = (payload[1] << 8) | payload[0];
  uint8_t subtype = (frameControl & 0x00F0) >> 4;

  const char* frameType;
  if (subtype == 8)      frameType = "WIFI_BEACON";
  else if (subtype == 4) frameType = "WIFI_PROBEREQ";
  else if (subtype == 5) frameType = "WIFI_PROBERESP";
  else return;

  if (len < 36) return;

  wifi_mgmt_hdr_t* hdr = (wifi_mgmt_hdr_t*)payload;
  char macStr[18];
  macToStr(hdr->addr2, macStr);

  if (!shouldEmit(macStr)) return;

  char ssid[33];
  int fixedFieldsLen = (subtype == 4) ? 0 : 12;
  int taggedStart = 24 + fixedFieldsLen;
  extractSSID(payload + taggedStart, len - taggedStart, ssid, sizeof(ssid));

  int8_t rssi = pkt->rx_ctrl.rssi;

  Serial.printf("%s,%s,%s,%d,%d,\n", frameType, macStr, ssid, rssi, currentChannel);
}

void wifiChannelHop() {
  unsigned long now = millis();
  if (now - lastHop >= CHANNEL_HOP_INTERVAL_MS) {
    currentChannel = (currentChannel % 13) + 1;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    lastHop = now;
  }
}

// ---------- BLE scanning ----------
BLEScan* pBLEScan;

class BLEAdvCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    char macStr[18];
    strncpy(macStr, advertisedDevice.getAddress().toString().c_str(), sizeof(macStr));
    for (int i = 0; macStr[i]; i++) macStr[i] = toupper(macStr[i]);

    if (!shouldEmit(macStr)) return;

    char name[33];
    if (advertisedDevice.haveName()) {
      strncpy(name, advertisedDevice.getName().c_str(), sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
      for (int j = 0; name[j]; j++) if (name[j] == ',') name[j] = ';';
    } else {
      strcpy(name, "<no-name>");
    }

    int rssi = advertisedDevice.getRSSI();
    String extra = "";
    if (advertisedDevice.haveManufacturerData()) {
      extra = "mfg_data_present";
    }

    Serial.printf("BLE,%s,%s,%d,,%s\n", macStr, name, rssi, extra.c_str());
  }
};

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);

  memset(dedupCache, 0, sizeof(dedupCache));

  // --- WiFi promiscuous setup ---
  WiFi.mode(WIFI_MODE_NULL);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

  // --- BLE setup ---
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new BLEAdvCallback(), true);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(BLE_SCAN_INTERVAL_MS);
  pBLEScan->setWindow(BLE_SCAN_WINDOW_MS);
  pBLEScan->start(0, nullptr, false);

  Serial.println("# ESP32 recon sniffer online: WIFI+BLE, dedup+hop active, always-on");
}

void loop() {
  wifiChannelHop();

  static unsigned long lastBleRestart = 0;
  if (millis() - lastBleRestart > 10000) {
    pBLEScan->clearResults();
    lastBleRestart = millis();
  }
}
