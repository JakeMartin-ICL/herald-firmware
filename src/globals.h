#pragma once

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
#include <SPIFFS.h>
#include <SPI.h>
#include <MFRC522.h>
#include "freertos/queue.h"

#ifdef LOCAL_BUILD
#include "secrets.h"
#endif

// ---- Pin defines ----

#define BUTTON_ENDTURN 18
#define LED 2

// RFID (MFRC522 via VSPI with custom SCK to avoid conflict with pin 18)
#define RFID_SS_PIN   21
#define RFID_RST_PIN  22
#define RFID_SCK_PIN  14
#define RFID_MISO_PIN 19
#define RFID_MOSI_PIN 23

// ---- Firmware version ----

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// ---- Constants ----

const int WS_PORT = 8765;
const char* const HUB_HOSTNAME = "herald";
const int RECONNECT_INTERVAL_MS = 5000;
const int MAX_CREDENTIALS = 10;

// ---- Structs ----

struct WifiCredential {
  String ssid;
  String password;
};

struct OtaArgs {
  char url[512];
  char version[64];
};

// ---- Shared globals (defined in main.cpp) ----

extern bool isHub;
extern String myHwId;
extern bool debugModeEnabled;
extern bool otaInProgress;
extern bool rfidEnabled;
extern volatile bool otaComplete;
extern char otaCompleteVersion[64];
extern QueueHandle_t otaProgressQueue;
extern int otaLastPercent;
extern WebSocketsServer wsServer;
extern WebSocketsClient wsClient;
extern WifiCredential credentials[MAX_CREDENTIALS];
extern int credentialCount;

// ---- Cross-file function prototypes ----

// main.cpp
void debugLog(const char* message);
void debugLog(String message);
void sendToHub(JsonDocument& doc);
void setLed(bool on);
void handleLedCommand(JsonDocument& doc);
void saveCredentials();

// websockets.cpp
extern int appClientNum;
void forwardToApp(JsonDocument& doc);
void electHub();

// rfid.cpp
void initRfid();
void loopRfid();
void handleRfidWrite(const char* internalId);

// ota.cpp
void performOtaUpdate(const char* url, const char* version);
