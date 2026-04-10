#include "globals.h"

// ---- Hub: client tracking ----

String clientHwIds[MAX_CLIENTS];
String clientVersions[MAX_CLIENTS];
bool clientIsApp[MAX_CLIENTS];
int appClientNum = -1;

void initClientTracking() {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    clientHwIds[i] = "";
    clientVersions[i] = "";
    clientIsApp[i] = false;
  }
}

void forwardToApp(JsonDocument& doc) {
  if (appClientNum >= 0) {
    String out;
    serializeJson(doc, out);
    wsServer.sendTXT(appClientNum, out);
  }
}

void sendToClient(uint8_t clientNum, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  wsServer.sendTXT(clientNum, out);
}

// ---- Hub event handler helpers ----

static void onServerDisconnected(uint8_t clientNum) {
  if (clientIsApp[clientNum]) {
    debugLog("App disconnected (heap: " + String(ESP.getFreeHeap()) + ")");
    appClientNum = -1;
    clientIsApp[clientNum] = false;
  } else if (clientHwIds[clientNum] != "") {
    String hwId = clientHwIds[clientNum];
    debugLog("Box " + hwId + " disconnected");
    JsonDocument notify;
    notify["type"] = "disconnected";
    notify["hwid"] = hwId;
    forwardToApp(notify);
    clientHwIds[clientNum] = "";
    clientVersions[clientNum] = "";
  }
}

static void sendStoredStateBackup(uint8_t clientNum) {
  if (!SPIFFS.exists("/game_state.json")) {
    JsonDocument none;
    none["type"] = "state_backup_none";
    sendToClient(clientNum, none);
    return;
  }
  File f = SPIFFS.open("/game_state.json", "r");
  if (!f) return;
  String stored = f.readString();
  f.close();

  // File contains {"payload":...,"compressed":...}
  // Prepend the type field without re-parsing
  if (stored.length() < 2 || stored[0] != '{') return;
  String msg = "{\"type\":\"state_backup\"," + stored.substring(1);
  if (debugModeEnabled) debugLog("state_backup: " + String(msg.length()) + " bytes, heap: " + String(ESP.getFreeHeap()));
  wsServer.sendTXT(clientNum, msg);
}

static void handleHelloFromApp(uint8_t clientNum) {
  clientIsApp[clientNum] = true;
  appClientNum = clientNum;
  rfidEnabled = false; // app controls RFID state; reset to known baseline on connect
  debugLog("App connected (heap: " + String(ESP.getFreeHeap()) + ")");

  JsonDocument ack;
  ack["type"] = "hello_ack";
  ack["client"] = "hub";
  ack["version"] = FIRMWARE_VERSION;
  // Include GitHub config if hub has one (hello_ack is not forwarded to app log)
  if (getGitHubConfigEnteredAt() > 0) {
    const GitHubConfig& cfg = getGitHubConfig();
    JsonObject ghCfg = ack["github_config"].to<JsonObject>();
    ghCfg["pat"]        = cfg.pat;
    ghCfg["gist_id"]    = cfg.gist_id;
    ghCfg["entered_at"] = cfg.enteredAt;
  }
  sendToClient(clientNum, ack);

  JsonDocument hubBox;
  hubBox["type"] = "connected";
  hubBox["hwid"] = myHwId;
  hubBox["version"] = FIRMWARE_VERSION;
  sendToClient(clientNum, hubBox);

  // Report already connected boxes (with their stored versions)
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clientHwIds[i] != "" && !clientIsApp[i]) {
      JsonDocument notify;
      notify["type"] = "connected";
      notify["hwid"] = clientHwIds[i];
      notify["version"] = clientVersions[i];
      sendToClient(clientNum, notify);
    }
  }
  // Report ESP-NOW connected boxes (client boxes that don't use WebSocket)
  for (int i = 0; i < getEspNowPeerCount(); i++) {
    JsonDocument notify;
    notify["type"] = "connected";
    notify["hwid"] = getEspNowPeerHwid(i);
    notify["version"] = getEspNowPeerVersion(i);
    sendToClient(clientNum, notify);
  }

  // Send hub's battery voltage immediately so app doesn't wait for the 60s timer
  JsonDocument batt;
  batt["type"] = "battery";
  batt["hwid"] = myHwId;
  batt["voltage"] = readBatteryVoltage();
  sendToClient(clientNum, batt);

  // Send saved game state if one exists
  sendStoredStateBackup(clientNum);
}

static void handleHelloFromBox(uint8_t clientNum, JsonDocument& doc) {
  const char* hwId = doc["hwid"];
  const char* version = doc["version"] | "";

  // Evict any stale slot with the same HWID (e.g. box reconnected on a new WS connection)
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (i != clientNum && clientHwIds[i] == hwId) {
      debugLog("Box " + String(hwId) + " evicting stale slot " + String(i));
      clientHwIds[i] = "";
      clientVersions[i] = "";
    }
  }

  clientHwIds[clientNum] = hwId;
  clientVersions[clientNum] = version;

  // Sync GitHub config: if hub has a newer config, push it to this box
  long boxEnteredAt = doc["github_config_entered_at"] | 0L;
  if (getGitHubConfigEnteredAt() > boxEnteredAt) {
    const GitHubConfig& cfg = getGitHubConfig();
    JsonDocument cfgMsg;
    cfgMsg["type"]       = "github_config_set";
    cfgMsg["pat"]        = cfg.pat;
    cfgMsg["gist_id"]    = cfg.gist_id;
    cfgMsg["entered_at"] = cfg.enteredAt;
    sendToClient(clientNum, cfgMsg);
  }

  JsonDocument assign;
  assign["type"] = "assigned";
  assign["hwid"] = hwId;
  sendToClient(clientNum, assign);

  JsonDocument notify;
  notify["type"] = "connected";
  notify["hwid"] = hwId;
  notify["version"] = version;
  forwardToApp(notify);

  debugLog("Box " + String(hwId) + " connected (ws client " + String(clientNum) + ")");
}

static void handleHubCommand(uint8_t clientNum, JsonDocument& doc) {
  const char* msgType = doc["type"] | "";

  if (strncmp(msgType, "led", 3) == 0) {
    handleLedPatternCommand(doc);
  } else if (strcmp(msgType, "display") == 0) {
    handleDisplayCommand(doc);
  } else if (strcmp(msgType, "ota_update") == 0) {
    performOtaUpdate(doc["url"] | "", doc["version"] | "");
  } else if (strcmp(msgType, "debug_on") == 0) {
    debugModeEnabled = true;
    debugLog("Debug mode enabled");
  } else if (strcmp(msgType, "debug_off") == 0) {
    debugLog("Debug mode disabled");
    debugModeEnabled = false;
  } else if (strcmp(msgType, "rfid_enable") == 0) {
    debugLog("RFID enabled");
    rfidEnabled = true;
  } else if (strcmp(msgType, "rfid_disable") == 0) {
    debugLog("RFID disabled");
    rfidEnabled = false;
  } else if (strcmp(msgType, "rfid_prompt") == 0) {
    if (doc["show"] | false) showRfidPromptOnDisplay();
    else hideRfidPromptOnDisplay();
  } else if (strcmp(msgType, "countdown") == 0) {
    uint32_t ms = doc["durationMs"] | 0u;
    if (ms > 0) {
      startCountdownOnDisplay(ms);
      startCountdownLed(ms, doc["color"] | "#ffffff", doc["rainbow"] | false);
    } else {
      stopCountdownOnDisplay();
      stopCountdownLed();
    }
  } else if (strcmp(msgType, "rfid_write") == 0) {
    debugLog("RFID write: " + String(doc["internalId"] | ""));
    handleRfidWrite(doc["internalId"] | "");
  } else if (strcmp(msgType, "state_backup") == 0) {
    // Store { payload, compressed } directly to SPIFFS without intermediate String
    File f = SPIFFS.open("/game_state.json", "w");
    if (f) {
      f.print("{\"payload\":");
      serializeJson(doc["payload"], f);
      f.print(",\"compressed\":");
      serializeJson(doc["compressed"], f);
      f.print("}");
      if (debugModeEnabled) debugLog("Game state backed up (" + String(f.position()) + " bytes)");
      f.close();
    }
  } else if (strcmp(msgType, "github_config_set") == 0) {
    GitHubConfig cfg = {};
    strncpy(cfg.pat,     doc["pat"]     | "", sizeof(cfg.pat)     - 1);
    strncpy(cfg.gist_id, doc["gist_id"] | "", sizeof(cfg.gist_id) - 1);
    cfg.enteredAt = doc["entered_at"] | 0L;
    saveGitHubConfig(cfg);
    // Broadcast to all connected WS client boxes
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (clientHwIds[i] != "" && !clientIsApp[i]) sendToClient(i, doc);
    }
    // Broadcast to ESP-NOW connected boxes
    sendToAllBoxesEspNow(doc);
  } else if (strcmp(msgType, "state_backup_get") == 0) {
    sendStoredStateBackup(clientNum);
  } else if (strcmp(msgType, "state_backup_clear") == 0) {
    if (SPIFFS.exists("/game_state.json")) SPIFFS.remove("/game_state.json");
  } else if (strcmp(msgType, "wifi_credentials_get") == 0) {
    JsonDocument resp;
    resp["type"] = "wifi_credentials";
    JsonArray arr = resp["credentials"].to<JsonArray>();
    for (int i = 0; i < credentialCount; i++) {
      JsonObject cred = arr.add<JsonObject>();
      cred["ssid"] = credentials[i].ssid;
      cred["password"] = credentials[i].password;
    }
    sendToClient(clientNum, resp);
  } else if (strcmp(msgType, "wifi_credentials_set") == 0) {
    JsonArray arr = doc["credentials"].as<JsonArray>();
    credentialCount = 0;
    for (size_t i = 0; i < arr.size() && credentialCount < MAX_CREDENTIALS; i++) {
      credentials[credentialCount].ssid = arr[i]["ssid"].as<String>();
      credentials[credentialCount].password = arr[i]["password"].as<String>();
      credentialCount++;
    }
    saveCredentials();
    // Forward credentials to clients one at a time (ESP-NOW 250-byte limit)
    for (int i = 0; i < credentialCount; i++) {
      JsonDocument cred;
      cred["type"] = "wifi_cred";
      cred["index"] = i;
      cred["total"] = credentialCount;
      cred["ssid"] = credentials[i].ssid;
      cred["password"] = credentials[i].password;
      sendToAllBoxesEspNow(cred);
    }
    JsonDocument ack;
    ack["type"] = "wifi_credentials_ack";
    sendToClient(clientNum, ack);
  }
}

static void routeFromApp(uint8_t clientNum, JsonDocument& doc) {
  const char* targetHwId = doc["hwid"] | "";
  // Messages from the app with no hwid are directed at the hub itself
  if (strlen(targetHwId) == 0) {
    handleHubCommand(clientNum, doc);
    return;
  }

  // "all" — broadcast to every connected box and handle hub's own update
  if (strcmp(targetHwId, "all") == 0) {
    sendToAllBoxesEspNow(doc);
    const char* msgType = doc["type"] | "";
    if (strcmp(msgType, "ota_update") == 0) {
      performOtaUpdate(doc["url"] | "", doc["version"] | "");
    } else if (strncmp(msgType, "led", 3) == 0) {
      handleLedPatternCommand(doc);
    } else if (strcmp(msgType, "display") == 0) {
      handleDisplayCommand(doc);
    }
    return;
  }

  // Hub's own commands
  if (strcmp(targetHwId, myHwId.c_str()) == 0) {
    handleHubCommand(clientNum, doc);
    return;
  }

  // Forward to the target box via ESP-NOW
  sendToBoxEspNow(String(targetHwId), doc);
}

// ---- Hub event handler ----

void onServerEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      debugLog("WS client " + String(clientNum) + " connected, awaiting hello");
      break;

    case WStype_DISCONNECTED:
      onServerDisconnected(clientNum);
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      deserializeJson(doc, payload, length);
      const char* msgType = doc["type"];

      if (strcmp(msgType, "hello") == 0) {
        if (strcmp(doc["client"] | "", "app") == 0) handleHelloFromApp(clientNum);
        else if (strcmp(doc["client"] | "", "box") == 0) handleHelloFromBox(clientNum, doc);
      } else if (!clientIsApp[clientNum]) {
        // Route messages from boxes to app
        String hwId = clientHwIds[clientNum];
        if (hwId != "") { doc["hwid"] = hwId; forwardToApp(doc); }
      } else {
        // Route messages from app
        routeFromApp(clientNum, doc);
      }
      break;
    }

    default:
      break;
  }
}

// ---- Client event handler ----

void onClientEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED: {
      rfidEnabled = false; // app controls RFID state; reset to known baseline on reconnect
      debugLog("Connected to hub, sending hello");
      JsonDocument hello;
      hello["type"] = "hello";
      hello["client"] = "box";
      hello["hwid"] = myHwId;
      hello["version"] = FIRMWARE_VERSION;
      hello["github_config_entered_at"] = getGitHubConfigEnteredAt();
      String out;
      serializeJson(hello, out);
      wsClient.sendTXT(out);
      break;
    }

    case WStype_DISCONNECTED:
      debugLog("Disconnected from hub, will retry...");
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      deserializeJson(doc, payload, length);
      const char* msgType = doc["type"];

      if (strcmp(msgType, "assigned") == 0) {
        debugLog("Confirmed hwid: " + myHwId);
      } else if (strcmp(msgType, "led") == 0) {
        handleLedCommand(doc);
      } else if (strcmp(msgType, "led_anim") == 0) {
        handleLedAnim(doc);
      } else if (strcmp(msgType, "led_anim_stop") == 0) {
        stopLedAnim();
      } else if (strcmp(msgType, "display") == 0) {
        handleDisplayCommand(doc);
      } else if (strcmp(msgType, "ota_update") == 0) {
        performOtaUpdate(doc["url"] | "", doc["version"] | "");
      } else if (strcmp(msgType, "debug_on") == 0) {
        debugModeEnabled = true;
        debugLog("Debug mode enabled");
      } else if (strcmp(msgType, "debug_off") == 0) {
        debugLog("Debug mode disabled");
        debugModeEnabled = false;
      } else if (strcmp(msgType, "rfid_enable") == 0) {
        rfidEnabled = true;
        debugLog("RFID enabled");
      } else if (strcmp(msgType, "rfid_disable") == 0) {
        rfidEnabled = false;
        debugLog("RFID disabled");
      } else if (strcmp(msgType, "rfid_prompt") == 0) {
        if (doc["show"] | false) showRfidPromptOnDisplay();
        else hideRfidPromptOnDisplay();
      } else if (strcmp(msgType, "countdown") == 0) {
        uint32_t ms = doc["durationMs"] | 0u;
        if (ms > 0) {
          startCountdownOnDisplay(ms);
          startCountdownLed(ms, doc["color"] | "#ffffff", doc["rainbow"] | false);
        } else {
          stopCountdownOnDisplay();
          stopCountdownLed();
        }
      } else if (strcmp(msgType, "wifi_credentials_set") == 0) {
        JsonArray arr = doc["credentials"].as<JsonArray>();
        credentialCount = 0;
        for (size_t i = 0; i < arr.size() && credentialCount < MAX_CREDENTIALS; i++) {
          credentials[credentialCount].ssid = arr[i]["ssid"].as<String>();
          credentials[credentialCount].password = arr[i]["password"].as<String>();
          credentialCount++;
        }
        saveCredentials();
        debugLog("WiFi credentials updated");
      } else if (strcmp(msgType, "github_config_set") == 0) {
        GitHubConfig cfg = {};
        strncpy(cfg.pat,     doc["pat"]     | "", sizeof(cfg.pat)     - 1);
        strncpy(cfg.gist_id, doc["gist_id"] | "", sizeof(cfg.gist_id) - 1);
        cfg.enteredAt = doc["entered_at"] | 0L;
        saveGitHubConfig(cfg);
        debugLog("GitHub config updated");
      }
      break;
    }

    default:
      break;
  }
}

// ---- Hub setup ----

void becomeHub() {
  isHub = true;
  initClientTracking();
  Serial.println("Becoming hub");
  wsServer.begin();
  wsServer.onEvent(onServerEvent);
  wsServer.enableHeartbeat(15000, 6000, 2);
  Serial.printf("WebSocket server started on port %d\n", WS_PORT);

  initEspNow();

  if (MDNS.begin(HUB_HOSTNAME)) {
    MDNS.addService("ws", "tcp", WS_PORT);
    Serial.printf("mDNS started — hub reachable at %s.local\n", HUB_HOSTNAME);
  } else {
    Serial.println("mDNS failed — connect via IP");
  }

  Serial.print("Hub IP address: ");
  Serial.println(WiFi.localIP());
}

// ---- Client setup ----

void becomeClient() {
  isHub = false;
  Serial.println("Becoming client — using ESP-NOW for box communication");
  initEspNow();
  sendHelloEspNow();
  WiFi.disconnect(false); // drop AP association but keep radio on for ESP-NOW
  Serial.println("WiFi disconnected (client mode)");
  showClientOnDisplay();
}

// ---- Election ----

void electHub() {
#ifdef FORCE_HUB
  becomeHub();
#elif defined(FORCE_CLIENT)
  becomeClient();
#else
  Serial.println("Checking for existing hub...");
  bool found = false;
  for (int attempt = 0; attempt < 3 && !found; attempt++) {
    if (attempt > 0) delay(600);
    WiFiClient testClient;
    found = testClient.connect((String(HUB_HOSTNAME) + ".local").c_str(), WS_PORT);
    testClient.stop();
  }
  if (found) {
    Serial.println("Hub found!");
    becomeClient();
  } else {
    Serial.println("No hub found after 3 attempts — becoming hub");
    becomeHub();
  }
#endif
}
