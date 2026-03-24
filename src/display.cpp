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

// ---- Normal display render ----

static void renderDisplay() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Determine which rows to show below the separator
  bool hasStatus = dispStatus[0] != '\0';
  bool showTimer = dispShowTimer && dispTimerRunning;
  int bottomRows = (hasStatus ? 1 : 0) + (dispShowRound ? 1 : 0) + (showTimer ? 1 : 0);

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
    }
  }

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

void tickDisplay() {
  if (!dispTimerRunning || !oledOk) return;
  // Re-render once per second while timer is running
  static uint32_t lastSec = 0;
  uint32_t nowSec = dispTimerSecs + (millis() - dispTimerSyncMs) / 1000;
  if (nowSec != lastSec) {
    lastSec = nowSec;
    renderDisplay();
  }
}

void handleDisplayCommand(JsonDocument& doc) {
  const char* name   = doc["name"]   | "";
  const char* status = doc["status"] | "";
  Serial.printf("Display: '%s' / '%s'\n", name, status);
  strncpy(dispName,   name,   sizeof(dispName)   - 1);
  dispName[sizeof(dispName) - 1] = '\0';
  strncpy(dispStatus, status, sizeof(dispStatus) - 1);
  dispStatus[sizeof(dispStatus) - 1] = '\0';

  // Round
  if (doc["round"].is<int>()) {
    dispShowRound = true;
    dispRound = doc["round"].as<int>();
  } else {
    dispShowRound = false;
  }

  // Timer
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
