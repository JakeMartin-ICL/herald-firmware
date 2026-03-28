#include "globals.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_ADDR     0x3C

static Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static bool oledOk = false;

// ---- Normal display state ----

static char dispName[48]   = "";
static char dispStatus[32] = "";

// Round display
static int  dispRound     = 0;
static bool dispShowRound = false;

// Turn timer: firmware counts up from timerSecs as of timerSyncMs
static uint32_t dispTimerSecs    = 0;
static bool     dispTimerRunning = false;
static uint32_t dispTimerSyncMs  = 0;
static bool     dispShowTimer    = false;

// Countdown: counts down to dispCountdownEndMs
static bool     dispShowCountdown   = false;
static uint32_t dispCountdownEndMs  = 0;

// Message (max 21 chars — one line at text size 1)
static char dispMessage[22] = "";

// Battery voltage — updated from main loop; -1 = not yet read
static float dispBatteryVoltage = -1.0f;

// ---- Pending display cache (used when menu is open) ----

struct PendingDisplay {
  char     name[48];
  char     status[32];
  char     message[22];
  bool     showRound;
  int      round;
  bool     showTimer;
  bool     timerRunning;
  uint32_t timerSecs;
  bool     pending;
};
static PendingDisplay pendingDisplay = {};

// ---- Helpers ----

// Draw text centred horizontally at the given y; left-aligns if wider than screen.
static void drawCentered(const char* text, int y, uint8_t size) {
  oled.setTextSize(size);
  int16_t bx, by;
  uint16_t bw, bh;
  oled.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  int x = ((int)SCREEN_WIDTH - (int)bw) / 2;
  if (x < 0) x = 0;
  oled.setCursor(x, y);
  oled.print(text);
}

// Draw a low-battery icon in the top-right corner (13x7px).
// Only drawn when voltage is known and ≤ 3.4 V (≈15% for a LiPo).
static void drawLowBatteryIcon() {
  if (dispBatteryVoltage < 0.0f || dispBatteryVoltage > 3.4f) return;
  // Body outline: 11x7 px
  oled.drawRect(114, 0, 11, 7, SSD1306_WHITE);
  // Positive terminal nub: 2x3 px
  oled.fillRect(125, 2, 2, 3, SSD1306_WHITE);
  // Low-charge fill: 2px wide out of 9px interior
  oled.fillRect(115, 1, 2, 5, SSD1306_WHITE);
}

// ---- Normal display render ----

static void renderDisplay() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Determine which rows to show below the separator
  bool hasStatus = dispStatus[0] != '\0';
  bool hasMessage = dispMessage[0] != '\0';
  bool showTimer = dispShowTimer && dispTimerRunning;
  bool showCountdown = dispShowCountdown && (millis() < dispCountdownEndMs);
  int bottomRows = (hasStatus ? 1 : 0) + (dispShowRound ? 1 : 0) + (showTimer ? 1 : 0) + (showCountdown ? 1 : 0) + (hasMessage ? 1 : 0);

  if (bottomRows == 0) {
    // Name centred vertically over the full screen (no extras)
    uint8_t nameSize = (strlen(dispName) <= 10) ? 2 : 1;
    int nameH = (nameSize == 2) ? 16 : 8;
    int nameY = (SCREEN_HEIGHT - nameH) / 2;
    drawCentered(dispName, nameY, nameSize);
  } else {
    // Dynamic layout: name in upper portion, separator, bottom rows
    const int ROW_H = 10; // 8px text + 2px gap
    int sepY = SCREEN_HEIGHT - bottomRows * ROW_H - 2;
    if (sepY < 16) sepY = 16;

    uint8_t nameSize = (strlen(dispName) <= 10) ? 2 : 1;
    int nameH = (nameSize == 2) ? 16 : 8;
    int nameY = (sepY - nameH) / 2;
    if (nameY < 0) nameY = 0;
    drawCentered(dispName, nameY, nameSize);

    oled.drawFastHLine(0, sepY, SCREEN_WIDTH, SSD1306_WHITE);

    int rowY = sepY + 3;

    if (hasStatus) {
      drawCentered(dispStatus, rowY, 1);
      rowY += ROW_H;
    }
    if (dispShowRound) {
      char buf[16];
      snprintf(buf, sizeof(buf), "Round %d", dispRound);
      drawCentered(buf, rowY, 1);
      rowY += ROW_H;
    }
    if (showTimer) {
      uint32_t total = dispTimerSecs + (millis() - dispTimerSyncMs) / 1000;
      char buf[12];
      uint32_t h = total / 3600, m = (total % 3600) / 60, s = total % 60;
      if (h > 0) snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
      else        snprintf(buf, sizeof(buf), "%u:%02u", m, s);
      drawCentered(buf, rowY, 1);
      rowY += ROW_H;
    }
    if (showCountdown) {
      uint32_t nowMs = millis();
      uint32_t remaining = (dispCountdownEndMs > nowMs) ? (dispCountdownEndMs - nowMs + 999) / 1000 : 0;
      char buf[12];
      uint32_t m = remaining / 60, s = remaining % 60;
      if (m > 0) snprintf(buf, sizeof(buf), "-%u:%02u", m, s);
      else        snprintf(buf, sizeof(buf), "-%us", (unsigned)remaining);
      drawCentered(buf, rowY, 1);
      rowY += ROW_H;
    }
    if (hasMessage) {
      drawCentered(dispMessage, rowY, 1);
    }
  }

  drawLowBatteryIcon();
  oled.display();
}

// ---- Public functions ----

void initDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  delay(100); // let I2C bus settle before first command

  // Scan before init so wiring problems are visible even if begin() fails.
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C device found at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) Serial.println("I2C scan: no devices found — check wiring");

  // reset=false: reset pin is -1 (unconnected); software reset can leave display blank.
  // periphBegin=false: Wire already initialised above with custom pins.
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, false, false);
  if (!oledOk) {
    Serial.println("OLED init failed — check wiring and I2C address");
    // Release the I2C peripheral so floating SDA/SCL lines don't generate
    // spurious interrupts that interfere with WiFi packet processing.
    Wire.end();
    return;
  }

  Serial.println("OLED init OK");
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  drawCentered("Herald", (SCREEN_HEIGHT - 16) / 2, 2);
  oled.display();
}

void showIpOnDisplay(const char* ip) {
  strncpy(dispName, "Herald Hub", sizeof(dispName) - 1);
  dispName[sizeof(dispName) - 1] = '\0';
  strncpy(dispStatus, ip, sizeof(dispStatus) - 1);
  dispStatus[sizeof(dispStatus) - 1] = '\0';
  renderDisplay();
}

void showClientOnDisplay() {
  strncpy(dispName, "Herald", sizeof(dispName) - 1);
  dispName[sizeof(dispName) - 1] = '\0';
  strncpy(dispStatus, "Client", sizeof(dispStatus) - 1);
  dispStatus[sizeof(dispStatus) - 1] = '\0';
  renderDisplay();
}

void showHotspotOnDisplay() {
  strncpy(dispName, "Herald Hub", sizeof(dispName) - 1);
  dispName[sizeof(dispName) - 1] = '\0';
  strncpy(dispStatus, "Hotspot", sizeof(dispStatus) - 1);
  dispStatus[sizeof(dispStatus) - 1] = '\0';
  renderDisplay();
}

void refreshDisplay() {
  renderDisplay();
}

void setDisplayBatteryVoltage(float v) {
  dispBatteryVoltage = v;
}

void startCountdownOnDisplay(uint32_t durationMs) {
  dispShowCountdown  = true;
  dispCountdownEndMs = millis() + durationMs;
  renderDisplay();
}

void stopCountdownOnDisplay() {
  if (!dispShowCountdown) return;
  dispShowCountdown = false;
  renderDisplay();
}

// ---- RFID placement guide (animated) ----

static bool     rfidPromptActive = false;
static bool     rfidPromptFrame  = false; // false = sensor view, true = card view
static uint32_t rfidPromptMs     = 0;
static const uint32_t RFID_PROMPT_INTERVAL_MS = 2000;

// Flat-top/bottom regular hexagon: vertices at 0°,60°,120°,180°,240°,300°
static void drawHex(int cx, int cy, int r) {
  // cos/sin values for the 6 vertex angles, multiplied by 512 (fixed-point)
  static const int16_t COS6[6] = { 512,  256, -256, -512, -256,  256 };
  static const int16_t SIN6[6] = {   0,  443,  443,    0, -443, -443 };
  int vx[6], vy[6];
  for (int i = 0; i < 6; i++) {
    vx[i] = cx + (r * COS6[i] + 256) / 512;
    vy[i] = cy + (r * SIN6[i] + 256) / 512;
  }
  for (int i = 0; i < 6; i++) {
    oled.drawLine(vx[i], vy[i], vx[(i+1)%6], vy[(i+1)%6], SSD1306_WHITE);
  }
}

static void renderRfidPrompt() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Box top-down: hex with flat top/bottom, LED ring, button
  // Hex top flat edge: y≈9 (vertices at 240°/300°), bottom: y≈55
  const int cx  = 34, cy  = 32;
  const int hexR = 27, ringR = 15, btnR = 6;
  // RFID sensor: above button, centred in the ring
  const int rfx = cx, rfy = cy - 10;

  drawHex(cx, cy, hexR);
  oled.drawCircle(cx, cy, ringR, SSD1306_WHITE);
  oled.fillCircle(cx, cy, btnR, SSD1306_WHITE);

  if (!rfidPromptFrame) {
    // Frame 1: sensor location shown as crosshair + circle
    oled.drawCircle(rfx, rfy, 4, SSD1306_WHITE);
    oled.drawFastHLine(rfx - 9, rfy, 5, SSD1306_WHITE); // left arm
    oled.drawFastHLine(rfx + 5, rfy, 5, SSD1306_WHITE); // right arm
    oled.drawFastVLine(rfx, rfy - 9, 5, SSD1306_WHITE); // top arm
    oled.drawFastVLine(rfx, rfy + 5, 5, SSD1306_WHITE); // bottom arm
  } else {
    // Frame 2: card top-aligned to hex top edge, reaching near hex bottom
    // Hex top flat edge spans x=21..48, y=9; bottom at y≈55
    oled.fillRect(21, 9, 27, 43, SSD1306_BLACK);  // card covers diagram below
    oled.drawRect(21, 9, 27, 43, SSD1306_WHITE);  // card outline

    // Left-pointing arrow to the right of the hex top edge, at y=9
    oled.drawFastHLine(50, 9, 14, SSD1306_WHITE); // shaft →
    oled.drawLine(50, 9, 54, 7, SSD1306_WHITE);   // upper wing
    oled.drawLine(50, 9, 54, 11, SSD1306_WHITE);  // lower wing
  }

  // Divider + right-side label
  oled.drawFastVLine(68, 0, SCREEN_HEIGHT, SSD1306_WHITE);
  oled.setTextSize(1);
  if (!rfidPromptFrame) {
    oled.setCursor(72, 12);  oled.print("Sensor");
    oled.setCursor(72, 24);  oled.print("location");
  } else {
    oled.setCursor(72, 6);   oled.print("Align top");
    oled.setCursor(72, 16);  oled.print("edge of");
    oled.setCursor(72, 26);  oled.print("card with");
    oled.setCursor(72, 36);  oled.print("top edge");
    oled.setCursor(72, 46);  oled.print("of box");
  }

  drawLowBatteryIcon();
  oled.display();
}

void showRfidPromptOnDisplay() {
  rfidPromptActive = true;
  rfidPromptFrame  = false;
  rfidPromptMs     = millis();
  renderRfidPrompt();
}

void hideRfidPromptOnDisplay() {
  rfidPromptActive = false;
  renderDisplay();
}

void tickDisplay() {
  if (!oledOk) return;
  if (isMenuOpen()) return;
  if (rfidPromptActive) {
    uint32_t now = millis();
    if (now - rfidPromptMs >= RFID_PROMPT_INTERVAL_MS) {
      rfidPromptMs    = now;
      rfidPromptFrame = !rfidPromptFrame;
      renderRfidPrompt();
    }
    return;
  }

  // Detect countdown expiry — clear flag and do a final render
  if (dispShowCountdown && millis() >= dispCountdownEndMs) {
    dispShowCountdown = false;
    renderDisplay();
    // fall through: timer may still need ticking
  }

  bool needsTick = dispTimerRunning || dispShowCountdown;
  if (!needsTick) return;

  // Re-render once per second while timer or countdown is running
  static uint32_t lastSec = 0;
  uint32_t nowSec = millis() / 1000;
  if (nowSec != lastSec) {
    lastSec = nowSec;
    renderDisplay();
  }
}

static void applyDisplayDoc(JsonDocument& doc) {
  const char* name   = doc["name"]   | "";
  const char* status = doc["status"] | "";
  strncpy(dispName,   name,   sizeof(dispName)   - 1);
  dispName[sizeof(dispName) - 1] = '\0';
  strncpy(dispStatus, status, sizeof(dispStatus) - 1);
  dispStatus[sizeof(dispStatus) - 1] = '\0';

  if (doc["round"].is<int>()) {
    dispShowRound = true;
    dispRound = doc["round"].as<int>();
  } else {
    dispShowRound = false;
  }

  if (doc["timerRunning"].is<bool>()) {
    dispShowTimer    = true;
    bool newRunning  = doc["timerRunning"].as<bool>();
    uint32_t newSecs = doc["timerSecs"] | 0;
    dispTimerSecs    = newSecs;
    dispTimerRunning = newRunning;
    dispTimerSyncMs  = millis();
  } else {
    dispShowTimer    = false;
    dispTimerRunning = false;
  }

  const char* message = doc["message"] | "";
  strncpy(dispMessage, message, sizeof(dispMessage) - 1);
  dispMessage[sizeof(dispMessage) - 1] = '\0';
}

void applyPendingDisplay() {
  if (!pendingDisplay.pending) return;
  pendingDisplay.pending = false;
  strncpy(dispName,    pendingDisplay.name,    sizeof(dispName)    - 1);
  dispName[sizeof(dispName) - 1] = '\0';
  strncpy(dispStatus,  pendingDisplay.status,  sizeof(dispStatus)  - 1);
  dispStatus[sizeof(dispStatus) - 1] = '\0';
  strncpy(dispMessage, pendingDisplay.message, sizeof(dispMessage) - 1);
  dispMessage[sizeof(dispMessage) - 1] = '\0';
  dispShowRound    = pendingDisplay.showRound;
  dispRound        = pendingDisplay.round;
  dispShowTimer    = pendingDisplay.showTimer;
  dispTimerRunning = pendingDisplay.timerRunning;
  dispTimerSecs    = pendingDisplay.timerSecs;
  dispTimerSyncMs  = millis();
  renderDisplay();
}

void handleDisplayCommand(JsonDocument& doc) {
  const char* name   = doc["name"]   | "";
  const char* status = doc["status"] | "";
  Serial.printf("Display: '%s' / '%s'\n", name, status);

  if (isMenuOpen()) {
    // Cache the update; apply it when the menu closes
    strncpy(pendingDisplay.name,   name,   sizeof(pendingDisplay.name)   - 1);
    pendingDisplay.name[sizeof(pendingDisplay.name) - 1] = '\0';
    strncpy(pendingDisplay.status, status, sizeof(pendingDisplay.status) - 1);
    pendingDisplay.status[sizeof(pendingDisplay.status) - 1] = '\0';
    const char* message = doc["message"] | "";
    strncpy(pendingDisplay.message, message, sizeof(pendingDisplay.message) - 1);
    pendingDisplay.message[sizeof(pendingDisplay.message) - 1] = '\0';
    pendingDisplay.showRound    = doc["round"].is<int>();
    pendingDisplay.round        = doc["round"] | 0;
    pendingDisplay.showTimer    = doc["timerRunning"].is<bool>();
    pendingDisplay.timerRunning = doc["timerRunning"] | false;
    pendingDisplay.timerSecs    = doc["timerSecs"] | 0;
    pendingDisplay.pending      = true;
    return;
  }

  applyDisplayDoc(doc);
  renderDisplay();
}

// ---- Menu display ----

void showMenuOnDisplay(const char** items, int count, int cursor) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Title bar
  drawCentered("- MENU -", 0, 1);
  oled.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  // Items — show a scrolling window around the cursor
  const int ITEM_H   = 10;
  const int START_Y  = 13;
  const int MAX_VIS  = 5;

  int startIdx = cursor - 2;
  if (startIdx < 0) startIdx = 0;
  if (startIdx + MAX_VIS > count) startIdx = (count > MAX_VIS) ? count - MAX_VIS : 0;

  for (int i = 0; i < MAX_VIS && startIdx + i < count; i++) {
    int idx = startIdx + i;
    oled.setCursor(0, START_Y + i * ITEM_H);
    oled.setTextSize(1);
    oled.print(idx == cursor ? ">" : " ");
    oled.print(" ");
    // Truncate to fit screen (max ~20 chars at size 1)
    char buf[22];
    strncpy(buf, items[idx], 21);
    buf[21] = '\0';
    oled.print(buf);
  }

  oled.display();
}

void showMessageOnDisplay(const char* line1, const char* line2) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  if (line2 && line2[0]) {
    drawCentered(line1, 20, 1);
    drawCentered(line2, 36, 1);
  } else {
    drawCentered(line1, (SCREEN_HEIGHT - 8) / 2, 1);
  }
  oled.display();
}
