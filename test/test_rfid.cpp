#include "production_rfid.cpp"

uint32_t clockMs = 1000;
SerialMock Serial;
SpiMock SPI;
bool isHub = true, rfidEnabled = true;
String myHwId = "AA:BB:CC:DD:EE:FF";
std::vector<JsonDocument> messages;

static void reset() {
  finishPendingRfidWrite(false);
  messages.clear();
  clockMs = 1000;
  rfid = MFRC522(RFID_SS_PIN, RFID_RST_PIN);
  rfidEnabled = true;
}

static JsonDocument runWrite(const char* id = "ti:tile:42") {
  handleRfidWrite(id);
  for (int i = 0; rfidWritePending && i < 300; ++i) {
    clockMs += 10;
    loopRfid();
  }
  assert(!rfidWritePending);
  assert(messages.size() == 1);
  return messages.back();
}

static void heldTag() {
  reset();
  // Exactly the real read -> browser command flow: read and halt the card,
  // then keep it physically in place while the write command comes back.
  loopRfid();
  assert(messages.size() == 1);
  assert(messages.back()["type"].text == "rfid");
  assert(rfid.cardState == MFRC522::CardState::HALT);
  messages.clear();
  auto result = runWrite();
  std::cerr << "Held tag: " << rfid.writeCalls << " block writes; "
            << (result["success"].boolean ? "success" : result["error"].text) << "\n";
  assert(result["success"].boolean);
  assert(result["verified"].boolean);
  assert(rfid.writeCalls == 2);
  assert(rfid.cardState == MFRC522::CardState::HALT);
  assert(!rfid.crypto);
  assert(rfid.badHalts == 0);
  assert(std::memcmp(rfid.memory[1].data(), "ti:tile:42", 10) == 0);
}

static void noCard() {
  reset(); rfid.present = false;
  const auto result = runWrite();
  assert(!result.at("success").boolean);
  assert(result.at("error").text.find("wake-up") != std::string::npos);
  assert(rfid.writeCalls == 0);
}

static void cardArrivesLater() {
  reset(); rfid.present = false;
  handleRfidWrite("ti:tile:42");
  for (int i = 0; i < 30; ++i) { clockMs += 10; loopRfid(); }
  assert(rfidWritePending);
  assert(rfid.writeCalls == 0);
  rfid.present = true;
  for (int i = 0; i < 30 && rfidWritePending; ++i) { clockMs += 10; loopRfid(); }
  assert(!rfidWritePending);
  assert(messages.back()["verified"].boolean);
}

static void transientFailures() {
  for (int kind = 0; kind < 6; ++kind) {
    reset();
    if (kind == 0) rfid.selectFailures = 1;
    if (kind == 1) rfid.authFailures = 1;
    if (kind == 2) rfid.block1Failures = 1;
    if (kind == 3) rfid.block2Failures = 1;
    if (kind == 4) rfid.readFailures = 1;
    if (kind == 5) rfid.corruptVerifications = 1;
    const auto result = runWrite();
    assert(result.at("success").boolean);
    assert(result.at("verified").boolean);
    assert(rfid.selectCalls >= 2);
    assert(rfid.badHalts == 0);
    assert(rfid.cardState == MFRC522::CardState::HALT);
  }
}

static void permanentFailures() {
  for (int kind = 0; kind < 6; ++kind) {
    reset();
    if (kind == 0) rfid.selectFailures = 1000;
    if (kind == 1) rfid.authFailures = 1000;
    if (kind == 2) rfid.block1Failures = 1000;
    if (kind == 3) rfid.block2Failures = 1000;
    if (kind == 4) rfid.readFailures = 1000;
    if (kind == 5) rfid.corruptVerifications = 1000;
    const auto result = runWrite();
    assert(!result.at("success").boolean);
    assert(result.find("verified") == result.end());
    const std::string stages[] = {"selection", "Authentication", "Write block 1", "Write block 2", "Verify block 1", "mismatch"};
    assert(result.at("error").text.find(stages[kind]) != std::string::npos);
    assert(rfid.badHalts == 0);
  }
}

static void changedTagOnRetry() {
  reset(); rfid.block2Failures = 1; rfid.swapOnBlock2Failure = true;
  const auto result = runWrite();
  assert(!result.at("success").boolean);
  assert(result.at("error").text.find("Different tag") != std::string::npos);
  assert(rfid.writeCalls == 2); // No write reaches the replacement tag.
}

static void payloadBoundaries() {
  reset();
  const char* longest = "1234567890123456789012345678901";
  assert(runWrite(longest)["verified"].boolean);
  assert(std::memcmp(rfid.memory[1].data(), longest, 16) == 0);
  assert(std::memcmp(rfid.memory[2].data(), longest + 16, 15) == 0);
  assert(rfid.memory[2][15] == 0);
  messages.clear();
  assert(runWrite("x")["verified"].boolean);
  assert(rfid.memory[1][0] == 'x');
  for (size_t i = 1; i < 16; ++i) assert(rfid.memory[1][i] == 0);
  for (byte value : rfid.memory[2]) assert(value == 0);
  for (const char* invalid : {"", "12345678901234567890123456789012", static_cast<const char*>(nullptr)}) {
    reset();
    assert(!runWrite(invalid)["success"].boolean);
    assert(rfid.writeCalls == 0);
  }
}

static void readingStillWorks() {
  reset();
  assert(runWrite()["verified"].boolean);
  messages.clear();
  loopRfid(); // Holding a halted tag does not produce repeated read events.
  assert(messages.empty());
  rfid.present = false; loopRfid();
  rfid.cardState = MFRC522::CardState::IDLE;
  rfid.present = true; loopRfid();
  assert(messages.size() == 1);
  assert(messages.back()["tagId"].text == "ti:tile:42");
  assert(rfid.cardState == MFRC522::CardState::HALT);
  assert(rfid.badHalts == 0);

  reset(); rfid.readFailures = 1;
  loopRfid();
  assert(messages.empty());
  assert(!rfid.crypto);
  assert(rfid.cardState == MFRC522::CardState::HALT);
  assert(rfid.badHalts == 0);
}

static void timerWraparound() {
  reset(); clockMs = UINT32_MAX - 100;
  rfid.selectFailures = 2;
  assert(runWrite()["verified"].boolean);
}

static void normalPollingDoesNotSelectOrResetIdleReaders() {
  reset(); rfid.present = false;
  for (int i = 0; i < 100; ++i) loopRfid();
  assert(messages.empty());
  assert(rfid.selectCalls == 0); // Do not add a SELECT timeout to each empty poll.
  assert(rfid.resetCalls == 0);

  rfid.present = true;
  loopRfid();
  assert(messages.size() == 1);
  assert(rfid.selectCalls == 1);
  for (int i = 0; i < 100; ++i) loopRfid();
  assert(messages.size() == 1);
  assert(rfid.selectCalls == 1); // A held, successfully halted tag stays quiet.
  assert(rfid.resetCalls == 0);
  assert(rfid.writeCalls == 0);
}

static void differentTagsHaveNoReadCooldown() {
  reset();
  std::memcpy(rfid.memory[1].data(), "ti:tile:42", 10);
  loopRfid();
  assert(messages.size() == 1);
  // Swap physical tags without advancing time or waiting for LED feedback.
  rfid.cardNumber = 2;
  rfid.cardState = MFRC522::CardState::IDLE;
  std::memcpy(rfid.memory[1].data(), "ti:tile:43", 10);
  loopRfid();
  assert(messages.size() == 2);
  assert(messages.back()["tagId"].text == "ti:tile:43");
}

int main() {
  heldTag();
  noCard();
  cardArrivesLater();
  transientFailures();
  permanentFailures();
  changedTagOnRetry();
  payloadBoundaries();
  readingStillWorks();
  timerWraparound();
  normalPollingDoesNotSelectOrResetIdleReaders();
  differentTagsHaveNoReadCooldown();
  std::cout << "RFID tests passed\n";
}
