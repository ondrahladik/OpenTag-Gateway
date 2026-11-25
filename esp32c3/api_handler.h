#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

void apiHandlerBegin();
void apiHandlerAppendStatus(JsonDocument& response);
