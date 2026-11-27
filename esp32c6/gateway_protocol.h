#pragma once

#include <Arduino.h>

constexpr uint8_t GATEWAY_RADIO_CHANNEL = 25;
constexpr uint16_t GATEWAY_DEFAULT_CHECKIN_MINUTES = 5;
constexpr size_t GATEWAY_FIRMWARE_CHUNK_SIZE = 1024;

void gatewayProtocolBegin(bool radioStarted);
void gatewayProtocolHandleLine(char* line);
