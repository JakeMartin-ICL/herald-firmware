#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using byte = uint8_t;
using std::min;

class String : public std::string {
public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
  explicit String(unsigned int value) : std::string(std::to_string(value)) {}
  int indexOf(char value) const {
    const auto index = find(value);
    return index == npos ? -1 : static_cast<int>(index);
  }
  String substring(size_t first, size_t last) const { return substr(first, last - first); }
};

struct JsonValue {
  std::string text;
  bool boolean = false;
  JsonValue& operator=(bool value) { boolean = value; return *this; }
  JsonValue& operator=(const char* value) { text = value; return *this; }
  JsonValue& operator=(const std::string& value) { text = value; return *this; }
  JsonValue& operator=(unsigned int value) { text = std::to_string(value); return *this; }
  JsonValue& operator=(int value) { text = std::to_string(value); return *this; }
};
struct JsonDocument : public std::map<std::string, JsonValue> {};

extern uint32_t clockMs;
inline uint32_t millis() { return clockMs; }
inline void delay(uint32_t duration) { clockMs += duration; }
struct SerialMock {
  template <typename... Args> void printf(const char*, Args...) {}
  template <typename T> void print(const T&) {}
  template <typename T> void println(const T&) {}
};
extern SerialMock Serial;
struct SpiMock { template <typename... Args> void begin(Args...) {} };
extern SpiMock SPI;

// Model the MFRC522 API boundary, not the writer. Wake-up puts an IDLE/HALT
// card in READY; SELECT must follow it. Re-probing READY is not a passive
// presence check. Crypto1 must remain enabled until HALT is transmitted.
class MFRC522 {
public:
  enum StatusCode { STATUS_OK, STATUS_ERROR, STATUS_COLLISION, STATUS_TIMEOUT };
  enum PCD_Register { VersionReg };
  enum { RxGain_max, PICC_CMD_MF_AUTH_KEY_A };
  struct MIFARE_Key { byte keyByte[6]; };
  struct Uid { byte size = 0; byte uidByte[10] = {}; byte sak = 8; };
  enum class CardState { IDLE, READY, ACTIVE, HALT };
  Uid uid;
  bool present = true, authenticated = false, crypto = false;
  CardState cardState = CardState::IDLE;
  byte cardNumber = 1;
  std::array<std::array<byte, 16>, 3> memory = {};
  int wakeCalls = 0, selectCalls = 0, writeCalls = 0, readCalls = 0, badHalts = 0;
  int authFailures = 0, block1Failures = 0, block2Failures = 0;
  int selectFailures = 0, readFailures = 0, corruptVerifications = 0;
  bool wroteBlock2 = false, swapOnBlock2Failure = false;

  MFRC522(int, int) {}
  void PCD_Init() {}
  void PCD_Reset() { cardState = CardState::IDLE; authenticated = false; crypto = false; }
  void PCD_SetAntennaGain(int) {}
  byte PCD_ReadRegister(PCD_Register) { return 0x92; }
  void PCD_StopCrypto1() { crypto = false; }
  static const char* GetStatusCodeName(StatusCode status) {
    return status == STATUS_TIMEOUT ? "Timeout in communication" : "Error in communication";
  }
  StatusCode PICC_WakeupA(byte*, byte*) {
    ++wakeCalls;
    if (!present || (cardState != CardState::IDLE && cardState != CardState::HALT)) return STATUS_TIMEOUT;
    cardState = CardState::READY;
    return STATUS_OK;
  }
  bool PICC_IsNewCardPresent() {
    if (!present || cardState != CardState::IDLE) return false;
    cardState = CardState::READY;
    return true;
  }
  bool PICC_ReadCardSerial() {
    ++selectCalls;
    if (!present || cardState != CardState::READY) return false;
    if (selectFailures > 0) { --selectFailures; return false; }
    cardState = CardState::ACTIVE;
    uid.size = 4;
    uid.uidByte[0] = cardNumber;
    uid.sak = 8;
    return true;
  }
  StatusCode PICC_Select(Uid* selected) {
    if (!PICC_ReadCardSerial()) return STATUS_TIMEOUT;
    *selected = uid;
    return STATUS_OK;
  }
  StatusCode PICC_HaltA() {
    if (cardState == CardState::ACTIVE && (!authenticated || crypto)) {
      cardState = CardState::HALT;
      authenticated = false;
      return STATUS_OK;
    }
    if (cardState == CardState::ACTIVE) ++badHalts;
    return STATUS_TIMEOUT;
  }
  StatusCode PCD_Authenticate(int, byte trailer, MIFARE_Key*, Uid*) {
    assert(trailer == 3);
    if (!present || cardState != CardState::ACTIVE) return STATUS_TIMEOUT;
    if (authFailures > 0) { --authFailures; return STATUS_TIMEOUT; }
    crypto = true; authenticated = true;
    return STATUS_OK;
  }
  StatusCode MIFARE_Write(byte block, byte* data, byte size) {
    ++writeCalls;
    assert(block == 1 || block == 2); // Never write UID or access/key blocks.
    assert(size == 16);
    if (!present || cardState != CardState::ACTIVE || !authenticated || !crypto) return STATUS_TIMEOUT;
    if (block == 1 && block1Failures > 0) { --block1Failures; return STATUS_TIMEOUT; }
    if (block == 2 && block2Failures > 0) {
      --block2Failures;
      if (swapOnBlock2Failure) ++cardNumber;
      return STATUS_TIMEOUT;
    }
    std::copy(data, data + 16, memory[block].begin());
    if (block == 2) wroteBlock2 = true;
    return STATUS_OK;
  }
  StatusCode MIFARE_Read(byte block, byte* data, byte* size) {
    ++readCalls;
    assert(block == 1 || block == 2);
    assert(*size >= 18);
    if (!present || cardState != CardState::ACTIVE || !authenticated || !crypto) return STATUS_TIMEOUT;
    if (readFailures > 0) { --readFailures; return STATUS_TIMEOUT; }
    std::copy(memory[block].begin(), memory[block].end(), data);
    if (wroteBlock2 && corruptVerifications > 0) { --corruptVerifications; data[0] ^= 1; }
    data[16] = data[17] = 0;
    *size = 18;
    return STATUS_OK;
  }
};

constexpr int RFID_SS_PIN = 5, RFID_RST_PIN = 17;
constexpr int RFID_SCK_PIN = 18, RFID_MISO_PIN = 19, RFID_MOSI_PIN = 23;
extern bool isHub, rfidEnabled;
extern String myHwId;
extern std::vector<JsonDocument> messages;
inline void forwardToApp(JsonDocument& message) { messages.push_back(message); }
inline void sendToHub(JsonDocument& message) { messages.push_back(message); }
inline void debugLog(const String&) {}
