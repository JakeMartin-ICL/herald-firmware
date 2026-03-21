#include "globals.h"

// ---- Shared global definitions ----

bool isHub = false;
String myHwId = "";
bool debugModeEnabled = false;
bool otaInProgress = false;
bool rfidEnabled = false;
volatile bool otaComplete = false;
char otaCompleteVersion[64] = "";
QueueHandle_t otaProgressQueue = NULL;
int otaLastPercent = -1;

WebSocketsServer wsServer(WS_PORT);
WebSocketsClient wsClient;

WifiCredential credentials[MAX_CREDENTIALS];
int credentialCount = 0;

// ---- WiFi credentials ----

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

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed — state backup unavailable");
  }

  initRfid();

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
