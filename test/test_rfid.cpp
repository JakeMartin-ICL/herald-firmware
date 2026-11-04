#include "production_rfid.cpp"

uint32_t clockMs = 1000;
SerialMock Serial;
SpiMock SPI;
bool isHub = true, rfidEnabled = true;
String myHwId = "AA:BB:CC:DD:EE:FF";
std::vector<JsonDocument> messages;
std::vector<std::string> debugMessages;

static void reset() {
  rfidEnabled = false;
  // Let an outstanding field cycle restore the antenna, using only public APIs.
  for (int i = 0; i < 3; ++i) { clockMs += 100; loopRfid(); }
  finishPendingRfidWrite(false);
  messages.clear();
  debugMessages.clear();
  clockMs = 1000;
  rfid = MFRC522(RFID_SS_PIN, RFID_RST_PIN);
  rfidEnabled = true;
  isHub = true;
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
  const uint32_t requested = clockMs;
  while (clockMs - requested < 300) { clockMs += 10; loopRfid(); }
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
  assert(!rfid.antennaOn);
  assert(rfid.resetCalls == 0);
  assert(rfid.badHalts == 0);
}

static void failedHeldTagEventuallyReads() {
  reset(); rfid.readFailures = 1;
  std::memcpy(rfid.memory[1].data(), "ti:tile:42", 10);
  loopRfid();
  assert(messages.empty());
  for (int i = 0; i < 100 && messages.empty(); ++i) { ++clockMs; loopRfid(); }
  assert(messages.size() == 1); // Same physical tag; never removed/re-presented.
  assert(messages.back()["tagId"].text == "ti:tile:42");
}

static void timerWraparound() {
  reset(); clockMs = UINT32_MAX - 100;
  rfid.selectFailures = 2;
  assert(runWrite()["verified"].boolean);
}

static void normalPollingDoesNotSelectOrResetIdleReaders() {
  reset(); rfid.present = false;
  const uint32_t start = clockMs;
  for (int i = 0; i < 100; ++i) loopRfid();
  assert(clockMs - start == 2500); // Exactly the original REQA timeout per poll.
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
  assert(rfid.fieldOffCalls == 0 && rfid.fieldOnCalls == 0);
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

static void waitForReadRecovery(uint32_t elapsed) {
  clockMs += elapsed;
  const uint32_t before = clockMs;
  loopRfid();
  assert(clockMs == before); // Waiting phases must yield, never sleep or transact.
}

static void failedReadsRetryHeldTags() {
  for (int kind = 0; kind < 6; ++kind) {
    reset();
    isHub = kind % 2 == 0; // Both local and client forwarding paths.
    std::memcpy(rfid.memory[1].data(), "ti:tile:42", 10);
    if (kind == 0) rfid.selectFailures = 1;
    if (kind == 1) rfid.authFailures = 1;
    if (kind == 2) rfid.readFailures = 1;
    if (kind == 3) rfid.block2ReadFailures = 1;
    if (kind == 4 || kind == 5) { rfid.shortReads = 1; rfid.shortReadBlock = kind - 3; }
    loopRfid();
    assert(messages.empty());
    assert(!rfid.antennaOn && !rfid.crypto);
    assert(rfid.fieldOffCalls == 1 && rfid.resetCalls == 0);
    const char* errors[] = {"selection", "Authentication", "Read block 1", "Read block 2", "Read block 1 incomplete", "Read block 2 incomplete"};
    assert(debugMessages.back().find(errors[kind]) != std::string::npos);
    const int probes = rfid.presenceCalls;
    const uint32_t failedAt = clockMs;
    waitForReadRecovery(9);
    assert(!rfid.antennaOn);
    waitForReadRecovery(1);
    assert(rfid.antennaOn);
    waitForReadRecovery(9);
    assert(messages.empty() && rfid.presenceCalls == probes);
    ++clockMs;
    loopRfid();
    assert(messages.size() == 1);
    assert(messages.back()["tagId"].text == "ti:tile:42");
    assert(clockMs - failedAt == 50); // 20 ms field cycle + model's 30 ms normal read.
    assert(rfid.fieldOnCalls == 1 && rfid.earlyActivations == 0);
    assert(rfid.resetCalls == 0 && rfid.writeCalls == 0 && rfid.badHalts == 0);
    for (int i = 0; i < 100; ++i) loopRfid();
    assert(messages.size() == 1); // No field cycles/duplicates after success.
    assert(rfid.fieldOffCalls == 1 && rfid.fieldOnCalls == 1);
  }
}

static void swappingTagsDuringRecovery() {
  reset(); rfid.authFailures = 1;
  std::memcpy(rfid.memory[1].data(), "ti:tile:42", 10);
  loopRfid();
  assert(messages.empty());
  rfid.cardNumber = 2;
  std::memcpy(rfid.memory[1].data(), "ti:tile:43", 10);
  for (int i = 0; i < 30 && messages.empty(); ++i) { ++clockMs; loopRfid(); }
  assert(messages.size() == 1);
  assert(messages.back()["tagId"].text == "ti:tile:43");
  assert(rfid.earlyActivations == 0 && rfid.resetCalls == 0);
}

static void disablingDuringRecoveryRestoresAntenna() {
  reset(); rfid.authFailures = 1;
  loopRfid();
  rfidEnabled = false;
  const int selects = rfid.selectCalls;
  waitForReadRecovery(10);
  assert(rfid.antennaOn);
  waitForReadRecovery(10);
  assert(messages.empty() && rfid.selectCalls == selects);
  rfidEnabled = true;
  loopRfid();
  assert(messages.size() == 1 && messages.back()["tagId"].text.empty());
  assert(rfid.earlyActivations == 0 && rfid.resetCalls == 0);
}

static void writingDuringRecoveryWaitsForField() {
  for (bool settling : {false, true}) {
    reset(); rfid.authFailures = 1;
    loopRfid();
    if (settling) waitForReadRecovery(10);
    const uint32_t before = clockMs;
    handleRfidWrite("ti:tile:42");
    loopRfid();
    assert(clockMs == before);
    assert(rfid.writeCalls == 0 && rfid.wakeCalls == 0);
    for (int i = 0; i < 100 && rfidWritePending; ++i) { ++clockMs; loopRfid(); }
    assert(!rfidWritePending);
    assert(messages.size() == 1 && messages.back()["verified"].boolean);
    assert(rfid.writeCalls == 2);
    assert(rfid.earlyActivations == 0 && rfid.resetCalls == 0);
  }
}

static void persistentFailuresAndReaderRemoval() {
  reset(); rfid.authFailures = 1000;
  for (int i = 0; i < 100; ++i) {
    ++clockMs;
    const uint32_t before = clockMs;
    loopRfid();
    assert(clockMs - before <= 27); // Only the SDK transaction can block.
  }
  assert(messages.empty() && rfid.fieldOffCalls > 1 && rfid.resetCalls == 0);
  rfid.present = false;
  for (int i = 0; i < 30; ++i) { ++clockMs; loopRfid(); }
  const int cycles = rfid.fieldOffCalls;
  for (int i = 0; i < 10; ++i) loopRfid();
  assert(rfid.fieldOffCalls == cycles && rfid.antennaOn);
  rfid.authFailures = 0;
  rfid.present = true;
  loopRfid();
  assert(messages.size() == 1);
  assert(rfid.earlyActivations == 0);
}

static void readRecoveryAcrossTimerWraparound() {
  reset(); clockMs = UINT32_MAX - 40; rfid.authFailures = 1;
  loopRfid();
  for (int i = 0; i < 30 && messages.empty(); ++i) { ++clockMs; loopRfid(); }
  assert(messages.size() == 1);
  assert(rfid.fieldOffCalls == 1 && rfid.fieldOnCalls == 1 && rfid.earlyActivations == 0);
}

int main() {
  heldTag();
  noCard();
  cardArrivesLater();
  transientFailures();
  permanentFailures();
  changedTagOnRetry();
  payloadBoundaries();
  failedHeldTagEventuallyReads();
  readingStillWorks();
  timerWraparound();
  normalPollingDoesNotSelectOrResetIdleReaders();
  differentTagsHaveNoReadCooldown();
  failedReadsRetryHeldTags();
  swappingTagsDuringRecovery();
  disablingDuringRecoveryRestoresAntenna();
  writingDuringRecoveryWaitsForField();
  persistentFailuresAndReaderRemoval();
  readRecoveryAcrossTimerWraparound();
  std::cout << "RFID tests passed\n";
}
