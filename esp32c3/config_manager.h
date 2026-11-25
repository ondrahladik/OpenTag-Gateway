#pragma once

#include <Arduino.h>

void configurationBegin();
String configurationApiToken();
bool configurationHasApiToken();
bool configurationSetApiToken(const String& token, bool clearToken, String& error);
