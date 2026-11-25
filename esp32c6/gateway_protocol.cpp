#include "gateway_protocol.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tag_radio.h"

static constexpr size_t MAX_IMAGE_BYTES = 640 * 384 / 8 * 2;
static constexpr size_t MIN_FIRMWARE_BYTES = 8192;
static constexpr size_t MAX_FIRMWARE_BYTES = 196608 - 16384;
static constexpr uint32_t APP_ENTRY = 16384;
static constexpr uint32_t NRF52811_RAM_START = 0x20000000;
static constexpr uint32_t NRF52811_RAM_END = 0x20006000;
static bool radioStarted = false;

static uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (uint8_t b = 0; b < 8; b++) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320 : 0);
  }
  return c ^ 0xFFFFFFFF;
}

static void reply(JsonDocument& document) {
  serializeJson(document, Serial1);
  Serial1.println();
}

static bool parseMac(const char* text, uint8_t out[8]) {
  if (!text || strlen(text) != 23) return false;
  unsigned values[8];
  if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x", &values[0], &values[1],
             &values[2], &values[3], &values[4], &values[5], &values[6],
             &values[7]) != 8)
    return false;
  for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(values[i]);
  return true;
}

static void formatMac(const uint8_t mac[8], char out[24]) {
  snprintf(out, 24, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5], mac[6], mac[7]);
}

static bool profile(uint8_t hw, uint16_t& w, uint16_t& h, uint8_t& rot, bool& bwr) {
  switch (hw) {
    case 0x32: w = 360; h = 184; rot = 90; bwr = true; return true;
    case 0x34: w = 400; h = 300; rot = 0; bwr = true; return true;
    case 0x05: case 0x36: w = 640; h = 384; rot = 0; bwr = true; return true;
    case 0x02: w = 400; h = 300; rot = 0; bwr = false; return true;
    case 0x12: case 0x24: w = 400; h = 300; rot = 0; bwr = true; return true;
    case 0x01: case 0x11: case 0x23: case 0x27: case 0x33: case 0x40:
      w = 296; h = 128; rot = 90; bwr = (hw == 0x11 || hw == 0x23 || hw == 0x33); return true;
    case 0x03: case 0x31: case 0x46:
      w = 212; h = 104; rot = 90; bwr = (hw != 0x46); return true;
    case 0x04: case 0x4E:
      w = 296; h = 152; rot = 90; bwr = (hw == 0x04); return true;
    case 0x22: case 0x47: w = 264; h = 176; rot = 90; bwr = true; return true;
    case 0x26: w = 640; h = 384; rot = 0; bwr = false; return true;
    case 0x00: w = 152; h = 152; rot = 0; bwr = false; return true;
    default: return false;
  }
}

static void listTags() {
  StaticJsonDocument<4096> document;
  document["ok"] = radioStarted;
  document["channel"] = tagRadioChannel();
  document["checkin_minutes"] = tagRadioCheckin();
  if (!radioStarted) document["error"] = "IEEE 802.15.4 radio initialization failed";
  JsonArray tags = document.createNestedArray("tags");
  TagInfo found[TAG_MAX_DEVICES];
  const size_t count = tagGetDevices(found, TAG_MAX_DEVICES);
  for (size_t i = 0; i < count; i++) {
    JsonObject tag = tags.createNestedObject();
    char mac[24];
    formatMac(found[i].mac, mac);
    tag["mac"] = mac;
    if (found[i].hasMetadata) {
      tag["hw_type"] = found[i].hwType;
      uint16_t w, h;
      uint8_t rot;
      bool bwr;
      if (profile(found[i].hwType, w, h, rot, bwr)) {
        const bool reportNative = found[i].hwType == 0x32;
        tag["width"] = !reportNative && (rot == 90 || rot == 270) ? h : w;
        tag["height"] = !reportNative && (rot == 90 || rot == 270) ? w : h;
      }
    } else {
      tag["hw_type"] = nullptr;
      tag["status"] = "discovered";
    }
    tag["normal_job_state"] = tagGetJob(found[i].mac, nullptr);
    tag["wake_reason"] = found[i].wakeReason;
    tag["screen_refresh_pending"] = found[i].screenRefreshPending;
    tag["battery_mv"] = found[i].battMv;
    tag["rssi"] = found[i].rssi;
    tag["checkins"] = found[i].checkins;
    if (found[i].seen) tag["last_seen_ms"] = millis() - found[i].lastSeenMs;
    else tag["last_seen_ms"] = nullptr;
  }
  reply(document);
}

static void rejectImage(const char* reason) {
  Serial.printf("[GW +%lu ms] JOB_REJECT reason=%s\n", static_cast<unsigned long>(millis()), reason);
  StaticJsonDocument<256> error;
  error["ok"] = false;
  error["error"] = reason;
  reply(error);
}

static bool isValidNrf52811Firmware(const uint8_t* image, size_t size) {
  if (!image || size < 8) return false;
  const uint32_t stackPointer = image[0] | (uint32_t(image[1]) << 8) |
                                (uint32_t(image[2]) << 16) | (uint32_t(image[3]) << 24);
  const uint32_t resetVector = image[4] | (uint32_t(image[5]) << 8) |
                               (uint32_t(image[6]) << 16) | (uint32_t(image[7]) << 24);
  const uint32_t resetAddress = resetVector & ~1u;
  return stackPointer >= NRF52811_RAM_START && stackPointer <= NRF52811_RAM_END &&
         (stackPointer & 7u) == 0 && (resetVector & 1u) != 0 &&
         resetAddress >= APP_ENTRY && resetAddress < APP_ENTRY + size;
}

static void handleFirmwareUpdate(JsonObjectConst request) {
  uint8_t mac[8] = {};
  const char* macText = request["mac"] | "";
  const uint32_t size = request["size"] | 0;
  const uint32_t expectedCrc = request["crc32"].as<uint32_t>();
  if (!parseMac(macText, mac)) {
    rejectImage("invalid target MAC");
    return;
  }

  bool foundTag = false;
  TagInfo tags[TAG_MAX_DEVICES];
  const size_t count = tagGetDevices(tags, TAG_MAX_DEVICES);
  for (size_t i = 0; i < count; ++i) {
    if (!memcmp(mac, tags[i].mac, sizeof(mac))) {
      uint16_t w, h;
      uint8_t rot;
      bool bwr;
      foundTag = profile(tags[i].hwType, w, h, rot, bwr);
      break;
    }
  }
  if (!foundTag) {
    rejectImage("unknown or unsupported target tag");
    return;
  }
  if (size < MIN_FIRMWARE_BYTES || size > MAX_FIRMWARE_BYTES) {
    char reason[128];
    snprintf(reason, sizeof(reason), "invalid OTA firmware size: got %lu B, allowed %lu..%lu B",
             static_cast<unsigned long>(size), static_cast<unsigned long>(MIN_FIRMWARE_BYTES),
             static_cast<unsigned long>(MAX_FIRMWARE_BYTES));
    rejectImage(reason);
    return;
  }

  uint8_t* firmware = static_cast<uint8_t*>(malloc(size));
  if (!firmware) {
    rejectImage("out of memory for OTA firmware");
    return;
  }
  Serial.printf("[GW +%lu ms] FW_HEADER mac=%s size=%lu B\n",
                static_cast<unsigned long>(millis()), macText, static_cast<unsigned long>(size));
  StaticJsonDocument<128> ready;
  ready["ok"] = true;
  ready["state"] = "ready";
  ready["size"] = size;
  reply(ready);

  size_t received = 0;
  uint32_t last = millis();
  while (received < size && millis() - last < 60000) {
    while (Serial1.available() && received < size) {
      firmware[received++] = static_cast<uint8_t>(Serial1.read());
      last = millis();
    }
    yield();
  }
  if (received != size) {
    free(firmware);
    rejectImage("firmware transfer timeout");
    return;
  }
  if (crc32(firmware, size) != expectedCrc) {
    free(firmware);
    rejectImage("firmware CRC32 mismatch");
    return;
  }
  if (!isValidNrf52811Firmware(firmware, size)) {
    free(firmware);
    rejectImage("invalid nRF52811 application image; upload the tag-fw OTA BIN, not full-flash BIN");
    return;
  }

  const char* error = nullptr;
  if (!tagQueueImage(mac, firmware, size, 3, &error)) {
    free(firmware);
    rejectImage(error ? error : "firmware queue rejected");
    return;
  }
  Serial.printf("[GW +%lu ms] FW_QUEUED mac=%s size=%lu B\n",
                static_cast<unsigned long>(millis()), macText, static_cast<unsigned long>(size));
  StaticJsonDocument<192> done;
  done["ok"] = true;
  done["state"] = "queued";
  done["size"] = size;
  done["data_type"] = 3;
  reply(done);
}

static void handleLine(char* line) {
  StaticJsonDocument<2048> request;
  if (deserializeJson(request, line)) {
    StaticJsonDocument<128> error;
    error["ok"] = false;
    error["error"] = "invalid JSON";
    reply(error);
    return;
  }
  const char* command = request["cmd"] | "";
  if (!strcmp(command, "list")) {
    listTags();
    return;
  }
  if (!strcmp(command, "put_firmware")) {
    handleFirmwareUpdate(request.as<JsonObjectConst>());
    return;
  }
  if (!strcmp(command, "set_checkin")) {
    const uint16_t minutes = constrain(static_cast<int>(request["minutes"] | GATEWAY_DEFAULT_CHECKIN_MINUTES), 1, 1440);
    tagRadioSetCheckin(minutes);
    StaticJsonDocument<128> response;
    response["ok"] = true;
    response["checkin_minutes"] = minutes;
    reply(response);
    return;
  }
  if (!strcmp(command, "set_tag_checkin")) {
    uint8_t mac[8];
    if (!parseMac(request["mac"], mac)) {
      StaticJsonDocument<128> error;
      error["ok"] = false;
      error["error"] = "invalid MAC";
      reply(error);
      return;
    }
    const uint16_t minutes = constrain(static_cast<int>(request["minutes"] | 0), 0, 1440);
    tagRadioSetTagCheckin(mac, minutes);
    StaticJsonDocument<128> response;
    response["ok"] = true;
    response["mac"] = request["mac"];
    response["checkin_minutes"] = minutes;
    reply(response);
    return;
  }
  if (!strcmp(command, "job")) {
    uint8_t mac[8];
    TagJob job{};
    StaticJsonDocument<256> response;
    const char* target = request["target"] | "normal";
    response["ok"] = parseMac(request["mac"], mac) && !strcmp(target, "normal");
    if (response["ok"]) {
      const TagJobState state = tagGetJob(mac, &job);
      response["state"] = state == JOB_DONE ? "done" : state == JOB_PENDING ? "pending" : "none";
      response["size"] = job.size;
      response["blocks_sent"] = job.blocksSent;
      response["age_ms"] = job.ageMs;
    } else {
      response["error"] = "invalid MAC or unsupported image target";
    }
    reply(response);
    return;
  }
  if (!strcmp(command, "put_image")) {
    uint8_t mac[8] = {};
    uint16_t w, h;
    uint8_t rot;
    bool bwr;
    const char* macText = request["mac"] | "";
    const char* target = request["target"] | "normal";
    if (strcmp(target, "normal")) {
      rejectImage("unsupported image target");
      return;
    }
    const uint8_t arg = 0;
    const uint32_t size = request["size"] | 0;
    const uint8_t dataType = request["data_type"] | 0;
    const uint32_t expectedCrc = request["crc32"].as<uint32_t>();
    if (!parseMac(macText, mac)) {
      rejectImage("invalid target MAC");
      return;
    }

    uint8_t hw = 0;
    bool foundTag = false;
    TagInfo tags[TAG_MAX_DEVICES];
    const size_t count = tagGetDevices(tags, TAG_MAX_DEVICES);
    for (size_t i = 0; i < count; i++) {
      if (!memcmp(mac, tags[i].mac, sizeof(mac))) {
        hw = tags[i].hwType;
        foundTag = true;
        break;
      }
    }
    if (!foundTag) {
      char reason[96];
      snprintf(reason, sizeof(reason), "unknown tag MAC: %s", macText);
      rejectImage(reason);
      return;
    }
    if (!profile(hw, w, h, rot, bwr)) {
      char reason[96];
      snprintf(reason, sizeof(reason), "unsupported tag type: 0x%02X", hw);
      rejectImage(reason);
      return;
    }

    const uint8_t expectedType = bwr ? 0x21 : 0x20;
    if (!size || size > MAX_IMAGE_BYTES) {
      char reason[112];
      snprintf(reason, sizeof(reason), "invalid image size: got %lu B, allowed 1..%lu B",
               static_cast<unsigned long>(size), static_cast<unsigned long>(MAX_IMAGE_BYTES));
      rejectImage(reason);
      return;
    }
    if (dataType != expectedType) {
      char reason[112];
      snprintf(reason, sizeof(reason), "image type mismatch: got 0x%02X, expected 0x%02X",
               dataType, expectedType);
      rejectImage(reason);
      return;
    }

    const size_t nativeW = rot == 90 || rot == 270 ? h : w;
    const size_t nativeH = rot == 90 || rot == 270 ? w : h;
    const size_t expectedSize = ((nativeW + 7) / 8) * nativeH * (bwr ? 2 : 1);
    if (size != expectedSize) {
      char reason[128];
      snprintf(reason, sizeof(reason), "image byte length mismatch: got %lu B, expected %lu B",
               static_cast<unsigned long>(size), static_cast<unsigned long>(expectedSize));
      rejectImage(reason);
      return;
    }

    Serial.printf("[GW +%lu ms] JOB_HEADER mac=%s target=%s size=%lu B data_type=0x%02X hw=0x%02X\n",
                  static_cast<unsigned long>(millis()), macText, target, static_cast<unsigned long>(size),
                  dataType, hw);
    uint8_t* image = static_cast<uint8_t*>(malloc(size));
    if (!image) {
      StaticJsonDocument<128> error;
      error["ok"] = false;
      error["error"] = "out of memory";
      reply(error);
      return;
    }
    StaticJsonDocument<128> ready;
    ready["ok"] = true;
    ready["state"] = "ready";
    ready["size"] = size;
    reply(ready);
    size_t received = 0;
    uint32_t last = millis();
    while (received < size && millis() - last < 30000) {
      while (Serial1.available() && received < size) {
        image[received++] = static_cast<uint8_t>(Serial1.read());
        last = millis();
      }
      yield();
    }
    if (received != size) {
      free(image);
      rejectImage("image transfer timeout");
      return;
    }
    if (crc32(image, size) != expectedCrc) {
      free(image);
      rejectImage("image CRC32 mismatch");
      return;
    }
    Serial.printf("[GW +%lu ms] JOB_BODY_OK mac=%s size=%lu B crc32=0x%08lX\n",
                  static_cast<unsigned long>(millis()), macText, static_cast<unsigned long>(size),
                  static_cast<unsigned long>(expectedCrc));
    const char* error = nullptr;
    if (!tagQueueImage(mac, image, size, dataType, &error, arg)) {
      free(image);
      rejectImage(error ? error : "queue rejected");
      return;
    }
    Serial.printf("[GW +%lu ms] JOB_QUEUED mac=%s target=%s size=%lu B data_type=0x%02X\n",
                  static_cast<unsigned long>(millis()), macText, target, static_cast<unsigned long>(size),
                  dataType);
    StaticJsonDocument<192> done;
    done["ok"] = true;
    done["state"] = "queued";
    done["size"] = size;
    done["crc32"] = expectedCrc;
    reply(done);
    return;
  }
  StaticJsonDocument<128> error;
  error["ok"] = false;
  error["error"] = "unknown command";
  reply(error);
}

void gatewayProtocolBegin(bool isRadioStarted) {
  radioStarted = isRadioStarted;
  StaticJsonDocument<128> response;
  response["ok"] = radioStarted;
  response["service"] = "OpenTag Gateway";
  response["channel"] = GATEWAY_RADIO_CHANNEL;
  if (!radioStarted) response["error"] = "IEEE 802.15.4 radio initialization failed";
  reply(response);
}

void gatewayProtocolHandleLine(char* line) {
  handleLine(line);
}
