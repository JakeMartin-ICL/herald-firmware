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
const char* HUB_IP = "192.168.31.233";

bool isHub = false;
int myBoxId = -1;

WebSocketsServer wsServer(WS_PORT);
WebSocketsClient wsClient;

// ---- JSON helpers ----

void sendToHub(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  wsClient.sendTXT(out);
}

void sendToClient(uint8_t clientNum, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  wsServer.sendTXT(clientNum, out);
}

// ---- LED (placeholder) ----

void setLed(bool on) {
  digitalWrite(LED, on ? HIGH : LOW);
}

void handleLedCommand(JsonDocument& doc) {
  const char* pattern = doc["pattern"];
  if (strcmp(pattern, "on") == 0) setLed(true);
  else if (strcmp(pattern, "off") == 0) setLed(false);
}

// ---- Hub: client tracking ----

const int MAX_CLIENTS = 10; // 9 boxes + 1 app
int clientBoxIds[MAX_CLIENTS];
bool clientIsApp[MAX_CLIENTS];
int nextBoxId = 1; // hub itself is box 0, clients start from 1
int appClientNum = -1;

void initClientTracking() {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    clientBoxIds[i] = -1;
    clientIsApp[i] = false;
  }
}

void forwardToApp(JsonDocument& doc) {
  if (appClientNum >= 0) {
    sendToClient(appClientNum, doc);
  } else {
    Serial.println("No app connected, dropping message");
  }
}

// ---- Hub event handler ----

void onServerEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      // Wait for hello message before assigning role
      Serial.printf("WS client %d connected, awaiting hello\n", clientNum);
      break;

    case WStype_DISCONNECTED: {
      if (clientIsApp[clientNum]) {
        Serial.println("App disconnected");
        appClientNum = -1;
        clientIsApp[clientNum] = false;
      } else {
        int boxId = clientBoxIds[clientNum];
        if (boxId >= 0) {
          Serial.printf("Box %d disconnected\n", boxId);
          // Notify app
          JsonDocument notify;
          notify["type"] = "disconnected";
          notify["box"] = boxId;
          forwardToApp(notify);
          clientBoxIds[clientNum] = -1;
        }
      }
      break;
    }

    case WStype_TEXT: {
      JsonDocument doc;
      deserializeJson(doc, payload, length);
      const char* msgType = doc["type"];

      // Handle hello handshake
      if (strcmp(msgType, "hello") == 0) {
        const char* clientType = doc["client"];

        if (strcmp(clientType, "app") == 0) {
          clientIsApp[clientNum] = true;
          appClientNum = clientNum;
          Serial.println("App connected");

          JsonDocument ack;
          ack["type"] = "hello_ack";
          ack["client"] = "hub";
          sendToClient(clientNum, ack);

          // Report hub's own box
          JsonDocument hubBox;
          hubBox["type"] = "connected";
          hubBox["box"] = 0;
          sendToClient(clientNum, hubBox);

          // Report any already-connected client boxes
          for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clientBoxIds[i] >= 0 && !clientIsApp[i]) {
              JsonDocument notify;
              notify["type"] = "connected";
              notify["box"] = clientBoxIds[i];
              sendToClient(clientNum, notify);
            }
          }
        } else if (strcmp(clientType, "box") == 0) {
          // This is a box — assign it an ID
          int boxId = nextBoxId++;
          clientBoxIds[clientNum] = boxId;

          // Send assignment back to box
          JsonDocument assign;
          assign["type"] = "assigned";
          assign["box"] = boxId;
          sendToClient(clientNum, assign);

          // Notify app
          JsonDocument notify;
          notify["type"] = "connected";
          notify["box"] = boxId;
          forwardToApp(notify);

          Serial.printf("Box %d connected (ws client %d)\n", boxId, clientNum);
        }
        break;
      }

      // Route messages from boxes to app
      if (!clientIsApp[clientNum]) {
        int boxId = clientBoxIds[clientNum];
        if (boxId >= 0) {
          // Stamp the box ID and forward to app
          doc["box"] = boxId;
          forwardToApp(doc);
        }
        break;
      }

      // Route messages from app to a specific box
      if (clientIsApp[clientNum]) {
        int targetBox = doc["box"] | -1;
        if (targetBox < 0) break;

          // If the command is for the hub itself, handle locally
        if (targetBox == 0) {
          if (strcmp(msgType, "led") == 0) {
            handleLedCommand(doc);
          }
          break;
        }

        // Find the ws clientNum for this box ID
        for (int i = 0; i < MAX_CLIENTS; i++) {
          if (clientBoxIds[i] == targetBox) {
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
      sendToHub(hello);
      break;
    }

    case WStype_DISCONNECTED:
      Serial.println("Disconnected from hub");
      myBoxId = -1;
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      deserializeJson(doc, payload, length);
      const char* msgType = doc["type"];

      if (strcmp(msgType, "assigned") == 0) {
        myBoxId = doc["box"];
        Serial.printf("Assigned box ID: %d\n", myBoxId);
      } else if (strcmp(msgType, "led") == 0) {
        handleLedCommand(doc);
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
  myBoxId = 0;
  initClientTracking();
  Serial.println("Becoming hub");
  wsServer.begin();
  wsServer.onEvent(onServerEvent);
  Serial.printf("WebSocket server started on port %d\n", WS_PORT);

  if (MDNS.begin("herald")) {
    MDNS.addService("ws", "tcp", WS_PORT);
    Serial.println("mDNS started — hub reachable at herald.local");
  } else {
    Serial.println("mDNS failed — connect via IP");
  }

  Serial.print("Hub IP address: ");
  Serial.println(WiFi.localIP());
}

// ---- Client setup ----

void becomeClient() {
  isHub = false;
  Serial.printf("Connecting to hub at %s:%d\n", HUB_IP, WS_PORT);
  wsClient.begin(HUB_IP, WS_PORT, "/");
  wsClient.onEvent(onClientEvent);
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
  if (testClient.connect(HUB_IP, WS_PORT)) {
    testClient.stop();
    becomeClient();
  } else {
    Serial.println("No hub found");
    becomeHub();
  }
#endif
}

// ---- Button handling ----

bool lastEndTurnState = HIGH;

void handleButtons() {
  bool endTurnState = digitalRead(BUTTON_ENDTURN);

  if (endTurnState == LOW && lastEndTurnState == HIGH) {
    setLed(true);
    delay(100);
    setLed(false);

    JsonDocument doc;
    doc["type"] = "endturn";
    doc["box"] = myBoxId;

    if (isHub) {
      // Hub's own button — forward directly to app
      forwardToApp(doc);
    } else {
      sendToHub(doc);
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
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

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