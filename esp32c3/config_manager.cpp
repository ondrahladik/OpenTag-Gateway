#include "config_manager.h"

#include <Preferences.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <string.h>

#include "config.h"

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "opentag";
constexpr char API_TOKEN_KEY[] = "api_token";
constexpr char UPDATE_PASSWORD_KEY[] = "update_password";
constexpr size_t UPDATE_PASSWORD_SALT_SIZE = 16;
constexpr size_t UPDATE_PASSWORD_HASH_SIZE = 32;
constexpr size_t UPDATE_PASSWORD_RECORD_SIZE = UPDATE_PASSWORD_SALT_SIZE + UPDATE_PASSWORD_HASH_SIZE;
constexpr uint32_t UPDATE_PASSWORD_ITERATIONS = 100000;
constexpr size_t MIN_UPDATE_PASSWORD_LENGTH = 8;
constexpr size_t MAX_UPDATE_PASSWORD_LENGTH = 128;
SemaphoreHandle_t configMutex = nullptr;
String apiToken;
bool configurationLoaded = false;
bool updatePasswordConfigured = false;
uint8_t updatePasswordRecord[UPDATE_PASSWORD_RECORD_SIZE]{};

bool hashUpdatePassword(const String& password, const uint8_t* salt, uint8_t* hash) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(
             MBEDTLS_MD_SHA256,
             reinterpret_cast<const unsigned char*>(password.c_str()), password.length(), salt,
             UPDATE_PASSWORD_SALT_SIZE, UPDATE_PASSWORD_ITERATIONS, UPDATE_PASSWORD_HASH_SIZE,
             hash) == 0;
}
}

void configurationBegin() {
  configMutex = xSemaphoreCreateMutex();
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, true)) return;
  apiToken = preferences.getString(API_TOKEN_KEY, "");
  if (preferences.getBytesLength(UPDATE_PASSWORD_KEY) == UPDATE_PASSWORD_RECORD_SIZE &&
      preferences.getBytes(UPDATE_PASSWORD_KEY, updatePasswordRecord,
                           UPDATE_PASSWORD_RECORD_SIZE) == UPDATE_PASSWORD_RECORD_SIZE) {
    updatePasswordConfigured = true;
  }
  preferences.end();
  configurationLoaded = true;
}

String configurationApiToken() {
  String token;
  configurationReadApiToken(token);
  return token;
}

bool configurationReadApiToken(String& token) {
  if (!configurationLoaded || !configMutex ||
      xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return false;
  token = apiToken;
  xSemaphoreGive(configMutex);
  return true;
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
  if (!configurationLoaded) {
    error = "Configuration is unavailable.";
    return false;
  }
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

bool configurationHasUpdatePassword() {
  if (!configurationLoaded || !configMutex ||
      xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return false;
  const bool configured = updatePasswordConfigured;
  xSemaphoreGive(configMutex);
  return configured;
}

bool configurationSetUpdatePassword(const String& password, String& error) {
  if (password.length() < MIN_UPDATE_PASSWORD_LENGTH ||
      password.length() > MAX_UPDATE_PASSWORD_LENGTH) {
    error = "Update password must be 8 to 128 characters long.";
    return false;
  }
  for (size_t i = 0; i < password.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(password[i]);
    if (c < 0x20 || c == 0x7F) {
      error = "Update password cannot contain control characters.";
      return false;
    }
  }
  if (!configurationLoaded || !configMutex ||
      xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    error = "Configuration is currently unavailable.";
    return false;
  }

  uint8_t record[UPDATE_PASSWORD_RECORD_SIZE];
  for (size_t i = 0; i < UPDATE_PASSWORD_SALT_SIZE; i += sizeof(uint32_t)) {
    const uint32_t randomValue = esp_random();
    for (size_t byte = 0; byte < sizeof(uint32_t); ++byte)
      record[i + byte] = static_cast<uint8_t>(randomValue >> (byte * 8));
  }
  if (!hashUpdatePassword(password, record, record + UPDATE_PASSWORD_SALT_SIZE)) {
    xSemaphoreGive(configMutex);
    error = "Failed to hash the update password.";
    return false;
  }

  Preferences preferences;
  const bool opened = preferences.begin(PREFERENCES_NAMESPACE, false);
  const size_t written = opened
                             ? preferences.putBytes(UPDATE_PASSWORD_KEY, record,
                                                    UPDATE_PASSWORD_RECORD_SIZE)
                             : 0;
  uint8_t verified[UPDATE_PASSWORD_RECORD_SIZE]{};
  const bool persisted =
      opened && written == UPDATE_PASSWORD_RECORD_SIZE &&
      preferences.getBytesLength(UPDATE_PASSWORD_KEY) == UPDATE_PASSWORD_RECORD_SIZE &&
      preferences.getBytes(UPDATE_PASSWORD_KEY, verified, UPDATE_PASSWORD_RECORD_SIZE) ==
          UPDATE_PASSWORD_RECORD_SIZE &&
      memcmp(record, verified, UPDATE_PASSWORD_RECORD_SIZE) == 0;
  if (opened) preferences.end();
  if (!persisted) {
    xSemaphoreGive(configMutex);
    error = "Failed to save the update password.";
    return false;
  }
  memcpy(updatePasswordRecord, record, UPDATE_PASSWORD_RECORD_SIZE);
  updatePasswordConfigured = true;
  xSemaphoreGive(configMutex);
  return true;
}

bool configurationVerifyUpdatePassword(const String& password) {
  if (!configurationLoaded || !configMutex ||
      xSemaphoreTake(configMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return false;
  uint8_t record[UPDATE_PASSWORD_RECORD_SIZE];
  const bool configured = updatePasswordConfigured;
  if (configured) memcpy(record, updatePasswordRecord, sizeof(record));
  xSemaphoreGive(configMutex);
  if (!configured) return false;

  uint8_t actualHash[UPDATE_PASSWORD_HASH_SIZE];
  if (!hashUpdatePassword(password, record, actualHash)) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < UPDATE_PASSWORD_HASH_SIZE; ++i)
    difference |= actualHash[i] ^ record[UPDATE_PASSWORD_SALT_SIZE + i];
  return difference == 0;
}
