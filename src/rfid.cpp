#include "globals.h"

// ---- RFID ----

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
MFRC522::MIFARE_Key rfidKey;

static bool     rfidWritePending = false;
static char     rfidPendingContent[32] = {};
static uint32_t rfidWriteRequestedMs = 0;
static uint32_t rfidWriteSeenMs = 0;
static uint32_t rfidWriteLastAttemptMs = 0;
static uint8_t  rfidWriteAttempts = 0;
static String   rfidWriteLastError;

static const uint32_t RFID_WRITE_WINDOW_MS = 1800;
static const uint32_t RFID_WRITE_SETTLE_MS = 120;
static const uint32_t RFID_WRITE_RETRY_MS = 90;

static void resetRfidReader() {
  rfid.PCD_Reset();
  delay(4);
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
}

void initRfid() {
  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max); // boost from default 33dB to 48dB

  // Check SPI connectivity: firmware version register should be 0x91 or 0x92.
  // 0x00 or 0xFF means SPI communication failed (loose wire, wrong pins, dead chip).
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (ver == 0x00 || ver == 0xFF) {
    Serial.printf("RFID: SPI communication failed (VersionReg=0x%02X) - check wiring\n", ver);
  } else {
    Serial.printf("RFID reader initialised (firmware v0x%02X, gain=max)\n", ver);
  }
}

// Authenticate sector 0 with the default factory key (all 0xFF).
static bool rfidAuth() {
  for (byte i = 0; i < 6; i++) rfidKey.keyByte[i] = 0xFF;
  MFRC522::StatusCode s = rfid.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A, 3, &rfidKey, &rfid.uid);
  if (s != MFRC522::STATUS_OK) {
    Serial.print("RFID auth failed: ");
    Serial.println(rfid.GetStatusCodeName(s));
  }
  return s == MFRC522::STATUS_OK;
}

// Read up to 32 bytes from blocks 1-2 of sector 0 into `out`.
// Returns false if auth or any read fails.
static bool rfidReadContent(String& out) {
  if (!rfidAuth()) return false;

  char raw[33] = {};
  byte buf[18];
  byte bufSize;
  MFRC522::StatusCode s;

  bufSize = sizeof(buf);
  s = rfid.MIFARE_Read(1, buf, &bufSize);
  if (s != MFRC522::STATUS_OK) {
    Serial.print("RFID: read block 1 failed: ");
    Serial.println(rfid.GetStatusCodeName(s));
    rfid.PCD_StopCrypto1();
    return false;
  }
  memcpy(raw, buf, 16);

  bufSize = sizeof(buf);
  s = rfid.MIFARE_Read(2, buf, &bufSize);
  if (s != MFRC522::STATUS_OK) {
    Serial.print("RFID: read block 2 failed: ");
    Serial.println(rfid.GetStatusCodeName(s));
    rfid.PCD_StopCrypto1();
    return false;
  }
  memcpy(raw + 16, buf, 16);

  raw[32] = '\0';
  out = String(raw);
  int nullPos = out.indexOf('\0');
  if (nullPos >= 0) out = out.substring(0, nullPos);
  return true;
}

// Write a null-terminated string into blocks 1-2 of sector 0 (max 31 chars).
// Returns true on success; sets errOut and returns false on failure.
static bool rfidWriteContent(const char* content, String& errOut) {
  if (!rfidAuth()) {
    errOut = "Auth failed - tag may need replacing";
    return false;
  }

  byte block1[16] = {};
  byte block2[16] = {};
  size_t len = min(strlen(content), (size_t)31);
  if (len <= 16) {
    memcpy(block1, content, len);
  } else {
    memcpy(block1, content, 16);
    memcpy(block2, content + 16, len - 16);
  }

  MFRC522::StatusCode s = rfid.MIFARE_Write(1, block1, 16);
  if (s != MFRC522::STATUS_OK) {
    Serial.print("RFID: write block 1 failed: ");
    Serial.println(rfid.GetStatusCodeName(s));
    rfid.PCD_StopCrypto1();
    errOut = "Write failed (block 1)";
    return false;
  }
  s = rfid.MIFARE_Write(2, block2, 16);
  if (s != MFRC522::STATUS_OK) {
    Serial.print("RFID: write block 2 failed: ");
    Serial.println(rfid.GetStatusCodeName(s));
    rfid.PCD_StopCrypto1();
    errOut = "Write failed (block 2)";
    return false;
  }
  rfid.PCD_StopCrypto1();
  return true;
}

static void finishPendingRfidWrite(bool success, const String& error = "") {
  JsonDocument resp;
  resp["type"] = "rfid_write_result";
  resp["hwid"] = myHwId;
  resp["success"] = success;
  if (!success && error.length() > 0) resp["error"] = error;
  forwardToApp(resp);

  rfidWritePending = false;
  rfidPendingContent[0] = '\0';
  rfidWriteRequestedMs = 0;
  rfidWriteSeenMs = 0;
  rfidWriteLastAttemptMs = 0;
  rfidWriteAttempts = 0;
  rfidWriteLastError = "";
}

static void processPendingRfidWrite() {
  if (!rfidWritePending) return;

  uint32_t now = millis();
  if ((now - rfidWriteRequestedMs) >= RFID_WRITE_WINDOW_MS) {
    finishPendingRfidWrite(false, rfidWriteAttempts == 0 ? "No card in field" : rfidWriteLastError);
    return;
  }

  byte atqa[2];
  byte atqaSize = sizeof(atqa);
  bool cardPresent = (rfid.PICC_WakeupA(atqa, &atqaSize) == MFRC522::STATUS_OK)
                  || rfid.PICC_IsNewCardPresent();

  if (!cardPresent) {
    rfidWriteSeenMs = 0;
    return;
  }

  if (rfidWriteSeenMs == 0) rfidWriteSeenMs = now;
  if ((now - rfidWriteSeenMs) < RFID_WRITE_SETTLE_MS) return;
  if ((now - rfidWriteLastAttemptMs) < RFID_WRITE_RETRY_MS) return;

  rfidWriteLastAttemptMs = now;
  rfidWriteAttempts++;
  debugLog("RFID write attempt " + String(rfidWriteAttempts));

  if (!rfid.PICC_ReadCardSerial()) {
    rfidWriteLastError = "Card detected but UID read failed";
    resetRfidReader();
    return;
  }

  // Give the PICC a brief moment to settle after anticollision/select before
  // starting authenticated writes. Reads can succeed while auth/write still fail.
  delay(20);

  String writeErr;
  bool ok = rfidWriteContent(rfidPendingContent, writeErr);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (ok) {
    finishPendingRfidWrite(true);
    return;
  }

  rfidWriteLastError = writeErr;
  resetRfidReader();
}

// Called from loop() - detect new tags, read their content, forward as rfid message.
void loopRfid() {
  if (!rfidEnabled) return;

  if (rfidWritePending) {
    processPendingRfidWrite();
    return;
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String content;
  bool ok = rfidReadContent(content);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (!ok) return;

  // Sanitise: non-ASCII bytes in a WebSocket text frame cause the browser to close
  // the connection. Replace any such bytes so the message is always valid UTF-8.
  bool hadBadBytes = false;
  for (size_t i = 0; i < content.length(); i++) {
    if ((uint8_t)content[i] < 0x20 || (uint8_t)content[i] > 0x7E) {
      content[i] = '?';
      hadBadBytes = true;
    }
  }
  if (hadBadBytes) {
    Serial.printf("RFID: corrupted tag (non-ASCII bytes), sanitised: %s\n", content.c_str());
  }

  if (content.length() > 0 && content.indexOf(':') < 0) {
    Serial.printf("RFID: unrecognised format: %s\n", content.c_str());
    return;
  }
  if (content.length() == 0) {
    Serial.println("RFID: blank tag");
  }

  debugLog("RFID tag read: " + content);
  JsonDocument doc;
  doc["type"] = "rfid";
  doc["hwid"] = myHwId;
  doc["tagId"] = content;
  if (isHub) forwardToApp(doc);
  else sendToHub(doc);
}

// Called when the hub receives an rfid_write command from the app.
// Queue a short retry window so writes can wait for the tag to settle in the field.
void handleRfidWrite(const char* internalId) {
  strncpy(rfidPendingContent, internalId, sizeof(rfidPendingContent) - 1);
  rfidPendingContent[sizeof(rfidPendingContent) - 1] = '\0';
  rfidWritePending = true;
  rfidWriteRequestedMs = millis();
  rfidWriteSeenMs = 0;
  rfidWriteLastAttemptMs = 0;
  rfidWriteAttempts = 0;
  rfidWriteLastError = "Write failed";

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
