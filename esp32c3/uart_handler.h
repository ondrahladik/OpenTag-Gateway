#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

enum C6ReplyKind : uint8_t {
  C6_REPLY_ANY,
  C6_REPLY_LIST,
  C6_REPLY_IMAGE_READY,
  C6_REPLY_IMAGE_QUEUED
};

bool c6UartBegin();
bool c6UartLock(uint32_t timeoutMs);
void c6UartUnlock();
bool c6UartCommand(const String& jsonLine, C6ReplyKind expected, JsonDocument& response, uint32_t timeoutMs);
bool c6UartCommandLocked(const String& jsonLine, C6ReplyKind expected, JsonDocument& response, uint32_t timeoutMs);
bool c6UartWaitResponse(C6ReplyKind expected, JsonDocument& response, uint32_t timeoutMs);
bool c6UartWaitResponseLocked(C6ReplyKind expected, JsonDocument& response, uint32_t timeoutMs);
bool c6UartSendImage(const uint8_t* data, size_t length);
bool c6UartSendImageLocked(const uint8_t* data, size_t length);
