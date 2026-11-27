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
constexpr size_t GATEWAY_FIRMWARE_CHUNK_SIZE = 1024;

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
    case C6_REPLY_GATEWAY_FW_CHUNK:
      return "gateway firmware chunk";
    case C6_REPLY_GATEWAY_FW_UPDATED:
      return "gateway firmware update";
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
    case C6_REPLY_GATEWAY_FW_CHUNK:
      return response["state"] == "chunk";
    case C6_REPLY_GATEWAY_FW_UPDATED:
      return response["state"] == "updated";
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

bool c6UartSendGatewayFirmwareLocked(const uint8_t* data, size_t length, size_t alreadySent,
                                     String& error) {
  if (!data || !length) {
    error = "No firmware data was provided.";
    return false;
  }
  size_t sent = 0;
  while (sent < length) {
    const size_t chunk = min(GATEWAY_FIRMWARE_CHUNK_SIZE, length - sent);
    const uint8_t chunkLength[2] = {
        static_cast<uint8_t>(chunk & 0xFF),
        static_cast<uint8_t>((chunk >> 8) & 0xFF),
    };
    if (C6Serial.write(chunkLength, sizeof(chunkLength)) != sizeof(chunkLength) ||
        C6Serial.write(data + sent, chunk) != chunk) {
      error = String("UART write failed at byte ") + String(sent) + ".";
      return false;
    }
    C6Serial.flush();

    const size_t expectedReceived = alreadySent + sent + chunk;
    DynamicJsonDocument response(128);
    if (!c6UartWaitResponseLocked(C6_REPLY_GATEWAY_FW_CHUNK, response, 30000)) {
      error = String("ESP32-C6 did not acknowledge the chunk ending at byte ") +
              String(expectedReceived) + ".";
      return false;
    }
    if (!response["ok"].as<bool>()) {
      error = String(response["error"] | "ESP32-C6 rejected a firmware chunk.");
      return false;
    }
    if (response["received"].as<size_t>() != expectedReceived) {
      error = String("ESP32-C6 acknowledged ") + String(response["received"].as<size_t>()) +
              " bytes, expected " + String(expectedReceived) + ".";
      return false;
    }
    sent += chunk;
  }
  return true;
}
