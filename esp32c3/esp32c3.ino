#include <Arduino.h>
#include <WiFi.h>

#include "api_handler.h"
#include "config_manager.h"
#include "uart_handler.h"
#include "web_server.h"
#include "wifi_manager.h"

HardwareSerial C6Serial(1);

void setup() {
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.begin(115200);
  const uint32_t serialWaitStarted = millis();
  while (!Serial && static_cast<uint32_t>(millis() - serialWaitStarted) < 2000)
    delay(10);
  Serial.println("OpenTag ESP32-C3 starting.");
#endif

  configurationBegin();
  const bool uartStarted = c6UartBegin();
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.println(uartStarted ? "C6 UART ready." : "ERROR: C6 UART failed to start.");
  Serial.println("Wi-Fi: connecting to saved network or starting setup portal.");
#endif

  if (!wifiManagerBegin()) {
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.println("ERROR: Wi-Fi setup failed.");
#endif
    return;
  }

#if ARDUINO_USB_CDC_ON_BOOT
  Serial.print("Wi-Fi connected. IP: ");
  Serial.println(WiFi.localIP());
#endif
  const bool webStarted = webServerBegin();
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.println(webStarted ? "Web administration server ready." : "ERROR: Web server task failed to start.");
#endif
  if (!webStarted) return;
  apiHandlerBegin();
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.println("API worker started.");
#endif
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
