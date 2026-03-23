#include "globals.h"
#include <FastLED.h>

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

// ---- Battery voltage ----

float readBatteryVoltage() {
  // analogReadMilliVolts uses ESP32 factory eFuse calibration — no hardcoded
  // full-scale reference needed, which varies chip to chip with 11dB attenuation.
  float vAdc = analogReadMilliVolts(VBAT_PIN) / 1000.0f;
  return vAdc * VBAT_DIVIDER_RATIO;
}

// ---- Onboard LED ----

void setLed(bool on) {
  digitalWrite(LED, on ? HIGH : LOW);
}

// ---- LED ring ----

static CRGB ledRing[LED_RING_COUNT];

// Gamma-2.2 lookup table: converts linear sRGB (0–255) to LED PWM (0–255).
// WS2812B output is linear; sRGB colours assume a ~2.2 gamma display.
// Without this, intermediate colours (orange, pink, etc.) look wrong.
static const uint8_t GAMMA8[256] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
    5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
   10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
   17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
   25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
   37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
   51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
   69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
   90, 92, 93, 95, 96, 98, 99,101,102,104,105,107,109,110,112,114,
  115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
  144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
  177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
  215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255,
};

static uint8_t hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

static void parseHexColorRaw(const char* hex, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!hex || hex[0] != '#' || strlen(hex) < 7) { r = g = b = 0; return; }
  r = (hexVal(hex[1]) << 4) | hexVal(hex[2]);
  g = (hexVal(hex[3]) << 4) | hexVal(hex[4]);
  b = (hexVal(hex[5]) << 4) | hexVal(hex[6]);
}

static CRGB parseHexColor(const char* hex) {
  uint8_t r, g, b;
  parseHexColorRaw(hex, r, g, b);
  return CRGB(GAMMA8[r], GAMMA8[g], GAMMA8[b]);
}

void initLedRing() {
  FastLED.addLeds<WS2812B, LED_RING_PIN, GRB>(ledRing, LED_RING_COUNT);
  FastLED.clear(true);
}

void handleLedCommand(JsonDocument& doc) {
  stopLedAnim();
  JsonArray arr = doc["leds"].as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) return;
  for (size_t i = 0; i < arr.size() && i < LED_RING_COUNT; i++) {
    ledRing[i] = parseHexColor(arr[i] | "#000000");
  }
  FastLED.show();
}

// ---- LED animation ----

#define MAX_ANIM_FRAMES 32
#define ANIM_FADE_INTERVAL_MS 16  // ~60fps for fade interpolation

struct AnimFrame {
  uint8_t r[LED_RING_COUNT];
  uint8_t g[LED_RING_COUNT];
  uint8_t b[LED_RING_COUNT];
  uint32_t ms;
  bool fade;
};

static AnimFrame animFrames[MAX_ANIM_FRAMES];
static int  animFrameCount   = 0;
static int  animFrameIndex   = -1;  // -1 = not running
static int  animLoops        = 0;   // -1=infinite, >0=plays remaining
static uint32_t animFrameStart  = 0;
static uint32_t animLastFadeMs  = 0;
static uint8_t  prevR[LED_RING_COUNT];
static uint8_t  prevG[LED_RING_COUNT];
static uint8_t  prevB[LED_RING_COUNT];

static void showAnimFrame(int idx) {
  const AnimFrame& f = animFrames[idx];
  for (int i = 0; i < LED_RING_COUNT; i++) {
    ledRing[i] = CRGB(GAMMA8[f.r[i]], GAMMA8[f.g[i]], GAMMA8[f.b[i]]);
  }
  FastLED.show();
}

static void advanceAnimFrame() {
  // Save current frame colours as previous (for next fade)
  const AnimFrame& cur = animFrames[animFrameIndex];
  for (int i = 0; i < LED_RING_COUNT; i++) {
    prevR[i] = cur.r[i]; prevG[i] = cur.g[i]; prevB[i] = cur.b[i];
  }

  animFrameIndex++;
  if (animFrameIndex >= animFrameCount) {
    if (animLoops == -1) {
      animFrameIndex = 0;          // loop forever
    } else if (animLoops > 1) {
      animLoops--;
      animFrameIndex = 0;
    } else {
      stopLedAnim();
      return;
    }
  }

  animFrameStart = millis();
  if (!animFrames[animFrameIndex].fade) showAnimFrame(animFrameIndex);
}

void stopLedAnim() {
  animFrameIndex = -1;
}

void handleLedAnim(JsonDocument& doc) {
  stopLedAnim();

  // Parse loop setting: true=infinite, false/absent=once, N=N times
  JsonVariant loopVal = doc["loop"];
  if (loopVal.is<bool>()) {
    animLoops = loopVal.as<bool>() ? -1 : 1;
  } else if (loopVal.is<int>()) {
    int n = loopVal.as<int>();
    animLoops = (n <= 0) ? 1 : n;
  } else {
    animLoops = 1;
  }

  // Parse frames
  JsonArray frames = doc["frames"].as<JsonArray>();
  animFrameCount = 0;
  for (JsonObject frame : frames) {
    if (animFrameCount >= MAX_ANIM_FRAMES) break;
    AnimFrame& af = animFrames[animFrameCount];
    af.ms   = frame["ms"]   | 100;
    af.fade = frame["fade"] | false;
    JsonArray leds = frame["leds"].as<JsonArray>();
    for (int i = 0; i < LED_RING_COUNT; i++) {
      const char* hex = (i < (int)leds.size()) ? (leds[i] | "#000000") : "#000000";
      parseHexColorRaw(hex, af.r[i], af.g[i], af.b[i]);
    }
    animFrameCount++;
  }

  if (animFrameCount == 0) return;

  // Initialise prev colours to black so first fade starts from off
  memset(prevR, 0, sizeof(prevR));
  memset(prevG, 0, sizeof(prevG));
  memset(prevB, 0, sizeof(prevB));

  animFrameIndex = 0;
  animFrameStart = millis();
  animLastFadeMs = 0;
  if (!animFrames[0].fade) showAnimFrame(0);
}

void tickLedAnim() {
  if (animFrameIndex < 0 || animFrameCount == 0) return;

  uint32_t now     = millis();
  uint32_t elapsed = now - animFrameStart;
  const AnimFrame& frame = animFrames[animFrameIndex];

  if (elapsed >= frame.ms) {
    advanceAnimFrame();
    return;
  }

  if (frame.fade && (now - animLastFadeMs) >= ANIM_FADE_INTERVAL_MS) {
    // Linear interpolation in sRGB, gamma applied at display
    float t = (float)elapsed / (float)frame.ms;
    for (int i = 0; i < LED_RING_COUNT; i++) {
      uint8_t r = (uint8_t)(prevR[i] + t * ((float)frame.r[i] - prevR[i]));
      uint8_t g = (uint8_t)(prevG[i] + t * ((float)frame.g[i] - prevG[i]));
      uint8_t b = (uint8_t)(prevB[i] + t * ((float)frame.b[i] - prevB[i]));
      ledRing[i] = CRGB(GAMMA8[r], GAMMA8[g], GAMMA8[b]);
    }
    FastLED.show();
    animLastFadeMs = now;
  }
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

bool lastPassState = HIGH;

static void sendButtonEvent(const char* type) {
  setLed(true);
  delay(100);
  setLed(false);

  JsonDocument doc;
  doc["type"] = type;
  doc["hwid"] = myHwId;

  if (isHub) {
    forwardToApp(doc);
  } else {
    sendToHub(doc);
  }
}

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
      sendButtonEvent("longpress");
    }
  }

  if (endTurnState == HIGH && lastEndTurnState == LOW) {
    if (!longPressHandled) {
      sendButtonEvent("endturn");
    }
  }

  lastEndTurnState = endTurnState;

  // Pass button — simple press only
  bool passState = digitalRead(BUTTON_PASS);
  if (passState == HIGH && lastPassState == LOW) {
    sendButtonEvent("pass");
  }
  lastPassState = passState;
}

// ---- Setup ----

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  pinMode(BUTTON_ENDTURN, INPUT_PULLUP);
  pinMode(BUTTON_PASS, INPUT_PULLUP);
  analogSetPinAttenuation(VBAT_PIN, ADC_11db);
  initDisplay();
  initLedRing();

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

  tickLedAnim();
  handleButtons();

  // Report battery voltage every 60 seconds
  static unsigned long lastBatteryMs = 0;
  if (millis() - lastBatteryMs >= 30000UL) {
    lastBatteryMs = millis();
    JsonDocument battDoc;
    battDoc["type"] = "battery";
    battDoc["hwid"] = myHwId;
    battDoc["voltage"] = readBatteryVoltage();
    if (isHub) forwardToApp(battDoc);
    else sendToHub(battDoc);
  }
}
