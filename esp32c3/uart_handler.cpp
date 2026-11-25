#include "uart_handler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstring>

#include "config.h"

extern HardwareSerial C6Serial;

namespace {
constexpr size_t UART_LINE_CAPACITY = 4096;
constexpr UBaseType_t UART_REPLY_QUEUE_LENGTH = 4;

struct UartReply {
  char json[UART_LINE_CAPACITY];
};

QueueHandle_t replyQueue = nullptr;
SemaphoreHandle_t transactionMutex = nullptr;
char lineBuffer[UART_LINE_CAPACITY];
char discardedReply[UART_LINE_CAPACITY];
size_t lineLength = 0;
bool droppingLongLine = false;

#if ARDUINO_USB_CDC_ON_BOOT
const char* replyName(C6ReplyKind expected) {
  switch (expected) {
    case C6_REPLY_LIST:
      return "list";
    case C6_REPLY_IMAGE_READY:
      return "image ready";
    case C6_REPLY_IMAGE_QUEUED:
      return "image queued";
    default:
      return "command";
  }
}
#endif

void uartReaderTask(void*) {
  for (;;) {
    while (C6Serial.available() > 0) {
      const int incoming = C6Serial.read();
      if (incoming < 0) break;
      const char c = static_cast<char>(incoming);
      if (c == '\n') {
        if (!droppingLongLine && lineLength > 0) {
          lineBuffer[lineLength] = '\0';
          const bool hasOk = strstr(lineBuffer, "\"ok\"") != nullptr;
          const bool isService = strstr(lineBuffer, "\"service\"") != nullptr;
          if (lineBuffer[0] == '{' && hasOk && !isService) {
            if (xQueueSend(replyQueue, lineBuffer, 0) != pdTRUE) {
#if ARDUINO_USB_CDC_ON_BOOT
              Serial.println("C6 UART reply queue full; dropping oldest reply.");
#endif
              xQueueReceive(replyQueue, discardedReply, 0);
              xQueueSend(replyQueue, lineBuffer, 0);
            }
          }
        }
        lineLength = 0;
        droppingLongLine = false;
      } else if (c != '\r') {
        if (lineLength + 1 < sizeof(lineBuffer)) lineBuffer[lineLength++] = c;
        else {
          lineLength = 0;
          droppingLongLine = true;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

bool matchesReply(const JsonDocument& response, C6ReplyKind expected) {
  if (!response["ok"].is<bool>()) return false;
  if (!response["ok"].as<bool>()) return true;
  switch (expected) {
    case C6_REPLY_LIST:
      return response["tags"].is<JsonArrayConst>();
    case C6_REPLY_IMAGE_READY:
      return response["state"] == "ready";
    case C6_REPLY_IMAGE_QUEUED:
      return response["state"] == "queued";
    default:
      return true;
  }
}
}

bool c6UartBegin() {
  replyQueue = xQueueCreate(UART_REPLY_QUEUE_LENGTH, sizeof(UartReply));
  transactionMutex = xSemaphoreCreateMutex();
  if (!replyQueue || !transactionMutex) return false;
  C6Serial.begin(C6_UART_BAUD, SERIAL_8N1, C6_UART_RX_PIN, C6_UART_TX_PIN);
  return xTaskCreate(uartReaderTask, "c6-uart-rx", 6144, nullptr, 2, nullptr) == pdPASS;
}

bool c6UartLock(uint32_t timeoutMs) {
  return transactionMutex && xSemaphoreTake(transactionMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void c6UartUnlock() {
  if (transactionMutex) xSemaphoreGive(transactionMutex);
}

bool c6UartCommand(const String& jsonLine, C6ReplyKind expected, JsonDocument& response, uint32_t timeoutMs) {
  if (!c6UartLock(timeoutMs)) return false;
  const bool result = c6UartCommandLocked(jsonLine, expected, response, timeoutMs);
  c6UartUnlock();
  return result;
}

bool c6UartCommandLocked(const String& jsonLine, C6ReplyKind expected, JsonDocument& response, uint32_t timeoutMs) {
  if (!replyQueue || jsonLine.isEmpty() || jsonLine.length() >= 2048) return false;
  UartReply stale;
  while (xQueueReceive(replyQueue, &stale, 0) == pdTRUE) {}
  C6Serial.write(reinterpret_cast<const uint8_t*>(jsonLine.c_str()), jsonLine.length());
  C6Serial.write('\n');
  C6Serial.flush();
  return c6UartWaitResponseLocked(expected, response, timeoutMs);
}

bool c6UartWaitResponse(C6ReplyKind expected, JsonDocument& response, uint32_t timeoutMs) {
  if (!c6UartLock(timeoutMs)) return false;
  const bool result = c6UartWaitResponseLocked(expected, response, timeoutMs);
  c6UartUnlock();
  return result;
}

bool c6UartWaitResponseLocked(C6ReplyKind expected, JsonDocument& response, uint32_t timeoutMs) {
  if (!replyQueue) return false;
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    const uint32_t remaining = timeoutMs - static_cast<uint32_t>(millis() - started);
    UartReply reply;
    const uint32_t waitMs = remaining < 100u ? remaining : 100u;
    if (xQueueReceive(replyQueue, &reply, pdMS_TO_TICKS(waitMs)) != pdTRUE) continue;
    DynamicJsonDocument parsed(UART_LINE_CAPACITY);
    const DeserializationError parseError = deserializeJson(parsed, reply.json);
    if (parseError) {
#if ARDUINO_USB_CDC_ON_BOOT
      Serial.print("C6 UART JSON parse failed: ");
      Serial.println(parseError.c_str());
#endif
      continue;
    }
    if (!matchesReply(parsed, expected)) {
#if ARDUINO_USB_CDC_ON_BOOT
      Serial.print("C6 UART reply shape mismatch for ");
      Serial.print(replyName(expected));
      Serial.print(": ok-type=");
      Serial.print(parsed["ok"].is<bool>());
      Serial.print(" ok=");
      Serial.print(parsed["ok"] | false);
      Serial.print(" tags-array=");
      Serial.println(parsed["tags"].is<JsonArray>());
#endif
      continue;
    }
    response.set(parsed);
    return true;
  }
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.print("C6 UART timeout waiting for: ");
  Serial.println(replyName(expected));
#endif
  return false;
}

bool c6UartSendImage(const uint8_t* data, size_t length) {
  if (!c6UartLock(30000)) return false;
  const bool result = c6UartSendImageLocked(data, length);
  c6UartUnlock();
  return result;
}

bool c6UartSendImageLocked(const uint8_t* data, size_t length) {
  if (!data || !length) return false;
  size_t sent = 0;
  while (sent < length) {
    const size_t chunk = min(static_cast<size_t>(1024), length - sent);
    const size_t written = C6Serial.write(data + sent, chunk);
    if (written != chunk) return false;
    sent += written;
    taskYIELD();
  }
  C6Serial.flush();
  return true;
}
