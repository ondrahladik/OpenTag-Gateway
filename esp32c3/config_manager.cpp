#include "config_manager.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "opentag";
constexpr char API_TOKEN_KEY[] = "api_token";
SemaphoreHandle_t configMutex = nullptr;
String apiToken;
}

void configurationBegin() {
  configMutex = xSemaphoreCreateMutex();
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, true)) return;
  apiToken = preferences.getString(API_TOKEN_KEY, "");
  preferences.end();
}

String configurationApiToken() {
  if (!configMutex || xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return "";
  const String token = apiToken;
  xSemaphoreGive(configMutex);
  return token;
}

bool configurationHasApiToken() {
  return configurationApiToken().length() > 0;
}

bool configurationSetApiToken(const String& token, bool clearToken, String& error) {
  String cleanToken = token;
  cleanToken.trim();
  if (clearToken) cleanToken = "";
  if (cleanToken.length() > MAX_API_TOKEN_LENGTH) {
    error = "Token is too long (maximum 512 characters).";
    return false;
  }
  for (size_t i = 0; i < cleanToken.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(cleanToken[i]);
    if (c < 0x21 || c > 0x7e) {
      error = "Token may contain only printable characters without spaces.";
      return false;
    }
  }
  if (cleanToken.isEmpty() && !clearToken) return true;
  if (!configMutex || xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    error = "Configuration is currently busy.";
    return false;
  }
  Preferences preferences;
  const bool opened = preferences.begin(PREFERENCES_NAMESPACE, false);
  const size_t written = opened ? preferences.putString(API_TOKEN_KEY, cleanToken) : 0;
  const bool persisted = opened && written == cleanToken.length() &&
                         preferences.isKey(API_TOKEN_KEY) &&
                         preferences.getString(API_TOKEN_KEY, "") == cleanToken;
  if (opened) preferences.end();
  if (!persisted) {
    xSemaphoreGive(configMutex);
    error = "Failed to save the token to memory.";
    return false;
  }
  apiToken = cleanToken;
  xSemaphoreGive(configMutex);
  return true;
}
