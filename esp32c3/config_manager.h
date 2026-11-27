#pragma once

#include <Arduino.h>

void configurationBegin();
String configurationApiToken();
bool configurationReadApiToken(String& token);
bool configurationHasApiToken();
bool configurationSetApiToken(const String& token, bool clearToken, String& error);
bool configurationHasUpdatePassword();
bool configurationSetUpdatePassword(const String& password, String& error);
bool configurationVerifyUpdatePassword(const String& password);
