#include <Arduino.h>

#include "gateway_protocol.h"
#include "tag_radio.h"

static constexpr int C3_UART_RX_PIN = 10;
static constexpr int C3_UART_TX_PIN = 11;
static constexpr char FIRMWARE_VERSION[] = "v1.0.0";
static char lineBuffer[2048];
static size_t lineLength = 0;

void setup() {
  Serial.begin(115200);
  Serial.printf("OpenTag ESP32-C6 %s starting.\n", FIRMWARE_VERSION);
  Serial1.begin(115200, SERIAL_8N1, C3_UART_RX_PIN, C3_UART_TX_PIN);
  Serial.printf("[GW] UART1 ready, RX=GPIO%d TX=GPIO%d\n", C3_UART_RX_PIN, C3_UART_TX_PIN);
  delay(250);

  const bool radioStarted = tagRadioBegin(GATEWAY_RADIO_CHANNEL, GATEWAY_DEFAULT_CHECKIN_MINUTES);
  gatewayProtocolBegin(radioStarted);
}

void loop() {
  while (Serial1.available()) {
    const char c = static_cast<char>(Serial1.read());
    if (c == '\n') {
      lineBuffer[lineLength] = 0;
      gatewayProtocolHandleLine(lineBuffer);
      lineLength = 0;
    } else if (c != '\r' && lineLength + 1 < sizeof(lineBuffer)) {
      lineBuffer[lineLength++] = c;
    } else if (lineLength + 1 >= sizeof(lineBuffer)) {
      lineLength = 0;
    }
  }
  delay(2);
}
