#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPUpdate.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include "freertos/queue.h"

#ifdef LOCAL_BUILD
#include "secrets.h"
#endif

#define BUTTON_ENDTURN 18
#define LED 2

// RFID (MFRC522 via VSPI with custom SCK to avoid conflict with pin 18)
// Adjust these to match your wiring.
#define RFID_SS_PIN   21
#define RFID_RST_PIN  22
#define RFID_SCK_PIN  14
#define RFID_MISO_PIN 19
#define RFID_MOSI_PIN 23

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

const int WS_PORT = 8765;
const char* HUB_HOSTNAME = "herald";
const int RECONNECT_INTERVAL_MS = 5000;
const int MAX_CREDENTIALS = 10;

// ---- WiFi credentials ----

struct WifiCredential {
  String ssid;
  String password;
};

WifiCredential credentials[MAX_CREDENTIALS];
int credentialCount = 0;

void saveCredentials() {
  Preferences prefs;
  prefs.begin("herald", false);
  prefs.putInt("wifiCount", credentialCount);
  for (int i = 0; i < credentialCount; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), credentials[i].ssid);
    prefs.putString(("pwd" + String(i)).c_str(), credentials[i].password);
  }
  prefs.end();
}

void loadCredentials() {
  Preferences prefs;
  prefs.begin("herald", true);
  credentialCount = prefs.getInt("wifiCount", 0);
  for (int i = 0; i < credentialCount; i++) {
    credentials[i].ssid = prefs.getString(("ssid" + String(i)).c_str(), "");
    credentials[i].password = prefs.getString(("pwd" + String(i)).c_str(), "");
  }
  prefs.end();

#ifdef LOCAL_BUILD
  if (credentialCount == 0) {
    credentials[0].ssid = WIFI_SSID;
    credentials[0].password = WIFI_PASSWORD;
    credentialCount = 1;
    saveCredentials(); // bootstrap NVS from secrets.h
  }
#endif
}

bool connectWifi() {
  for (int i = 0; i < credentialCount; i++) {
    Serial.printf("Trying WiFi: %s\n", credentials[i].ssid.c_str());
    WiFi.begin(credentials[i].ssid.c_str(), credentials[i].password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected to %s\n", credentials[i].ssid.c_str());
      return true;
    }
    WiFi.disconnect();
    Serial.println();
  }
  return false;
}

bool isHub = false;
String myHwId = "";
bool debugModeEnabled = false;
bool otaInProgress = false;
bool rfidEnabled = false;
volatile bool otaComplete = false;
char otaCompleteVersion[64] = "";
QueueHandle_t otaProgressQueue = NULL;
int otaLastPercent = -1;

struct OtaArgs {
  char url[512];
  char version[64];
};

WebSocketsServer wsServer(WS_PORT);
WebSocketsClient wsClient;

// ---- Hardware ID ----

String getHwId() {
  return WiFi.macAddress();
}

// ---- LED (placeholder) ----

void setLed(bool on) {
  digitalWrite(LED, on ? HIGH : LOW);
}

void handleLedCommand(JsonDocument& doc) {
  const char* pattern = doc["pattern"] | "";
  if (strcmp(pattern, "on") == 0) setLed(true);
  else if (strcmp(pattern, "off") == 0) setLed(false);
  // leds array handling will go here when we have NeoPixel hardware
}

// ---- Hub: client tracking ----

const int MAX_CLIENTS = 10;
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

// ---- Debug logging ----

void debugLog(const char* message) {
  Serial.println(message);
  if (debugModeEnabled) {
    JsonDocument doc;
    doc["type"] = "debug";
    doc["hwid"] = myHwId;
    doc["msg"] = message;
    if (isHub) {
      forwardToApp(doc);
    } else {
      String out;
      serializeJson(doc, out);
      wsClient.sendTXT(out);
    }
  }
}

void debugLog(String message) {
  debugLog(message.c_str());
}

// ---- Send helpers ----

void sendToHub(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  wsClient.sendTXT(out);
}

// ---- RFID ----

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
MFRC522::MIFARE_Key rfidKey;

// Authenticate sector 0 with the default factory key (all 0xFF).
static bool rfidAuth() {
  for (byte i = 0; i < 6; i++) rfidKey.keyByte[i] = 0xFF;
  MFRC522::StatusCode s = rfid.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A, 3, &rfidKey, &rfid.uid);
  if (s != MFRC522::STATUS_OK) {
    Serial.print("RFID auth failed: ");
    Serial.println(rfid.GetStatusCodeName(s));
  }
  return s == MFRC522::STATUS_OK;
}

// Read up to 32 bytes from blocks 1–2 of sector 0 into `out`.
// Returns false if auth or any read fails.
static bool rfidReadContent(String& out) {
  if (!rfidAuth()) return false;

  char raw[33] = {};
  byte buf[18];
  byte bufSize;

  bufSize = sizeof(buf);
  if (rfid.MIFARE_Read(1, buf, &bufSize) == MFRC522::STATUS_OK)
    memcpy(raw, buf, 16);

  bufSize = sizeof(buf);
  if (rfid.MIFARE_Read(2, buf, &bufSize) == MFRC522::STATUS_OK)
    memcpy(raw + 16, buf, 16);

  raw[32] = '\0';
  out = String(raw);
  int nullPos = out.indexOf('\0');
  if (nullPos >= 0) out = out.substring(0, nullPos);
  return true;
}

// Write a null-terminated string into blocks 1–2 of sector 0 (max 31 chars).
static bool rfidWriteContent(const char* content) {
  if (!rfidAuth()) return false;

  byte block1[16] = {};
  byte block2[16] = {};
  size_t len = min(strlen(content), (size_t)31);
  if (len <= 16) {
    memcpy(block1, content, len);
  } else {
    memcpy(block1, content, 16);
    memcpy(block2, content + 16, len - 16);
  }

  if (rfid.MIFARE_Write(1, block1, 16) != MFRC522::STATUS_OK) {
    Serial.println("RFID: write block 1 failed");
    rfid.PCD_StopCrypto1();
    return false;
  }
  if (rfid.MIFARE_Write(2, block2, 16) != MFRC522::STATUS_OK) {
    Serial.println("RFID: write block 2 failed");
    rfid.PCD_StopCrypto1();
    return false;
  }
  rfid.PCD_StopCrypto1();
  return true;
}

// Called from loop() — detect new tags, read their content, forward as rfid message.
void loopRfid() {
  if (!rfidEnabled) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String content;
  bool ok = rfidReadContent(content);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (!ok) return;

  if (content.length() == 0) {
    Serial.println("RFID: blank tag, ignoring");
    return;
  }
  if (content.indexOf(':') < 0) {
    Serial.printf("RFID: unrecognised format: %s\n", content.c_str());
    return;
  }

  debugLog("RFID tag read: " + content);
  JsonDocument doc;
  doc["type"] = "rfid";
  doc["hwid"] = myHwId;
  doc["tagId"] = content;
  if (isHub) forwardToApp(doc);
  else sendToHub(doc);
}

// Called when the hub receives an rfid_write command from the app.
// Wakes any card in the field (including halted ones) and writes the internalId string.
void handleRfidWrite(const char* internalId) {
  byte atqa[2];
  byte atqaSize = sizeof(atqa);

  // WUPA wakes cards in halt state; fall back to REQA for freshly presented cards
  bool cardPresent = (rfid.PICC_WakeupA(atqa, &atqaSize) == MFRC522::STATUS_OK)
                  || rfid.PICC_IsNewCardPresent();

  JsonDocument resp;
  resp["type"] = "rfid_write_result";
  resp["hwid"] = myHwId;

  if (!cardPresent || !rfid.PICC_ReadCardSerial()) {
    resp["success"] = false;
    resp["error"] = "No card in field";
    forwardToApp(resp);
    return;
  }

  bool ok = rfidWriteContent(internalId);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  resp["success"] = ok;
  if (!ok) resp["error"] = "Write failed";
  forwardToApp(resp);
}

// ---- OTA ----

void otaProgressCallback(int current, int total) {
  if (total <= 0) return;
  int percent = (current * 100) / total;
  if (percent == otaLastPercent) return;
  otaLastPercent = percent;

  if (otaProgressQueue) {
    xQueueSend(otaProgressQueue, &percent, 0);
  }
}

// Client OTA task — runs in background so main loop keeps calling wsClient.loop(),
// draining the receive buffer naturally without re-entrant loop() calls.
void otaTaskFn(void* param) {
  OtaArgs* args = (OtaArgs*)param;

  WiFiClientSecure wifiClient;
  wifiClient.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.onProgress(otaProgressCallback);

  t_httpUpdate_return ret = httpUpdate.update(wifiClient, args->url);
  delete args;

  if (ret == HTTP_UPDATE_OK) {
    strncpy(otaCompleteVersion, args->version, sizeof(otaCompleteVersion) - 1);
    otaComplete = true; // main loop handles notification, clean disconnect, and restart
  } else {
    Serial.printf("OTA failed: %s\n", httpUpdate.getLastErrorString().c_str());
    if (otaProgressQueue) {
      vQueueDelete(otaProgressQueue);
      otaProgressQueue = NULL;
    }
    otaInProgress = false;
  }

  vTaskDelete(NULL);
}

void performOtaUpdate(const char* url, const char* version) {
  if (otaInProgress) return;
  otaInProgress = true;
  otaLastPercent = -1;

  debugLog("OTA: starting update from " + String(url));

  // Run OTA in a background task for both hub and client so the main loop keeps
  // running (wsServer/wsClient.loop()), responding to pings and draining receive buffers.
  otaProgressQueue = xQueueCreate(20, sizeof(int));
  OtaArgs* args = new OtaArgs();
  strncpy(args->url, url, sizeof(args->url) - 1);
  args->url[sizeof(args->url) - 1] = '\0';
  strncpy(args->version, version, sizeof(args->version) - 1);
  args->version[sizeof(args->version) - 1] = '\0';
  xTaskCreate(otaTaskFn, "ota", 16384, args, 1, NULL);
}

// ---- Hub event handler ----

void onServerEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      debugLog("WS client " + String(clientNum) + " connected, awaiting hello");
      break;

    case WStype_DISCONNECTED: {
      if (clientIsApp[clientNum]) {
        debugLog("App disconnected");
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
      break;
    }

    case WStype_TEXT: {
      JsonDocument doc;
      deserializeJson(doc, payload, length);
      const char* msgType = doc["type"];

      if (strcmp(msgType, "hello") == 0) {
        const char* clientType = doc["client"];

        if (strcmp(clientType, "app") == 0) {
          clientIsApp[clientNum] = true;
          appClientNum = clientNum;
          rfidEnabled = false; // app controls RFID state; reset to known baseline on connect
          debugLog("App connected");

          // Acknowledge with version
          JsonDocument ack;
          ack["type"] = "hello_ack";
          ack["client"] = "hub";
          ack["version"] = FIRMWARE_VERSION;
          sendToClient(clientNum, ack);

          // Report hub itself
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

        } else if (strcmp(clientType, "box") == 0) {
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

          // Send assignment ack back to box
          JsonDocument assign;
          assign["type"] = "assigned";
          assign["hwid"] = hwId;
          sendToClient(clientNum, assign);

          // Notify app (with version)
          JsonDocument notify;
          notify["type"] = "connected";
          notify["hwid"] = hwId;
          notify["version"] = version;
          forwardToApp(notify);

          debugLog("Box " + String(hwId) + " connected (ws client " + String(clientNum) + ")");
        }
        break;
      }

      // Route messages from boxes to app
      if (!clientIsApp[clientNum]) {
        String hwId = clientHwIds[clientNum];
        if (hwId != "") {
          doc["hwid"] = hwId;
          forwardToApp(doc);
        }
        break;
      }

      // Route messages from app to boxes
      if (clientIsApp[clientNum]) {
        const char* targetHwId = doc["hwid"] | "";
        if (strlen(targetHwId) == 0) break;

        // "all" — broadcast OTA to every connected box and handle hub's own update
        if (strcmp(targetHwId, "all") == 0) {
          const char* msgTypeInner = doc["type"] | "";
          if (strcmp(msgTypeInner, "ota_update") == 0) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
              if (clientHwIds[i] != "" && !clientIsApp[i]) {
                sendToClient(i, doc);
              }
            }
            performOtaUpdate(doc["url"] | "", doc["version"] | "");
          }
          break;
        }

        // Handle hub's own commands locally
        if (strcmp(targetHwId, myHwId.c_str()) == 0) {
          const char* msgTypeInner = doc["type"] | "";
          if (strcmp(msgTypeInner, "led") == 0) {
            handleLedCommand(doc);
          } else if (strcmp(msgTypeInner, "ota_update") == 0) {
            performOtaUpdate(doc["url"] | "", doc["version"] | "");
          } else if (strcmp(msgTypeInner, "debug_on") == 0) {
            debugModeEnabled = true;
            debugLog("Debug mode enabled");
          } else if (strcmp(msgTypeInner, "debug_off") == 0) {
            debugLog("Debug mode disabled");
            debugModeEnabled = false;
          } else if (strcmp(msgTypeInner, "rfid_enable") == 0) {
            rfidEnabled = true;
          } else if (strcmp(msgTypeInner, "rfid_disable") == 0) {
            rfidEnabled = false;
          } else if (strcmp(msgTypeInner, "rfid_write") == 0) {
            handleRfidWrite(doc["internalId"] | "");
          } else if (strcmp(msgTypeInner, "wifi_credentials_get") == 0) {
            JsonDocument resp;
            resp["type"] = "wifi_credentials";
            JsonArray arr = resp["credentials"].to<JsonArray>();
            for (int i = 0; i < credentialCount; i++) {
              JsonObject cred = arr.add<JsonObject>();
              cred["ssid"] = credentials[i].ssid;
              cred["password"] = credentials[i].password;
            }
            sendToClient(clientNum, resp);
          } else if (strcmp(msgTypeInner, "wifi_credentials_set") == 0) {
            JsonArray arr = doc["credentials"].as<JsonArray>();
            credentialCount = 0;
            for (size_t i = 0; i < arr.size() && credentialCount < MAX_CREDENTIALS; i++) {
              credentials[credentialCount].ssid = arr[i]["ssid"].as<String>();
              credentials[credentialCount].password = arr[i]["password"].as<String>();
              credentialCount++;
            }
            saveCredentials();
            // Forward to all connected clients
            for (int i = 0; i < MAX_CLIENTS; i++) {
              if (clientHwIds[i] != "" && !clientIsApp[i]) {
                sendToClient(i, doc);
              }
            }
            // Acknowledge to app
            JsonDocument ack;
            ack["type"] = "wifi_credentials_ack";
            sendToClient(clientNum, ack);
          }
          break;
        }

        // Forward to correct box
        for (int i = 0; i < MAX_CLIENTS; i++) {
          if (clientHwIds[i] == targetHwId) {
            sendToClient(i, doc);
            break;
          }
        }
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
      } else if (strcmp(msgType, "rfid_disable") == 0) {
        rfidEnabled = false;
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
  Serial.printf("WebSocket server started on port %d\n", WS_PORT);

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
  Serial.printf("Connecting to hub at %s.local:%d\n", HUB_HOSTNAME, WS_PORT);
  wsClient.begin(String(HUB_HOSTNAME) + ".local", WS_PORT, "/");
  wsClient.onEvent(onClientEvent);
  wsClient.setReconnectInterval(RECONNECT_INTERVAL_MS);
}

// ---- Election ----

void electHub() {
#ifdef FORCE_HUB
  becomeHub();
#elif defined(FORCE_CLIENT)
  becomeClient();
#else
  WiFiClient testClient;
  Serial.println("Trying to find hub...");
  if (testClient.connect((String(HUB_HOSTNAME) + ".local").c_str(), WS_PORT)) {
    testClient.stop();
    Serial.println("Hub found!");
    becomeClient();
  } else {
    Serial.println("No hub found");
    becomeHub();
  }
#endif
}

// ---- Button handling ----

bool lastEndTurnState = HIGH;
unsigned long endTurnPressTime = 0;
bool longPressHandled = false;
const unsigned long LONG_PRESS_MS = 2000;

void handleButtons() {
  if (otaInProgress) return;

  bool endTurnState = digitalRead(BUTTON_ENDTURN);

  if (endTurnState == LOW && lastEndTurnState == HIGH) {
    endTurnPressTime = millis();
    longPressHandled = false;
  }

  if (endTurnState == LOW && !longPressHandled) {
    if (millis() - endTurnPressTime >= LONG_PRESS_MS) {
      longPressHandled = true;
      setLed(true);
      delay(100);
      setLed(false);

      JsonDocument doc;
      doc["type"] = "longpress";
      doc["hwid"] = myHwId;

      if (isHub) {
        forwardToApp(doc);
      } else {
        sendToHub(doc);
      }
    }
  }

  if (endTurnState == HIGH && lastEndTurnState == LOW) {
    if (!longPressHandled) {
      setLed(true);
      delay(100);
      setLed(false);

      JsonDocument doc;
      doc["type"] = "endturn";
      doc["hwid"] = myHwId;

      if (isHub) {
        forwardToApp(doc);
      } else {
        sendToHub(doc);
      }
    }
  }

  lastEndTurnState = endTurnState;
}

// ---- Setup ----

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  pinMode(BUTTON_ENDTURN, INPUT_PULLUP);

  Serial.println("Herald booting...");
  Serial.printf("Firmware version: %s\n", FIRMWARE_VERSION);

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();
  Serial.println("RFID reader initialised");

  loadCredentials();

  if (!connectWifi()) {
    Serial.println("Failed to connect to any WiFi network — halting");
    while (true) { delay(1000); }
  }

  Serial.println("WiFi connected!");
  myHwId = getHwId();
  Serial.printf("Hardware ID: %s\n", myHwId.c_str());

  delay(500);
  electHub();
}

// ---- Loop ----

void loop() {
  loopRfid();

  if (isHub) {
    wsServer.loop();

    if (otaProgressQueue) {
      int percent;
      if (xQueueReceive(otaProgressQueue, &percent, 0) == pdTRUE) {
        JsonDocument doc;
        doc["type"] = "ota_progress";
        doc["hwid"] = myHwId;
        doc["percent"] = percent;
        forwardToApp(doc);
      }
    }

    if (otaComplete) {
      JsonDocument doc;
      doc["type"] = "ota_complete";
      doc["hwid"] = myHwId;
      doc["version"] = otaCompleteVersion;
      forwardToApp(doc);
      delay(200); // let ota_complete be sent
      if (appClientNum >= 0) wsServer.disconnect(appClientNum); // clean close so app detects disconnect immediately
      delay(200); // let close frame be sent
      ESP.restart();
    }
  } else {
    if (otaComplete) {
      wsClient.disconnect();
      delay(200);
      ESP.restart();
    }

    wsClient.loop();

    if (otaProgressQueue) {
      int percent;
      if (xQueueReceive(otaProgressQueue, &percent, 0) == pdTRUE) {
        JsonDocument doc;
        doc["type"] = "ota_progress";
        doc["hwid"] = myHwId;
        doc["percent"] = percent;
        sendToHub(doc);
      }
    }
  }

  handleButtons();
}
