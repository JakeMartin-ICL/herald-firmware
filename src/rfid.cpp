#include "globals.h"

// ---- RFID ----

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
MFRC522::MIFARE_Key rfidKey;

static bool     rfidWritePending = false;
static char     rfidPendingContent[32] = {};
static uint32_t rfidWriteRequestedMs = 0;
static uint32_t rfidWriteLastAttemptMs = 0;
static uint8_t  rfidWriteAttempts = 0;
static String   rfidWriteLastError;
static MFRC522::Uid rfidWriteUid = {};

static const uint32_t RFID_WRITE_WINDOW_MS = 1800;
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
static String rfidStatusError(const char* stage, MFRC522::StatusCode status) {
  return String(stage) + ": " + rfid.GetStatusCodeName(status);
}

static bool rfidAuth(String* error = nullptr) {
  for (byte i = 0; i < 6; i++) rfidKey.keyByte[i] = 0xFF;
  MFRC522::StatusCode s = rfid.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A, 3, &rfidKey, &rfid.uid);
  if (s != MFRC522::STATUS_OK) {
    Serial.print("RFID auth failed: ");
    Serial.println(rfid.GetStatusCodeName(s));
    if (error) *error = rfidStatusError("Authentication failed", s);
  }
  return s == MFRC522::STATUS_OK;
}

// Read up to 32 bytes from blocks 1-2 of sector 0 into `out`.
// Returns false if auth or any read fails.
static bool rfidReadContent(String& out) {
  if (!rfidAuth()) return false;

  char raw[33] = {};
  byte buf[18] = {};
  byte bufSize;
  MFRC522::StatusCode s;

  bufSize = sizeof(buf);
  s = rfid.MIFARE_Read(1, buf, &bufSize);
  if (s != MFRC522::STATUS_OK || bufSize != sizeof(buf)) {
    Serial.print("RFID: read block 1 failed: ");
    if (s == MFRC522::STATUS_OK) Serial.println("Incomplete response");
    else Serial.println(rfid.GetStatusCodeName(s));
    return false;
  }
  memcpy(raw, buf, 16);

  bufSize = sizeof(buf);
  s = rfid.MIFARE_Read(2, buf, &bufSize);
  if (s != MFRC522::STATUS_OK || bufSize != sizeof(buf)) {
    Serial.print("RFID: read block 2 failed: ");
    if (s == MFRC522::STATUS_OK) Serial.println("Incomplete response");
    else Serial.println(rfid.GetStatusCodeName(s));
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
  if (!rfidAuth(&errOut)) return false;

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
    errOut = rfidStatusError("Write block 1 failed", s);
    return false;
  }
  s = rfid.MIFARE_Write(2, block2, 16);
  if (s != MFRC522::STATUS_OK) {
    Serial.print("RFID: write block 2 failed: ");
    Serial.println(rfid.GetStatusCodeName(s));
    errOut = rfidStatusError("Write block 2 failed", s);
    return false;
  }
  // A write ACK alone does not prove the stored ID is correct. Verify both
  // blocks, including padding, before telling the app to advance to the next tag.
  const byte* expected[] = {block1, block2};
  for (byte block = 1; block <= 2; block++) {
    byte actual[18];
    byte size = sizeof(actual);
    s = rfid.MIFARE_Read(block, actual, &size);
    if (s != MFRC522::STATUS_OK) {
      errOut = rfidStatusError(block == 1 ? "Verify block 1 failed" : "Verify block 2 failed", s);
      return false;
    }
    if (memcmp(actual, expected[block - 1], 16) != 0) {
      errOut = block == 1 ? "Verify block 1 mismatch" : "Verify block 2 mismatch";
      return false;
    }
  }
  // The caller sends HALT while Crypto1 is still active, then stops Crypto1.
  return true;
}

static void finishPendingRfidWrite(bool success, const String& error = "") {
  JsonDocument resp;
  resp["type"] = "rfid_write_result";
  resp["hwid"] = myHwId;
  resp["success"] = success;
  if (success) resp["verified"] = true;
  resp["attempts"] = rfidWriteAttempts;
  if (!success && error.length() > 0) resp["error"] = error;
  forwardToApp(resp);

  rfidWritePending = false;
  rfidPendingContent[0] = '\0';
  rfidWriteRequestedMs = 0;
  rfidWriteLastAttemptMs = 0;
  rfidWriteAttempts = 0;
  rfidWriteLastError = "";
  rfidWriteUid.size = 0;
}

static void processPendingRfidWrite() {
  if (!rfidWritePending) return;

  uint32_t now = millis();
  if ((now - rfidWriteRequestedMs) >= RFID_WRITE_WINDOW_MS) {
    finishPendingRfidWrite(false, rfidWriteLastError);
    return;
  }

  if (rfidWriteAttempts > 0 && (now - rfidWriteLastAttemptMs) < RFID_WRITE_RETRY_MS) return;
  rfidWriteLastAttemptMs = now;
  rfidWriteAttempts++;

  byte atqa[2];
  byte atqaSize = sizeof(atqa);
  rfid.PCD_StopCrypto1();
  const MFRC522::StatusCode wakeStatus = rfid.PICC_WakeupA(atqa, &atqaSize);
  if (wakeStatus != MFRC522::STATUS_OK && wakeStatus != MFRC522::STATUS_COLLISION) {
    // Preserve a more informative select/auth/write/verify error on later
    // timeouts instead of replacing it with "No card in field".
    if (rfidWriteLastError.length() == 0) {
      rfidWriteLastError = rfidStatusError("No tag answered wake-up; keep tag on reader", wakeStatus);
    }
    resetRfidReader();
    return;
  }

  // WUPA changes the card to READY/READY*. SELECT must follow immediately.
  // Repeating wake-up/presence probes during a settling interval changes that
  // protocol state and can prevent a write from ever being attempted.
  debugLog("RFID write attempt " + String(rfidWriteAttempts));

  const MFRC522::StatusCode selectStatus = rfid.PICC_Select(&rfid.uid);
  if (selectStatus != MFRC522::STATUS_OK) {
    rfidWriteLastError = rfidStatusError("Card selection failed", selectStatus);
    resetRfidReader();
    return;
  }

  // A retry after a partial write must never continue on a different tag.
  if (rfidWriteUid.size == 0) rfidWriteUid = rfid.uid;
  else if (rfidWriteUid.size != rfid.uid.size || memcmp(rfidWriteUid.uidByte, rfid.uid.uidByte, rfid.uid.size) != 0) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    finishPendingRfidWrite(false, "Different tag detected during retry; present the original tag again");
    return;
  }

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

  // Match IsNewCardPresent's register setup, but retain the request status:
  // its boolean result hides errors that can leave a card in READY.
  rfid.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);
  byte atqa[2];
  byte atqaSize = sizeof(atqa);
  const MFRC522::StatusCode requestStatus = rfid.PICC_RequestA(atqa, &atqaSize);
  const bool answered = requestStatus == MFRC522::STATUS_OK || requestStatus == MFRC522::STATUS_COLLISION;
  if (!answered && requestStatus != MFRC522::STATUS_TIMEOUT) {
    resetRfidReader();
    return;
  }

  // A lost ATQA response can also leave a card READY. Try SELECT even after
  // a timeout, but do not reset if it fails: absent and successfully halted
  // cards normally time out, and resetting would re-arm duplicate reads.
  if (!rfid.PICC_ReadCardSerial()) {
    // The presence request already moved the card to READY. Leaving a failed
    // selection there makes later "new card" probes ignore a tag still held
    // in place. Reset the RF field so the next loop can try it again.
    if (answered) resetRfidReader();
    return;
  }

  String content;
  bool ok = rfidReadContent(content);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (!ok) {
    // HALT suppresses duplicates only after a successful read. On failure,
    // power-cycle the card via the reader reset so it can retry without the
    // user having to remove and re-present the tag.
    resetRfidReader();
    return;
  }

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
// Retry complete wake/select/auth/write/verify transactions while the tag is held.
void handleRfidWrite(const char* internalId) {
  if (!internalId || strlen(internalId) == 0 || strlen(internalId) >= sizeof(rfidPendingContent)) {
    finishPendingRfidWrite(false, "Tag ID must contain 1 to 31 characters");
    return;
  }
  strncpy(rfidPendingContent, internalId, sizeof(rfidPendingContent) - 1);
  rfidPendingContent[sizeof(rfidPendingContent) - 1] = '\0';
  rfidWritePending = true;
  rfidWriteRequestedMs = millis();
  rfidWriteLastAttemptMs = 0;
  rfidWriteAttempts = 0;
  rfidWriteLastError = "";
  rfidWriteUid.size = 0;

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
