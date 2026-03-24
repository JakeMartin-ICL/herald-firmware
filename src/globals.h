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
#include <esp_now.h>
#include "freertos/queue.h"

#ifdef LOCAL_BUILD
#include "secrets.h"
#endif

// ---- Pin defines ----

#define BUTTON_ENDTURN 16
#define BUTTON_PASS    14
#define LED 2

// LED ring (WS2812B)
#define LED_RING_PIN   12
#define LED_RING_COUNT 24

// OLED display (SSD1306 via I2C)
#define OLED_SDA 21
#define OLED_SCL 22

// Battery voltage ADC
#define VBAT_PIN           34
// Adjust to match actual resistor values: ratio = (R_high + R_low) / R_low
// e.g. two equal resistors → 2.0; 100k + 47k → ~3.13
#define VBAT_DIVIDER_RATIO 1.34f  // 6.8k + 20k voltage divider: (6.8+20)/20

// RFID (MFRC522 via VSPI)
#define RFID_SS_PIN   5
#define RFID_RST_PIN  17
#define RFID_SCK_PIN  18
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
const int MAX_CLIENTS = 10;

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
float readBatteryVoltage();
void debugLog(const char* message);
void debugLog(String message);
void sendToHub(JsonDocument& doc);
void setLed(bool on);
void initLedRing();
void handleLedCommand(JsonDocument& doc);
void handleLedAnim(JsonDocument& doc);
void handleLedPatternCommand(JsonDocument& doc);
void stopLedAnim();
void tickLedAnim();
void initDisplay();
void showIpOnDisplay(const char* ip);
void handleDisplayCommand(JsonDocument& doc);
void saveCredentials();

// websockets.cpp
extern int appClientNum;
void forwardToApp(JsonDocument& doc);
void electHub();

// espnow.cpp
void initEspNow();
void sendHelloEspNow();
void sendToBoxEspNow(const String& hwid, JsonDocument& doc);
void sendToAllBoxesEspNow(JsonDocument& doc);
void sendToHubEspNow(JsonDocument& doc);
int getEspNowPeerCount();
const String& getEspNowPeerHwid(int i);
const String& getEspNowPeerVersion(int i);

// rfid.cpp
void initRfid();
void loopRfid();
void handleRfidWrite(const char* internalId);

// ota.cpp
void performOtaUpdate(const char* url, const char* version);
