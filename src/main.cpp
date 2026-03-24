#include "globals.h"
#include <FastLED.h>

// ---- Shared global definitions ----

bool isHub = false;
bool isHotspot = false;
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

static bool connectToHotspot() {
  Serial.printf("Scanning for herald hotspot (%s)...\n", HOTSPOT_SSID);
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == HOTSPOT_SSID) { found = true; break; }
  }
  WiFi.scanDelete();
  if (!found) { Serial.println("Herald hotspot not found"); return false; }
  Serial.println("Herald hotspot found — connecting...");
  WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

static void activateHotspot() {
  WiFi.softAP(HOTSPOT_SSID, HOTSPOT_PASS);
  isHotspot = true;
  Serial.printf("Herald hotspot started: %s\n", HOTSPOT_SSID);
  showHotspotOnDisplay();
}

// ---- Hardware ID ----

String getHwId() {
  return WiFi.macAddress();
}

// ---- Battery voltage ----

float readBatteryVoltage() {
  // Take 16 samples spread over ~32ms to average out brief current spikes
  // (WiFi TX bursts, LED updates). analogReadMilliVolts uses eFuse calibration.
  const int NUM_SAMPLES = 16;
  uint32_t sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    sum += analogReadMilliVolts(VBAT_PIN);
    if (i < NUM_SAMPLES - 1) delay(2);
  }
  float raw = (sum / (float)NUM_SAMPLES) / 1000.0f * VBAT_DIVIDER_RATIO;

  // Exponential moving average (α=0.1) across calls to smooth load-dependent
  // sag between reports. Initialise to the first raw reading.
  static float filtered = -1.0f;
  if (filtered < 0.0f) filtered = raw;
  else filtered = 0.1f * raw + 0.9f * filtered;

  return filtered;
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
static uint32_t animFadeMs      = 0; // per-animation fade duration override; 0 = full frame duration
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

  animFadeMs = 0; // raw animations always fade for the full frame duration
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
    // Linear interpolation in sRGB, gamma applied at display.
    // animFadeMs shortens the fade-in so the LED snaps to target then holds.
    uint32_t fd = (animFadeMs > 0 && animFadeMs < frame.ms) ? animFadeMs : frame.ms;
    float t = (float)elapsed / (float)fd;
    if (t > 1.0f) t = 1.0f;
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

// ---- Named LED pattern handlers ----

static void applyLedOff() {
  stopLedAnim();
  FastLED.clear();
  FastLED.show();
}

static void applyLedSolid(const char* colorHex) {
  stopLedAnim();
  CRGB c = parseHexColor(colorHex);
  fill_solid(ledRing, LED_RING_COUNT, c);
  FastLED.show();
}

static void applyLedAlternate(const char* colorHex) {
  stopLedAnim();
  CRGB c = parseHexColor(colorHex);
  for (int i = 0; i < LED_RING_COUNT; i++)
    ledRing[i] = (i % 4 < 2) ? c : CRGB::Black;
  FastLED.show();
}

static void applyLedAlternatePair(const char* aHex, const char* bHex) {
  stopLedAnim();
  CRGB ca = parseHexColor(aHex), cb = parseHexColor(bHex);
  for (int i = 0; i < LED_RING_COUNT; i++)
    ledRing[i] = (i % 4 < 2) ? ca : cb;
  FastLED.show();
}

static void applyLedHalf(const char* colorHex, bool first) {
  stopLedAnim();
  CRGB c = parseHexColor(colorHex);
  for (int i = 0; i < LED_RING_COUNT; i++)
    ledRing[i] = ((i < LED_RING_COUNT / 2) == first) ? c : CRGB::Black;
  FastLED.show();
}

static void applyLedRainbow() {
  stopLedAnim();
  for (int i = 0; i < LED_RING_COUNT; i++) {
    uint8_t hue = (uint8_t)((i * 256) / LED_RING_COUNT);
    CRGB c = CHSV(hue, 255, 255);
    ledRing[i] = CRGB(GAMMA8[c.r], GAMMA8[c.g], GAMMA8[c.b]);
  }
  FastLED.show();
}

static void applyLedThirds(const char* c1Hex, const char* c2Hex, const char* c3Hex) {
  stopLedAnim();
  int t = LED_RING_COUNT / 3;
  CRGB c1 = parseHexColor(c1Hex), c2 = parseHexColor(c2Hex), c3 = parseHexColor(c3Hex);
  for (int i = 0; i < LED_RING_COUNT; i++) {
    if (i < t)       ledRing[i] = c1;
    else if (i < t*2) ledRing[i] = c2;
    else              ledRing[i] = c3;
  }
  FastLED.show();
}

static void applyLedSectors(JsonDocument& doc) {
  stopLedAnim();
  FastLED.clear();
  JsonArray sectors = doc["sectors"].as<JsonArray>();
  int pos = 0;
  for (JsonObject sector : sectors) {
    CRGB c = parseHexColor(sector["color"] | "#000000");
    int count = sector["count"] | 0;
    for (int i = 0; i < count && pos < LED_RING_COUNT; i++, pos++)
      ledRing[pos] = c;
  }
  FastLED.show();
}

// ---- Named animation helpers (build directly into animFrames[]) ----

static void startAnimEngine(int frameCount, bool loop, uint32_t overrideFadeMs = 0) {
  animFadeMs = overrideFadeMs;
  animFrameCount = frameCount;
  animLoops = loop ? -1 : 1;
  memset(prevR, 0, sizeof(prevR));
  memset(prevG, 0, sizeof(prevG));
  memset(prevB, 0, sizeof(prevB));
  animFrameIndex = 0;
  animFrameStart = millis();
  animLastFadeMs = 0;
  if (!animFrames[0].fade) showAnimFrame(0);
}

static void applyLedAnimBreathe(const char* colorHex, bool rainbow, uint32_t halfPeriodMs) {
  stopLedAnim();
  animFrames[0].ms = halfPeriodMs; animFrames[0].fade = true;
  animFrames[1].ms = halfPeriodMs; animFrames[1].fade = true;
  if (rainbow) {
    for (int i = 0; i < LED_RING_COUNT; i++) {
      CRGB c = CHSV((uint8_t)((i * 255) / LED_RING_COUNT), 255, 255);
      animFrames[0].r[i] = c.r; animFrames[0].g[i] = c.g; animFrames[0].b[i] = c.b;
      animFrames[1].r[i] = c.r*5/100; animFrames[1].g[i] = c.g*5/100; animFrames[1].b[i] = c.b*5/100;
    }
  } else {
    uint8_t r, g, b;
    parseHexColorRaw(colorHex, r, g, b);
    for (int i = 0; i < LED_RING_COUNT; i++) { animFrames[0].r[i] = r; animFrames[0].g[i] = g; animFrames[0].b[i] = b; }
    for (int i = 0; i < LED_RING_COUNT; i++) { animFrames[1].r[i] = r*5/100; animFrames[1].g[i] = g*5/100; animFrames[1].b[i] = b*5/100; }
  }
  startAnimEngine(2, true);
}

static void applyLedAnimSpinner(const char* colorHex, bool rainbow, uint32_t stepMs, uint32_t fadeMs) {
  stopLedAnim();
  uint8_t br = 0, bg = 0, bb = 0;
  if (!rainbow) parseHexColorRaw(colorHex, br, bg, bb);
  const float tail[] = {1.0f, 0.7f, 0.4f, 0.15f, 0.03f};
  for (int headPos = 0; headPos < LED_RING_COUNT; headPos++) {
    AnimFrame& af = animFrames[headPos];
    af.ms = stepMs; af.fade = true;
    memset(af.r, 0, LED_RING_COUNT); memset(af.g, 0, LED_RING_COUNT); memset(af.b, 0, LED_RING_COUNT);
    uint8_t hr = br, hg = bg, hb = bb;
    if (rainbow) {
      CRGB c = CHSV((uint8_t)((headPos * 256) / LED_RING_COUNT), 255, 255);
      hr = c.r; hg = c.g; hb = c.b;
    }
    for (int t = 0; t < 5; t++) {
      int pos = (headPos - t + LED_RING_COUNT) % LED_RING_COUNT;
      af.r[pos] = (uint8_t)(hr * tail[t]);
      af.g[pos] = (uint8_t)(hg * tail[t]);
      af.b[pos] = (uint8_t)(hb * tail[t]);
    }
  }
  startAnimEngine(LED_RING_COUNT, true, fadeMs);
}

static void applyLedAnimChoosing(JsonDocument& doc) {
  stopLedAnim();
  JsonArray colorsArr = doc["colors"].as<JsonArray>();
  int numColors = min((int)colorsArr.size(), 8);
  if (numColors == 0) return;
  uint32_t activeMs = doc["activeMs"] | 600;
  uint32_t fadeMs   = doc["fadeMs"]   | 100;
  int ledsPerColor  = LED_RING_COUNT / numColors;
  uint8_t cr[8], cg[8], cb[8];
  for (int i = 0; i < numColors; i++)
    parseHexColorRaw(colorsArr[i] | "#000000", cr[i], cg[i], cb[i]);
  // 2 frames per colour: static hold, then fade to next
  for (int active = 0; active < numColors; active++) {
    auto fill = [&](AnimFrame& f, int brightIdx) {
      for (int c = 0; c < numColors; c++) {
        float br = (c == brightIdx) ? 1.0f : 0.5f;
        for (int j = 0; j < ledsPerColor; j++) {
          int pos = c * ledsPerColor + j;
          if (pos >= LED_RING_COUNT) break;
          f.r[pos] = (uint8_t)(cr[c] * br);
          f.g[pos] = (uint8_t)(cg[c] * br);
          f.b[pos] = (uint8_t)(cb[c] * br);
        }
      }
    };
    AnimFrame& sf = animFrames[active * 2];
    sf.ms = activeMs; sf.fade = false;
    fill(sf, active);
    AnimFrame& ff = animFrames[active * 2 + 1];
    ff.ms = fadeMs; ff.fade = true;
    fill(ff, (active + 1) % numColors);
  }
  startAnimEngine(numColors * 2, true);
}

static void applyLedAnimUpkeep() {
  stopLedAnim();
  const uint8_t GR = 0xd4, GG = 0xa0, GB = 0x17;   // gold
  const uint8_t PR = 0xe6, PG = 0x4d, PB = 0xa0;   // pink
  const uint8_t BRNR = 0xcc, BRNG = 0x77, BRNB = 0x00; // brown
  const int N = LED_RING_COUNT, T = N / 3;
  int fi = 0;
  auto setLed = [&](AnimFrame& f, int pos, uint8_t r, uint8_t g, uint8_t b) {
    f.r[pos] = r; f.g[pos] = g; f.b[pos] = b;
  };
  auto clear = [&](AnimFrame& f) { memset(f.r,0,N); memset(f.g,0,N); memset(f.b,0,N); };
  // Frame: all off
  clear(animFrames[fi]); animFrames[fi].ms = 100; animFrames[fi].fade = false; fi++;
  // Gold fill
  for (int i = 1; i <= T; i++) {
    clear(animFrames[fi]); animFrames[fi].ms = 100; animFrames[fi].fade = true;
    for (int j = 0; j < i; j++) setLed(animFrames[fi], j, GR, GG, GB);
    fi++;
  }
  // Hold gold 2s
  clear(animFrames[fi]); animFrames[fi].ms = 2000; animFrames[fi].fade = false;
  for (int j = 0; j < T; j++) setLed(animFrames[fi], j, GR, GG, GB);
  fi++;
  // Pink fill (keep gold)
  for (int i = 1; i <= T; i++) {
    clear(animFrames[fi]); animFrames[fi].ms = 100; animFrames[fi].fade = true;
    for (int j = 0; j < T; j++) setLed(animFrames[fi], j, GR, GG, GB);
    for (int j = 0; j < i; j++) setLed(animFrames[fi], T+j, PR, PG, PB);
    fi++;
  }
  // Brown fill (keep gold + pink)
  for (int i = 1; i <= T; i++) {
    clear(animFrames[fi]); animFrames[fi].ms = 100; animFrames[fi].fade = true;
    for (int j = 0; j < T; j++) { setLed(animFrames[fi], j, GR, GG, GB); setLed(animFrames[fi], T+j, PR, PG, PB); }
    for (int j = 0; j < i; j++) setLed(animFrames[fi], T*2+j, BRNR, BRNG, BRNB);
    fi++;
  }
  // Hold full 5s
  clear(animFrames[fi]); animFrames[fi].ms = 5000; animFrames[fi].fade = false;
  for (int j = 0; j < T; j++) { setLed(animFrames[fi], j, GR, GG, GB); setLed(animFrames[fi], T+j, PR, PG, PB); setLed(animFrames[fi], T*2+j, BRNR, BRNG, BRNB); }
  fi++;
  startAnimEngine(fi, true);
}

void handleLedPatternCommand(JsonDocument& doc) {
  const char* t = doc["type"] | "";
  if      (strcmp(t, "led_off")            == 0) applyLedOff();
  else if (strcmp(t, "led_solid")          == 0) applyLedSolid(doc["color"] | "#000000");
  else if (strcmp(t, "led_alternate")      == 0) applyLedAlternate(doc["color"] | "#000000");
  else if (strcmp(t, "led_alternate_pair") == 0) applyLedAlternatePair(doc["a"] | "#000000", doc["b"] | "#000000");
  else if (strcmp(t, "led_half")           == 0) applyLedHalf(doc["color"] | "#000000", doc["first"] | false);
  else if (strcmp(t, "led_rainbow")        == 0) applyLedRainbow();
  else if (strcmp(t, "led_thirds")         == 0) applyLedThirds(doc["c1"] | "#000000", doc["c2"] | "#000000", doc["c3"] | "#000000");
  else if (strcmp(t, "led_sectors")        == 0) applyLedSectors(doc);
  else if (strcmp(t, "led_anim_breathe")   == 0) applyLedAnimBreathe(doc["color"] | "#ffffff", doc["rainbow"] | false, doc["halfPeriodMs"] | 2000);
  else if (strcmp(t, "led_anim_spinner")   == 0) applyLedAnimSpinner(doc["color"] | "#ffffff", doc["rainbow"] | false, doc["stepMs"] | 50, doc["fadeMs"] | 0);
  else if (strcmp(t, "led_anim_choosing")  == 0) applyLedAnimChoosing(doc);
  else if (strcmp(t, "led_anim_upkeep")    == 0) applyLedAnimUpkeep();
  else if (strcmp(t, "led_anim_stop")      == 0) stopLedAnim();
  else if (strcmp(t, "led_anim")           == 0) handleLedAnim(doc);  // raw fallback
  else if (strcmp(t, "led")                == 0) handleLedCommand(doc); // raw fallback
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
  sendToHubEspNow(doc);
}

// ---- On-device menu ----

enum MenuLayer { MENU_NONE = 0, MENU_MAIN, MENU_WIFI };
static MenuLayer menuLayer   = MENU_NONE;
static int       menuCursor  = 0;
static uint32_t  menuLastInput = 0;
#define MENU_TIMEOUT_MS 10000

static void renderMenu() {
  // Build item list for the current layer
  static char     itemBufs[12][24];
  static const char* itemPtrs[12];
  int count = 0;

  if (menuLayer == MENU_MAIN) {
    strncpy(itemBufs[count], "Exit", 23);         itemPtrs[count] = itemBufs[count]; count++;
    strncpy(itemBufs[count], "Choose WiFi", 23);  itemPtrs[count] = itemBufs[count]; count++;
    if (isHub) {
      strncpy(itemBufs[count], "Become Hotspot", 23); itemPtrs[count] = itemBufs[count]; count++;
    }
  } else if (menuLayer == MENU_WIFI) {
    strncpy(itemBufs[count], "Back", 23); itemPtrs[count] = itemBufs[count]; count++;
    String currentSsid = WiFi.SSID();
    for (int i = 0; i < credentialCount && count < 12; i++) {
      bool isCurrent = (credentials[i].ssid == currentSsid);
      snprintf(itemBufs[count], 24, "%s%s", isCurrent ? "*" : " ", credentials[i].ssid.c_str());
      itemPtrs[count] = itemBufs[count];
      count++;
    }
    // Append HeraldHub hotspot entry
    bool onHotspot = isHotspot || (currentSsid == HOTSPOT_SSID);
    snprintf(itemBufs[count], 24, "%s%s", onHotspot ? "*" : " ", HOTSPOT_SSID);
    itemPtrs[count] = itemBufs[count]; count++;
  }

  if (menuCursor >= count) menuCursor = 0;
  showMenuOnDisplay(itemPtrs, count, menuCursor);
}

static void closeMenu() {
  menuLayer = MENU_NONE;
  refreshDisplay();
}

static void openMenu() {
  menuLayer    = MENU_MAIN;
  menuCursor   = 0;
  menuLastInput = millis();
  renderMenu();
}

static void switchWifi(int credIdx) {
  if (credIdx < 0 || credIdx >= credentialCount) return;
  menuLayer = MENU_NONE;

  const char* ssid = credentials[credIdx].ssid.c_str();
  showMessageOnDisplay("Connecting...", ssid);

  WiFi.disconnect();
  WiFi.begin(credentials[credIdx].ssid.c_str(), credentials[credIdx].password.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    if (isHub) {
      MDNS.end();
      if (MDNS.begin(HUB_HOSTNAME)) MDNS.addService("ws", "tcp", WS_PORT);
      showIpOnDisplay(WiFi.localIP().toString().c_str());
    } else {
      WiFi.disconnect(false); // stay on channel for ESP-NOW, drop association
      refreshDisplay();
    }
  } else {
    showMessageOnDisplay("Connect failed", ssid);
    delay(2000);
    refreshDisplay();
  }
}

static void switchToHotspot() {
  menuLayer = MENU_NONE;
  if (isHub) {
    activateHotspot();
    MDNS.end();
    if (MDNS.begin(HUB_HOSTNAME)) MDNS.addService("ws", "tcp", WS_PORT);
    return;
  }
  // Client: connect briefly to align ESP-NOW channel, then drop association
  showMessageOnDisplay("Connecting...", HOTSPOT_SSID);
  WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    WiFi.disconnect(false);
    showMessageOnDisplay("Switched to", HOTSPOT_SSID);
    delay(1500);
    refreshDisplay();
  } else {
    showMessageOnDisplay("Connect failed", HOTSPOT_SSID);
    delay(2000);
    refreshDisplay();
  }
}

static void menuEnter() {
  menuLastInput = millis();
  if (menuLayer == MENU_MAIN) {
    if (menuCursor == 0) {
      closeMenu();
    } else if (menuCursor == 1) {
      menuLayer  = MENU_WIFI;
      menuCursor = 0;
      renderMenu();
    } else if (menuCursor == 2 && isHub) {
      switchToHotspot(); // "Become Hotspot"
    }
  } else if (menuLayer == MENU_WIFI) {
    if (menuCursor == 0) {
      menuLayer  = MENU_MAIN;
      menuCursor = 0;
      renderMenu();
    } else if (menuCursor - 1 < credentialCount) {
      switchWifi(menuCursor - 1);
    } else {
      switchToHotspot(); // HeraldHub entry at end of list
    }
  }
}

static void menuNext() {
  menuLastInput = millis();
  // Count items for current layer
  int count = (menuLayer == MENU_MAIN) ? (isHub ? 3 : 2) : (2 + credentialCount);
  menuCursor = (menuCursor + 1) % count;
  renderMenu();
}

void tickMenu() {
  if (menuLayer == MENU_NONE) return;
  if (millis() - menuLastInput >= MENU_TIMEOUT_MS) closeMenu();
}

// ---- Button handling ----

bool lastEndTurnState = HIGH;
unsigned long endTurnPressTime = 0;
bool longPressHandled = false;
const unsigned long LONG_PRESS_MS = 2000;

bool lastPassState = HIGH;
static unsigned long passPressTime = 0;
static bool passLongPressHandled = false;

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
      if (menuLayer != MENU_NONE) {
        closeMenu(); // long press exits menu from anywhere
      } else {
        sendButtonEvent("longpress");
      }
    }
  }

  if (endTurnState == HIGH && lastEndTurnState == LOW) {
    if (!longPressHandled) {
      if (menuLayer != MENU_NONE) {
        menuEnter();
      } else {
        sendButtonEvent("endturn");
      }
    }
  }

  lastEndTurnState = endTurnState;

  // Pass button — simple press; long press opens menu
  bool passState = digitalRead(BUTTON_PASS);

  if (passState == LOW && lastPassState == HIGH) {
    passPressTime        = millis();
    passLongPressHandled = false;
  }

  if (passState == LOW && !passLongPressHandled) {
    if (millis() - passPressTime >= LONG_PRESS_MS) {
      passLongPressHandled = true;
      openMenu();
    }
  }

  if (passState == HIGH && lastPassState == LOW) {
    if (!passLongPressHandled) {
      if (menuLayer != MENU_NONE) {
        menuNext();
      } else {
        sendButtonEvent("pass");
      }
    }
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
    Serial.println("Saved networks failed — scanning for herald hotspot...");
    if (!connectToHotspot()) {
      Serial.println("No hotspot found — becoming hotspot hub");
      activateHotspot();
    }
  }

  WiFi.setSleep(false); // disable modem sleep — prevents missed incoming packets

  myHwId = getHwId();
  Serial.printf("Hardware ID: %s\n", myHwId.c_str());

  delay(500);
  electHub();

  if (isHub && !isHotspot) showIpOnDisplay(WiFi.localIP().toString().c_str());
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
    // Client: no WebSocket loop — communication is via ESP-NOW
    if (otaComplete) {
      ESP.restart();
    }

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
  tickDisplay();
  tickMenu();
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
