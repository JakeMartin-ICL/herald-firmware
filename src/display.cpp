#include "globals.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_ADDR     0x3C

static Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static bool oledOk = false;

static char dispName[48]   = "";
static char dispStatus[32] = "";

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

static void renderDisplay() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  bool hasStatus = dispStatus[0] != '\0';
  // Size 2 (12×16px/char) fits ~10 chars; fall back to size 1 (6×8px) for longer names.
  uint8_t nameSize = (strlen(dispName) <= 10) ? 2 : 1;
  int     nameH    = (nameSize == 2) ? 16 : 8;

  if (hasStatus) {
    // Name centred in the upper ~44px, separator, status centred below.
    int nameY = (44 - nameH) / 2;
    drawCentered(dispName, nameY, nameSize);
    oled.drawFastHLine(0, 44, SCREEN_WIDTH, SSD1306_WHITE);
    drawCentered(dispStatus, 50, 1);
  } else {
    // Name centred vertically over the full screen.
    int nameY = (SCREEN_HEIGHT - nameH) / 2;
    drawCentered(dispName, nameY, nameSize);
  }

  oled.display();
}

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

void handleDisplayCommand(JsonDocument& doc) {
  const char* name   = doc["name"]   | "";
  const char* status = doc["status"] | "";
  Serial.printf("Display: '%s' / '%s'\n", name, status);
  strncpy(dispName,   name,   sizeof(dispName)   - 1);
  dispName[sizeof(dispName) - 1] = '\0';
  strncpy(dispStatus, status, sizeof(dispStatus) - 1);
  dispStatus[sizeof(dispStatus) - 1] = '\0';
  renderDisplay();
}
