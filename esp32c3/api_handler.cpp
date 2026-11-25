#include "api_handler.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>

#include "config.h"
#include "config_manager.h"
#include "uart_handler.h"

namespace {
constexpr uint32_t POLL_TIMEOUT_MS = 30000;
constexpr uint32_t IMAGE_TIMEOUT_MS = 90000;
constexpr uint32_t IMAGE_TRANSFER_TIMEOUT_MS = 60000;
constexpr size_t STATUS_EVENT_CAPACITY = 12;
constexpr size_t STATUS_MESSAGE_CAPACITY = 160;
constexpr size_t STATUS_TAG_CAPACITY = 16;

enum class StatusLevel : uint8_t { INFO, SUCCESS, ERROR };

struct StatusEvent {
  uint32_t atMs;
  StatusLevel level;
  char message[STATUS_MESSAGE_CAPACITY];
};

struct StatusTag {
  char mac[24];
  int16_t rssiDbm;
  bool hasRssi;
};

class C6UartTransaction {
 public:
  bool acquire(uint32_t timeoutMs) {
    locked_ = c6UartLock(timeoutMs);
    return locked_;
  }

  ~C6UartTransaction() {
    if (locked_) c6UartUnlock();
  }

 private:
  bool locked_ = false;
};

SemaphoreHandle_t statusMutex = nullptr;
String currentStatus = "Waiting for connection.";
StatusLevel currentStatusLevel = StatusLevel::INFO;
String lastImageStatus = "No image has been processed yet.";
StatusLevel lastImageStatusLevel = StatusLevel::INFO;
uint32_t lastImageStatusAtMs = 0;
StatusTag statusTags[STATUS_TAG_CAPACITY]{};
size_t statusTagCount = 0;
uint32_t statusTagsUpdatedAtMs = 0;
bool statusTagsKnown = false;
StatusEvent statusEvents[STATUS_EVENT_CAPACITY]{};
size_t nextStatusEvent = 0;
size_t statusEventCount = 0;

const char* statusLevelName(StatusLevel level) {
  switch (level) {
    case StatusLevel::SUCCESS:
      return "success";
    case StatusLevel::ERROR:
      return "error";
    default:
      return "info";
  }
}

void setStatus(const String& status, StatusLevel level = StatusLevel::INFO, bool imageStage = false) {
  if (!statusMutex || xSemaphoreTake(statusMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
  if (imageStage) {
    lastImageStatus = status;
    lastImageStatusLevel = level;
    lastImageStatusAtMs = millis();
  }
  if (currentStatus == status && currentStatusLevel == level) {
    xSemaphoreGive(statusMutex);
    return;
  }
  currentStatus = status;
  currentStatusLevel = level;
  StatusEvent& event = statusEvents[nextStatusEvent];
  event.atMs = millis();
  event.level = level;
  const size_t messageLength = min(status.length(), sizeof(event.message) - 1);
  memcpy(event.message, status.c_str(), messageLength);
  event.message[messageLength] = '\0';
  nextStatusEvent = (nextStatusEvent + 1) % STATUS_EVENT_CAPACITY;
  if (statusEventCount < STATUS_EVENT_CAPACITY) ++statusEventCount;
  xSemaphoreGive(statusMutex);
}

void updateTagStatus(JsonArrayConst tags) {
  if (!statusMutex || xSemaphoreTake(statusMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
  statusTagCount = 0;
  for (JsonObjectConst tag : tags) {
    if (statusTagCount >= STATUS_TAG_CAPACITY) break;
    const char* mac = tag["mac"] | "";
    if (!*mac) continue;
    StatusTag& statusTag = statusTags[statusTagCount++];
    snprintf(statusTag.mac, sizeof(statusTag.mac), "%s", mac);
    const JsonVariantConst rssi = tag["rssi"];
    statusTag.hasRssi = rssi.is<int>();
    statusTag.rssiDbm = statusTag.hasRssi ? static_cast<int16_t>(rssi.as<int>()) : 0;
  }
  statusTagsUpdatedAtMs = millis();
  statusTagsKnown = true;
  xSemaphoreGive(statusMutex);
}

String apiUrl(const String& path) {
  String base = OPENTAG_API_BASE_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base + path;
}

bool jsonRequest(const char* method, const String& path, const String& token, const String& payload,
                 uint32_t timeoutMs, JsonDocument* response, String& error) {
  WiFiClientSecure client;
  client.setCACert(OPENTAG_API_ROOT_CA);
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, apiUrl(path))) {
    error = "Unable to start HTTPS request.";
    return false;
  }
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Accept", "application/json");
  int statusCode;
  if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    statusCode = http.POST(payload);
  } else {
    statusCode = http.GET();
  }
  if (statusCode < 200 || statusCode >= 300) {
    error = statusCode < 0 ? "API is unavailable." : "API returned HTTP " + String(statusCode) + ".";
    http.end();
    return false;
  }
  if (response) {
    const DeserializationError parseError = deserializeJson(*response, http.getStream());
    if (parseError || !response->is<JsonObject>()) {
      error = "API returned invalid JSON.";
      http.end();
      return false;
    }
  }
  http.end();
  return true;
}

bool postJson(const String& path, const String& token, JsonDocument& request,
              JsonDocument* response, String& error) {
  String payload;
  serializeJson(request, payload);
  return jsonRequest("POST", path, token, payload, POLL_TIMEOUT_MS, response, error);
}

uint32_t imageCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
  }
  return crc ^ 0xFFFFFFFFu;
}

class ImageBufferStream final : public Stream {
 public:
  ImageBufferStream(uint8_t* buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

  size_t write(uint8_t value) override {
    return write(&value, 1);
  }

  size_t write(const uint8_t* data, size_t length) override {
    const size_t available = capacity_ - length_;
    const size_t count = min(length, available);
    if (count) {
      memcpy(buffer_ + length_, data, count);
      length_ += count;
    }
    return count;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  size_t length() const { return length_; }

 private:
  uint8_t* buffer_;
  size_t capacity_;
  size_t length_ = 0;
};

String sha256Hex(const uint8_t* data, size_t length) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  uint8_t digest[32];
  const bool hashed = mbedtls_sha256_starts(&context, 0) == 0 &&
                      mbedtls_sha256_update(&context, data, length) == 0 &&
                      mbedtls_sha256_finish(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  if (!hashed) return "";
  static constexpr char hex[] = "0123456789abcdef";
  char result[65];
  for (size_t i = 0; i < sizeof(digest); ++i) {
    result[i * 2] = hex[digest[i] >> 4];
    result[i * 2 + 1] = hex[digest[i] & 0x0f];
  }
  result[64] = '\0';
  return String(result);
}

bool validJobId(const char* id) {
  if (!id || strlen(id) != 32) return false;
  for (size_t i = 0; i < 32; ++i)
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  return true;
}

bool downloadImage(const String& id, const String& token, size_t expectedSize,
                   const String& expectedSha256, uint8_t*& image, String& error) {
  setStatus("Downloading image from API.", StatusLevel::INFO, true);
  if (!expectedSize || expectedSize > MAX_IMAGE_BYTES) {
    error = "Invalid image size.";
    return false;
  }
  image = static_cast<uint8_t*>(malloc(expectedSize));
  if (!image) {
    error = "Not enough memory for image.";
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(OPENTAG_API_ROOT_CA);
  client.setTimeout(3000);
  HTTPClient http;
  http.setTimeout(IMAGE_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, apiUrl("/jobs/" + id + "/image"))) {
    error = "Unable to start image download.";
    free(image);
    image = nullptr;
    return false;
  }
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Accept", "application/octet-stream");
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    error = statusCode < 0 ? "Image download from API failed." : "API returned HTTP " + String(statusCode) + " while downloading the image.";
    http.end();
    free(image);
    image = nullptr;
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength >= 0 && static_cast<size_t>(contentLength) != expectedSize) {
    error = "Image size from API does not match job metadata.";
    http.end();
    free(image);
    image = nullptr;
    return false;
  }
  ImageBufferStream imageStream(image, expectedSize);
  const int received = http.writeToStream(&imageStream);
  http.end();
  if (received < 0) {
    error = "Image transfer failed: " + HTTPClient::errorToString(received);
    free(image);
    image = nullptr;
    return false;
  }
  if (static_cast<size_t>(received) != expectedSize || imageStream.length() != expectedSize) {
    error = "Downloaded image size does not match job metadata.";
    free(image);
    image = nullptr;
    return false;
  }
  setStatus("Image downloaded, verifying SHA-256.", StatusLevel::INFO, true);
  const String actualSha256 = sha256Hex(image, expectedSize);
  if (actualSha256.isEmpty() || (!expectedSha256.isEmpty() && !actualSha256.equalsIgnoreCase(expectedSha256))) {
    error = "Image SHA-256 does not match the API value.";
    free(image);
    image = nullptr;
    return false;
  }
  return true;
}

bool pushImage(JsonObjectConst job, const String& token, String& error) {
  setStatus("Processing new image.", StatusLevel::INFO, true);
  const char* jobId = job["id"] | "";
  const char* mac = job["mac"] | "";
  const char* target = job["target"] | "normal";
  if (!validJobId(jobId) || !*mac || strlen(mac) != 23) {
    error = "API returned invalid image metadata.";
    return false;
  }
  const size_t imageSize = job["size"] | 0u;
  const uint8_t dataType = job["data_type"] | 0;
  uint8_t* image = nullptr;
  if (!downloadImage(jobId, token, imageSize, String(job["sha256"] | ""), image, error)) return false;
  setStatus("Image verified, sending header to C6.", StatusLevel::INFO, true);

  DynamicJsonDocument header(384);
  header["cmd"] = "put_image";
  header["mac"] = mac;
  header["target"] = target;
  header["size"] = imageSize;
  header["data_type"] = dataType;
  header["crc32"] = imageCrc32(image, imageSize);
  String headerLine;
  serializeJson(header, headerLine);

  C6UartTransaction uartTransaction;
  if (!uartTransaction.acquire(30000)) {
    error = "ESP32-C6 UART is currently busy.";
    free(image);
    return false;
  }

  DynamicJsonDocument c6Ready(512);
  if (!c6UartCommandLocked(headerLine, C6_REPLY_IMAGE_READY, c6Ready, 30000)) {
    error = "ESP32-C6 did not respond to the image header.";
    free(image);
    return false;
  }
  if (!c6Ready["ok"].as<bool>() || c6Ready["state"] != "ready") {
    error = String("ESP32-C6 rejected the image: ") + String(c6Ready["error"] | "unknown reason");
    free(image);
    return false;
  }
  setStatus("C6 is ready, sending image data.", StatusLevel::INFO, true);
  if (!c6UartSendImageLocked(image, imageSize)) {
    error = "Failed to send image data to ESP32-C6.";
    free(image);
    return false;
  }
  free(image);

  setStatus("Image data sent, waiting for C6 confirmation.", StatusLevel::INFO, true);
  DynamicJsonDocument c6Done(512);
  if (!c6UartWaitResponseLocked(C6_REPLY_IMAGE_QUEUED, c6Done, IMAGE_TRANSFER_TIMEOUT_MS)) {
    error = "ESP32-C6 did not confirm the image was queued.";
    return false;
  }
  if (!c6Done["ok"].as<bool>()) {
    error = String("ESP32-C6 did not queue the image: ") + String(c6Done["error"] | "unknown reason");
    return false;
  }

  setStatus("C6 confirmed the image was queued, confirming the job with the API.", StatusLevel::INFO, true);
  DynamicJsonDocument queuedRequest(768);
  queuedRequest["c6_state"].set(c6Done);
  if (!postJson(String("/jobs/") + jobId + "/queued", token, queuedRequest, nullptr, error)) return false;
  setStatus("Image successfully queued on C6 and confirmed by the API.", StatusLevel::SUCCESS, true);
  return true;
}

bool markJobFailed(const String& jobId, const String& token, const String& reason, String& error) {
  DynamicJsonDocument request(2304);
  request["error"] = reason.substring(0, 2000);
  return postJson("/jobs/" + jobId + "/failed", token, request, nullptr, error);
}

bool synchronizeOnce(String& resultMessage) {
  const String token = configurationApiToken();
  if (token.isEmpty()) {
    resultMessage = "No API token is configured.";
    return false;
  }
  setStatus("Loading tag list from C6.");
  DynamicJsonDocument listRequest(64);
  listRequest["cmd"] = "list";
  String listLine;
  serializeJson(listRequest, listLine);
  DynamicJsonDocument gateway(8192);
  if (!c6UartCommand(listLine, C6_REPLY_LIST, gateway, 30000)) {
    resultMessage = "ESP32-C6 did not respond to the list command.";
    return false;
  }
  if (!gateway["ok"].as<bool>()) {
    resultMessage = String("ESP32-C6: ") + String(gateway["error"] | "list command failed");
    return false;
  }
  const size_t tagCount = gateway["tags"].as<JsonArrayConst>().size();
  updateTagStatus(gateway["tags"].as<JsonArrayConst>());
  setStatus("C6 responded, tags: " + String(tagCount) + ". Sending status to the API.");

  DynamicJsonDocument pollRequest(8192);
  pollRequest["tags"].set(gateway["tags"]);
  pollRequest["channel"].set(gateway["channel"]);
  pollRequest["checkin_minutes"].set(gateway["checkin_minutes"]);
  pollRequest["ip_address"] = WiFi.localIP().toString();
  DynamicJsonDocument pollResponse(12288);
  String error;
  if (!postJson("/gateway/poll", token, pollRequest, &pollResponse, error)) {
    resultMessage = error;
    return false;
  }
  if (!pollResponse["ok"].as<bool>()) {
    resultMessage = "API rejected synchronization.";
    return false;
  }
  const size_t jobTotal = pollResponse["jobs"].as<JsonArrayConst>().size();
  setStatus("API poll complete, jobs to forward: " + String(jobTotal) + ".");

  const uint16_t checkinMinutes = pollResponse["checkin_minutes"] | 0;
  if (checkinMinutes > 0) {
    setStatus("Setting the check-in interval on C6.");
    DynamicJsonDocument request(96), response(256);
    request["cmd"] = "set_checkin";
    request["minutes"] = checkinMinutes;
    String line;
    serializeJson(request, line);
    if (!c6UartCommand(line, C6_REPLY_ANY, response, 30000) || !response["ok"].as<bool>()) {
      resultMessage = "ESP32-C6 rejected the check-in interval.";
      return false;
    }
  }

  for (JsonObjectConst setting : pollResponse["tag_checkins"].as<JsonArrayConst>()) {
    const char* mac = setting["mac"] | "";
    if (!*mac) continue;
    DynamicJsonDocument request(160), response(256);
    setStatus("Setting the tag check-in interval on C6.");
    request["cmd"] = "set_tag_checkin";
    request["mac"] = mac;
    request["minutes"] = setting["checkin_minutes"] | 0;
    String line;
    serializeJson(request, line);
    if (!c6UartCommand(line, C6_REPLY_ANY, response, 30000) || !response["ok"].as<bool>()) {
      resultMessage = "ESP32-C6 rejected the tag check-in interval.";
      return false;
    }
  }

  uint16_t jobCount = 0;
  uint16_t failedJobCount = 0;
  size_t jobIndex = 0;
  for (JsonObjectConst job : pollResponse["jobs"].as<JsonArrayConst>()) {
    ++jobIndex;
    setStatus("Processing image " + String(jobIndex) + "/" + String(jobTotal) + ".");
    const String jobId = job["id"] | "";
    String jobError;
    if (pushImage(job, token, jobError)) {
      ++jobCount;
      continue;
    }
    setStatus("Image forwarding failed: " + jobError, StatusLevel::ERROR, true);
    if (!validJobId(jobId.c_str())) {
      resultMessage = jobError;
      return false;
    }
    String failedError;
    if (!markJobFailed(jobId, token, jobError, failedError)) {
      resultMessage = jobError + " Failed to mark the job as failed: " + failedError;
      return false;
    }
    ++failedJobCount;
  }
  resultMessage = "Synchronization complete, tags: " + String(tagCount) +
                  ", jobs: " + String(jobTotal) +
                  ", queued: " + String(jobCount) +
                  ", failed: " + String(failedJobCount) + ".";
  return failedJobCount == 0;
}

void apiWorkerTask(void*) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      setStatus("Waiting for Wi-Fi.");
    } else {
      setStatus("Starting synchronization.");
      String message;
      const bool success = synchronizeOnce(message);
      setStatus(message, success ? StatusLevel::SUCCESS : StatusLevel::ERROR);
    }
    vTaskDelay(pdMS_TO_TICKS(API_POLL_INTERVAL_SECONDS * 1000u));
  }
}
}

void apiHandlerBegin() {
  statusMutex = xSemaphoreCreateMutex();
  if (!statusMutex) return;
  setStatus("Waiting for the first synchronization.");
  if (xTaskCreate(apiWorkerTask, "api-sync", 12288, nullptr, 1, nullptr) != pdPASS)
    setStatus("Failed to start the synchronization task.", StatusLevel::ERROR);
}

void apiHandlerAppendStatus(JsonDocument& response) {
  if (!statusMutex || xSemaphoreTake(statusMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    response["status"] = "Status is unavailable.";
    response["status_level"] = "error";
    response["tags_known"] = false;
    response.createNestedArray("tags");
    response.createNestedArray("events");
    return;
  }
  response["status"] = currentStatus;
  response["status_level"] = statusLevelName(currentStatusLevel);
  response["last_image_status"] = lastImageStatus;
  response["last_image_level"] = statusLevelName(lastImageStatusLevel);
  response["last_image_known"] = lastImageStatusAtMs != 0;
  response["last_image_age_seconds"] =
      lastImageStatusAtMs ? static_cast<uint32_t>(millis() - lastImageStatusAtMs) / 1000u : 0;
  response["tags_known"] = statusTagsKnown;
  response["tags_age_seconds"] =
      statusTagsKnown ? static_cast<uint32_t>(millis() - statusTagsUpdatedAtMs) / 1000u : 0;
  JsonArray tags = response.createNestedArray("tags");
  for (size_t i = 0; i < statusTagCount; ++i) {
    JsonObject item = tags.createNestedObject();
    item["mac"] = statusTags[i].mac;
    if (statusTags[i].hasRssi) item["rssi_dbm"] = statusTags[i].rssiDbm;
    else item["rssi_dbm"] = nullptr;
  }
  JsonArray events = response.createNestedArray("events");
  const size_t oldestEvent = (nextStatusEvent + STATUS_EVENT_CAPACITY - statusEventCount) % STATUS_EVENT_CAPACITY;
  for (size_t i = 0; i < statusEventCount; ++i) {
    const StatusEvent& event = statusEvents[(oldestEvent + i) % STATUS_EVENT_CAPACITY];
    JsonObject item = events.createNestedObject();
    item["age_seconds"] = static_cast<uint32_t>(millis() - event.atMs) / 1000u;
    item["level"] = statusLevelName(event.level);
    item["message"] = event.message;
  }
  xSemaphoreGive(statusMutex);
}
