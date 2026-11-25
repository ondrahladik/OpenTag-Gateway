#include <Arduino.h>
#include "wifi_manager.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#include "config.h"

bool wifiManagerBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFiManager manager;
  manager.setDebugOutput(false);
  manager.setConnectTimeout(20);
  manager.setConfigPortalTimeout(0);
  manager.setAPCallback([](WiFiManager* portal) {
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.print("Wi-Fi setup AP started. SSID: ");
    Serial.print(portal->getConfigPortalSSID());
    Serial.print(", IP: ");
    Serial.println(WiFi.softAPIP());
#endif
  });
  if (!manager.autoConnect(WIFI_PORTAL_NAME)) return false;

  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  const uint32_t started = millis();
  while (time(nullptr) < 1700000000 && static_cast<uint32_t>(millis() - started) < 15000)
    vTaskDelay(pdMS_TO_TICKS(250));
  return true;
}
