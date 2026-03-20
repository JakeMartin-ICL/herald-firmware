#include "globals.h"

// ---- RFID ----

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
MFRC522::MIFARE_Key rfidKey;

void initRfid() {
  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();
  Serial.println("RFID reader initialised");
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

// Read up to 32 bytes from blocks 1–2 of sector 0 into `out`.
// Returns false if auth or any read fails.
static bool rfidReadContent(String& out) {
  if (!rfidAuth()) return false;

  char raw[33] = {};
  byte buf[18];
  byte bufSize;

  bufSize = sizeof(buf);
  if (rfid.MIFARE_Read(1, buf, &bufSize) == MFRC522::STATUS_OK)
    memcpy(raw, buf, 16);

  bufSize = sizeof(buf);
  if (rfid.MIFARE_Read(2, buf, &bufSize) == MFRC522::STATUS_OK)
    memcpy(raw + 16, buf, 16);

  raw[32] = '\0';
  out = String(raw);
  int nullPos = out.indexOf('\0');
  if (nullPos >= 0) out = out.substring(0, nullPos);
  return true;
}

// Write a null-terminated string into blocks 1–2 of sector 0 (max 31 chars).
static bool rfidWriteContent(const char* content) {
  if (!rfidAuth()) return false;

  byte block1[16] = {};
  byte block2[16] = {};
  size_t len = min(strlen(content), (size_t)31);
  if (len <= 16) {
    memcpy(block1, content, len);
  } else {
    memcpy(block1, content, 16);
    memcpy(block2, content + 16, len - 16);
  }

  if (rfid.MIFARE_Write(1, block1, 16) != MFRC522::STATUS_OK) {
    Serial.println("RFID: write block 1 failed");
    rfid.PCD_StopCrypto1();
    return false;
  }
  if (rfid.MIFARE_Write(2, block2, 16) != MFRC522::STATUS_OK) {
    Serial.println("RFID: write block 2 failed");
    rfid.PCD_StopCrypto1();
    return false;
  }
  rfid.PCD_StopCrypto1();
  return true;
}

// Called from loop() — detect new tags, read their content, forward as rfid message.
void loopRfid() {
  if (!rfidEnabled) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String content;
  bool ok = rfidReadContent(content);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (!ok) return;

  if (content.length() == 0) {
    Serial.println("RFID: blank tag, ignoring");
    return;
  }
  if (content.indexOf(':') < 0) {
    Serial.printf("RFID: unrecognised format: %s\n", content.c_str());
    return;
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
// Wakes any card in the field (including halted ones) and writes the internalId string.
void handleRfidWrite(const char* internalId) {
  byte atqa[2];
  byte atqaSize = sizeof(atqa);

  // WUPA wakes cards in halt state; fall back to REQA for freshly presented cards
  bool cardPresent = (rfid.PICC_WakeupA(atqa, &atqaSize) == MFRC522::STATUS_OK)
                  || rfid.PICC_IsNewCardPresent();

  JsonDocument resp;
  resp["type"] = "rfid_write_result";
  resp["hwid"] = myHwId;

  if (!cardPresent || !rfid.PICC_ReadCardSerial()) {
    resp["success"] = false;
    resp["error"] = "No card in field";
    forwardToApp(resp);
    return;
  }

  bool ok = rfidWriteContent(internalId);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  resp["success"] = ok;
  if (!ok) resp["error"] = "Write failed";
  forwardToApp(resp);
}
