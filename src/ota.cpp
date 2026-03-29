#include "globals.h"

// ---- OTA ----

void otaProgressCallback(int current, int total) {
  if (total <= 0) return;
  int percent = (current * 100) / total;
  if (percent == otaLastPercent) return;
  otaLastPercent = percent;

  if (otaProgressQueue) {
    xQueueSend(otaProgressQueue, &percent, 0);
  }
}

// Client OTA task — runs in background so main loop keeps calling wsClient.loop(),
// draining the receive buffer naturally without re-entrant loop() calls.
void otaTaskFn(void* param) {
  OtaArgs* args = (OtaArgs*)param;

  // Client boxes disconnect WiFi after election; reconnect here (safe: own task stack,
  // not the WiFi/ESP-NOW callback context where delay() would block the event loop).
  if (!isHub && WiFi.status() != WL_CONNECTED) {
    debugLog("OTA: reconnecting WiFi...");
    extern bool connectWifi();
    if (!connectWifi()) {
      debugLog("OTA: WiFi reconnect failed, aborting");
      delete args;
      if (otaProgressQueue) { vQueueDelete(otaProgressQueue); otaProgressQueue = NULL; }
      otaInProgress = false;
      vTaskDelete(NULL);
      return;
    }
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
    if (otaProgressQueue) { vQueueDelete(otaProgressQueue); otaProgressQueue = NULL; }
    otaInProgress = false;
  }

  vTaskDelete(NULL);
}

void performOtaUpdate(const char* url, const char* version) {
  if (otaInProgress) return;
  otaInProgress = true;
  otaLastPercent = -1;

  // Run OTA in a background task so the main loop keeps running (wsServer/wsClient.loop()),
  // responding to pings and draining receive buffers. WiFi reconnect also happens in the
  // task to avoid blocking the ESP-NOW/WiFi callback context.
  otaProgressQueue = xQueueCreate(20, sizeof(int));
  OtaArgs* args = new OtaArgs();
  strncpy(args->url, url, sizeof(args->url) - 1);
  args->url[sizeof(args->url) - 1] = '\0';
  strncpy(args->version, version, sizeof(args->version) - 1);
  args->version[sizeof(args->version) - 1] = '\0';
  xTaskCreate(otaTaskFn, "ota", 16384, args, 1, NULL);
}
