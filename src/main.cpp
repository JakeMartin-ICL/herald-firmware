#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "secrets.h"

#define BUTTON_ENDTURN 18
#define LED 2

const int WS_PORT = 8765;
const char* HUB_HOSTNAME = "herald";
const int RECONNECT_INTERVAL_MS = 5000;

bool isHub = false;
String myHwId = "";

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
bool clientIsApp[MAX_CLIENTS];
int appClientNum = -1;

void initClientTracking() {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    clientHwIds[i] = "";
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

// ---- Hub event handler ----

void onServerEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      Serial.printf("WS client %d connected, awaiting hello\n", clientNum);
      break;

    case WStype_DISCONNECTED: {
      if (clientIsApp[clientNum]) {
        Serial.println("App disconnected");
        appClientNum = -1;
        clientIsApp[clientNum] = false;
      } else if (clientHwIds[clientNum] != "") {
        String hwId = clientHwIds[clientNum];
        Serial.printf("Box %s disconnected\n", hwId.c_str());
        JsonDocument notify;
        notify["type"] = "disconnected";
        notify["hwid"] = hwId;
        forwardToApp(notify);
        clientHwIds[clientNum] = "";
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
          Serial.println("App connected");

          // Acknowledge
          JsonDocument ack;
          ack["type"] = "hello_ack";
          ack["client"] = "hub";
          sendToClient(clientNum, ack);

          // Report hub itself
          JsonDocument hubBox;
          hubBox["type"] = "connected";
          hubBox["hwid"] = myHwId;
          sendToClient(clientNum, hubBox);

          // Report already connected boxes
          for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clientHwIds[i] != "" && !clientIsApp[i]) {
              JsonDocument notify;
              notify["type"] = "connected";
              notify["hwid"] = clientHwIds[i];
              sendToClient(clientNum, notify);
            }
          }

        } else if (strcmp(clientType, "box") == 0) {
          const char* hwId = doc["hwid"];
          clientHwIds[clientNum] = hwId;

          // Send assignment ack back to box
          JsonDocument assign;
          assign["type"] = "assigned";
          assign["hwid"] = hwId;
          sendToClient(clientNum, assign);

          // Notify app
          JsonDocument notify;
          notify["type"] = "connected";
          notify["hwid"] = hwId;
          forwardToApp(notify);

          Serial.printf("Box %s connected (ws client %d)\n", hwId, clientNum);
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

      // Route messages from app to a specific box
      if (clientIsApp[clientNum]) {
        const char* targetHwId = doc["hwid"] | "";
        if (strlen(targetHwId) == 0) break;

        // Handle hub's own LED commands locally
        if (strcmp(targetHwId, myHwId.c_str()) == 0) {
          const char* msgTypeInner = doc["type"] | "";
          if (strcmp(msgTypeInner, "led") == 0) {
            handleLedCommand(doc);
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
      Serial.println("Connected to hub, sending hello");
      JsonDocument hello;
      hello["type"] = "hello";
      hello["client"] = "box";
      hello["hwid"] = myHwId;
      String out;
      serializeJson(hello, out);
      wsClient.sendTXT(out);
      break;
    }

    case WStype_DISCONNECTED:
      Serial.println("Disconnected from hub, will retry...");
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      deserializeJson(doc, payload, length);
      const char* msgType = doc["type"];

      if (strcmp(msgType, "assigned") == 0) {
        Serial.printf("Confirmed hwid: %s\n", myHwId.c_str());
      } else if (strcmp(msgType, "led") == 0) {
        handleLedCommand(doc);
      }
      break;
    }

    default:
      break;
  }
}

// ---- Send helpers ----

void sendToHub(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  wsClient.sendTXT(out);
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
  if (testClient.connect(String(HUB_HOSTNAME) + ".local", WS_PORT)) {
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
  bool endTurnState = digitalRead(BUTTON_ENDTURN);

  if (endTurnState == LOW && lastEndTurnState == HIGH) {
    // Button just pressed
    endTurnPressTime = millis();
    longPressHandled = false;
  }

  if (endTurnState == LOW && !longPressHandled) {
    if (millis() - endTurnPressTime >= LONG_PRESS_MS) {
      // Long press detected
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
    // Button released
    if (!longPressHandled) {
      // Short press
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
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected!");
  myHwId = getHwId();
  Serial.printf("Hardware ID: %s\n", myHwId.c_str());

  delay(500);
  electHub();
}

// ---- Loop ----

void loop() {
  if (isHub) {
    wsServer.loop();
  } else {
    wsClient.loop();
  }

  handleButtons();
}