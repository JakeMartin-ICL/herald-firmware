#include "globals.h"

// ---- OTA ----

void otaProgressCallback(int current, int total) {
  if (total <= 0) return;
  int percent = (current * 100) / total;
  updateOtaLed(percent);
}

// OTA task — runs in background so the main loop keeps calling wsServer.loop() on the hub,
// and because WiFiClientSecure + TLS needs more stack than the Arduino loop() task provides.
// The task also avoids blocking the ESP-NOW receive callback where this is triggered.
void otaTaskFn(void* param) {
  OtaArgs* args = (OtaArgs*)param;

  // Client boxes disconnect WiFi after election; reconnect here (safe: own task stack,
  // not the WiFi/ESP-NOW callback context where delay() would block the event loop).
  // Deinit ESP-NOW first — it locks the radio to one channel, which blocks WiFi scanning.
  if (!isHub && WiFi.status() != WL_CONNECTED) {
    debugLog("OTA: deiniting ESP-NOW and reconnecting WiFi...");
    esp_now_deinit();
    extern bool connectWifi();
    if (!connectWifi()) {
      debugLog("OTA: WiFi reconnect failed, aborting");
      delete args;
      otaInProgress = false;
      vTaskDelete(NULL);
      return;
    }
    showMessageOnDisplay("Updating", "");
  }

  debugLog("OTA: starting update from " + String(args->url));

  WiFiClientSecure wifiClient;
  wifiClient.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.onProgress(otaProgressCallback);

  t_httpUpdate_return ret = httpUpdate.update(wifiClient, args->url);

  if (ret == HTTP_UPDATE_OK) {
    strncpy(otaCompleteVersion, args->version, sizeof(otaCompleteVersion) - 1);
  }
  delete args;

  if (ret == HTTP_UPDATE_OK) {
    otaComplete = true; // main loop handles notification, clean disconnect, and restart
  } else {
    Serial.printf("OTA failed: %s\n", httpUpdate.getLastErrorString().c_str());
    otaInProgress = false;
  }

  vTaskDelete(NULL);
}

void performOtaUpdate(const char* url, const char* version) {
  if (otaInProgress) return;
  otaInProgress = true;
  startOtaLed();

  OtaArgs* args = new OtaArgs();
  strncpy(args->url, url, sizeof(args->url) - 1);
  args->url[sizeof(args->url) - 1] = '\0';
  strncpy(args->version, version, sizeof(args->version) - 1);
  args->version[sizeof(args->version) - 1] = '\0';
  xTaskCreate(otaTaskFn, "ota", 16384, args, 1, NULL);
}
