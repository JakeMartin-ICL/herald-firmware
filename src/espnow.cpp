#include "globals.h"

// ---- Peer table (hub: maps MAC → hwid; client: just stores hub MAC) ----

struct EspNowPeer {
  uint8_t mac[6];
  String  hwid;
  String  version;
};

static EspNowPeer peers[MAX_CLIENTS];
static int        peerCount = 0;
static uint8_t    hubMac[6] = {};
static bool       hubMacKnown = false;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- Peer management ----

static bool macEqual(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, 6) == 0;
}

static bool addEspNowPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0; // current channel
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

static String hwIdForMac(const uint8_t* mac) {
  for (int i = 0; i < peerCount; i++) {
    if (macEqual(peers[i].mac, mac)) return peers[i].hwid;
  }
  return "";
}

static void registerBoxPeer(const uint8_t* mac, const String& hwid, const String& version) {
  // Update existing entry or add new
  for (int i = 0; i < peerCount; i++) {
    if (macEqual(peers[i].mac, mac)) { peers[i].hwid = hwid; peers[i].version = version; return; }
  }
  if (peerCount >= MAX_CLIENTS) return;
  memcpy(peers[peerCount].mac, mac, 6);
  peers[peerCount].hwid = hwid;
  peers[peerCount].version = version;
  peerCount++;
  addEspNowPeer(mac);
}

int getEspNowPeerCount() { return peerCount; }
const String& getEspNowPeerHwid(int i) { return peers[i].hwid; }
const String& getEspNowPeerVersion(int i) { return peers[i].version; }

// ---- Send helpers ----

static void espNowSend(const uint8_t* mac, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  if (out.length() > 250) {
    Serial.printf("ESP-NOW: message too large (%d bytes), dropping\n", out.length());
    return;
  }
  esp_now_send(mac, (const uint8_t*)out.c_str(), out.length());
}

// Send to a specific box by hwid (hub only)
void sendToBoxEspNow(const String& hwid, JsonDocument& doc) {
  for (int i = 0; i < peerCount; i++) {
    if (peers[i].hwid == hwid) {
      espNowSend(peers[i].mac, doc);
      return;
    }
  }
  Serial.printf("ESP-NOW: no peer found for hwid %s\n", hwid.c_str());
}

// Broadcast to all known box peers (hub only)
// Send to hub (client only)
void sendToHubEspNow(JsonDocument& doc) {
  if (hubMacKnown) {
    espNowSend(hubMac, doc);
  } else {
    // Hub MAC not yet known — broadcast; hub responds with hub_ack and we learn its MAC
    espNowSend(BROADCAST_MAC, doc);
  }
}

void sendToAllBoxesEspNow(JsonDocument& doc) {
  for (int i = 0; i < peerCount; i++) {
    espNowSend(peers[i].mac, doc);
  }
}

// ---- Receive callback ----

static void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
  if (len <= 0 || len > 250) return;

  String msg((const char*)data, len);
  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) return;
  const char* msgType = doc["type"] | "";

  if (isHub) {
    // ---- Hub: receiving from a client box ----

    if (strcmp(msgType, "hello") == 0) {
      const char* hwid    = doc["hwid"]    | "";
      const char* version = doc["version"] | "";
      registerBoxPeer(mac_addr, String(hwid), String(version));

      // Acknowledge so the client learns our MAC
      JsonDocument ack;
      ack["type"] = "hub_ack";
      ack["hwid"] = myHwId;
      espNowSend(mac_addr, ack);

      // Notify the web app
      JsonDocument notify;
      notify["type"]    = "connected";
      notify["hwid"]    = hwid;
      notify["version"] = version;
      forwardToApp(notify);

      debugLog("ESP-NOW: box connected: " + String(hwid));
    } else {
      // Add hwid from peer table if not already in message
      if (strlen(doc["hwid"] | "") == 0) {
        String hwid = hwIdForMac(mac_addr);
        if (hwid.length() > 0) doc["hwid"] = hwid;
      }
      forwardToApp(doc);
    }

  } else {
    // ---- Client: receiving from hub ----

    if (strcmp(msgType, "hub_ack") == 0) {
      if (!hubMacKnown) {
        memcpy(hubMac, mac_addr, 6);
        hubMacKnown = true;
        addEspNowPeer(hubMac);
        debugLog("ESP-NOW: hub found, peer registered");
      }
      return;
    }

    // Handle all box commands (LED, display, RFID, OTA, wifi cred, debug)
    if (strncmp(msgType, "led", 3) == 0) {
      handleLedPatternCommand(doc);
    } else if (strcmp(msgType, "display") == 0) {
      handleDisplayCommand(doc);
    } else if (strcmp(msgType, "ota_update") == 0) {
      performOtaUpdate(doc["url"] | "", doc["version"] | "");
    } else if (strcmp(msgType, "debug_on") == 0) {
      debugModeEnabled = true;
    } else if (strcmp(msgType, "debug_off") == 0) {
      debugModeEnabled = false;
    } else if (strcmp(msgType, "rfid_enable") == 0) {
      rfidEnabled = true;
    } else if (strcmp(msgType, "rfid_disable") == 0) {
      rfidEnabled = false;
    } else if (strcmp(msgType, "rfid_write") == 0) {
      handleRfidWrite(doc["internalId"] | "");
    } else if (strcmp(msgType, "assigned") == 0) {
      debugLog("Confirmed hwid: " + myHwId);
    } else if (strcmp(msgType, "wifi_cred") == 0) {
      // Fragmented credential update: collect and save when all received
      int index = doc["index"] | -1;
      int total = doc["total"] | 0;
      if (index < 0 || total <= 0 || index >= MAX_CREDENTIALS) return;
      credentials[index].ssid     = doc["ssid"]     | "";
      credentials[index].password = doc["password"] | "";
      if (index == total - 1) {
        credentialCount = total;
        saveCredentials();
        debugLog("WiFi credentials updated via ESP-NOW");
      }
    }
  }
}

static void onEspNowSend(const uint8_t* mac, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.printf("ESP-NOW: send failed to %02X:%02X:%02X:%02X:%02X:%02X\n",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
}

// ---- Init ----

void initEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSend);

  // Add broadcast peer so we can send discovery messages
  addEspNowPeer(BROADCAST_MAC);
  Serial.println("ESP-NOW initialised");
}

// ---- Client: send hello to hub (broadcast) ----

void sendHelloEspNow() {
  JsonDocument doc;
  doc["type"]    = "hello";
  doc["client"]  = "box";
  doc["hwid"]    = myHwId;
  doc["version"] = FIRMWARE_VERSION;
  espNowSend(BROADCAST_MAC, doc);
}
