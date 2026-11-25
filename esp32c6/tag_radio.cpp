#include "tag_radio.h"

#include <esp_ieee802154.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mbedtls/md5.h>


namespace {

constexpr uint16_t PAN_ID = 0x4447;

constexpr uint8_t PKT_AVAIL_DATA_SHORTREQ = 0xE3;
constexpr uint8_t PKT_BLOCK_REQUEST = 0xE4;
constexpr uint8_t PKT_AVAIL_DATA_REQ = 0xE5;
constexpr uint8_t PKT_AVAIL_DATA_INFO = 0xE6;
constexpr uint8_t PKT_BLOCK_PARTIAL_REQUEST = 0xE7;
constexpr uint8_t PKT_BLOCK_PART = 0xE8;
constexpr uint8_t PKT_BLOCK_REQUEST_ACK = 0xE9;
constexpr uint8_t PKT_XFER_COMPLETE = 0xEA;
constexpr uint8_t PKT_XFER_COMPLETE_ACK = 0xEB;
constexpr uint8_t PKT_CANCEL_XFER = 0xEC;
constexpr uint8_t PKT_PING = 0xED;
constexpr uint8_t PKT_PONG = 0xEE;

constexpr uint8_t DATATYPE_NOUPDATE = 0;
constexpr uint8_t DATATYPE_FW_UPDATE = 3;
constexpr uint8_t DATATYPE_IMG_RAW_1BPP = 0x20;
constexpr uint8_t DATATYPE_IMG_RAW_2BPP = 0x21;
constexpr uint8_t DATATYPE_IMG_ZLIB = 0x30;
constexpr uint8_t WAKEUP_REASON_FIRSTBOOT = 0xFC;

constexpr size_t NORMAL_HDR = 21;  // FCF(2) seq(1) pan(2) dst(8) src(8)
constexpr size_t BCAST_HDR = 17;   // FCF(2) seq(1) dstPan(2) dstAddr(2) srcPan(2) src(8)
constexpr size_t BLOCK_DATA_SIZE = 4096;
constexpr size_t PART_DATA_SIZE = 99;
constexpr size_t MAX_PARTS = 42;
constexpr size_t MAX_JOBS = 12;
constexpr uint16_t BLOCK_WAIT_MS = 20;
constexpr uint16_t BLOCK_WAIT_MARGIN_MS = 4;

struct __attribute__((packed)) AvailDataInfo {
    uint8_t checksum;
    uint8_t dataVer[8];
    uint32_t dataSize;
    uint8_t dataType;
    uint8_t dataTypeArgument;
    uint16_t nextCheckIn;
};

struct __attribute__((packed)) BlockRequest {
    uint8_t checksum;
    uint8_t ver[8];
    uint8_t blockId;
    uint8_t type;
    uint8_t requestedParts[6];
};

struct __attribute__((packed)) BlockRequestAck {
    uint8_t checksum;
    uint16_t pleaseWaitMs;
};

static_assert(sizeof(AvailDataInfo) == 17, "AvailDataInfo");
static_assert(sizeof(BlockRequest) == 17, "BlockRequest");
static_assert(sizeof(BlockRequestAck) == 3, "BlockRequestAck");

struct RxItem {
    uint8_t len;  
    int8_t rssi;
    uint8_t lqi;
    int64_t t;
    uint8_t buf[125];
};

struct Job {
    bool used;
    bool busy;
    bool inTransfer;
    bool selected;
    uint8_t mac[8];
    uint8_t ver[8];
    uint8_t* data;
    size_t size;
    uint8_t type;
    uint8_t dataTypeArgument;
    TagJobState state;
    uint16_t blocks;
    uint32_t created;
};

struct TagSlot {
    bool used;
    TagInfo t;
};

SemaphoreHandle_t gLock;
SemaphoreHandle_t gTxDone;
QueueHandle_t gRxQueue;
volatile bool gTxOk = false;
volatile esp_ieee802154_tx_error_t gLastTxError = ESP_IEEE802154_TX_ERR_NONE;
volatile esp_err_t gLastTxSubmitError = ESP_OK;
volatile bool gTxTimedOut = false;

TagSlot gTags[TAG_MAX_DEVICES];
Job gJobs[MAX_JOBS];

uint8_t gApMac[8];
uint8_t gChannel = 25;
volatile uint16_t gCheckin = 5;
uint8_t gSeq = 0;
int64_t gLastBurstEnd = 0;
bool gPingLogged = false;

uint8_t gTxBuf[128];
uint8_t gBlock[BLOCK_DATA_SIZE + 4 + PART_DATA_SIZE];

struct Guard {
    Guard() { xSemaphoreTake(gLock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(gLock); }
};

uint8_t sumBytes(const uint8_t* p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}

bool radioCall(const char* operation, esp_err_t result) {
    if (result == ESP_OK) return true;
    Serial.printf("802.15.4: %s failed: %s (%d)\n", operation, esp_err_to_name(result), (int)result);
    return false;
}

const char* txErrorName(esp_ieee802154_tx_error_t error) {
    switch (error) {
        case ESP_IEEE802154_TX_ERR_NONE: return "NONE";
        case ESP_IEEE802154_TX_ERR_CCA_BUSY: return "CCA_BUSY";
        case ESP_IEEE802154_TX_ERR_ABORT: return "ABORT";
        case ESP_IEEE802154_TX_ERR_NO_ACK: return "NO_ACK";
        case ESP_IEEE802154_TX_ERR_INVALID_ACK: return "INVALID_ACK";
        case ESP_IEEE802154_TX_ERR_COEXIST: return "COEXIST";
        case ESP_IEEE802154_TX_ERR_SECURITY: return "SECURITY";
        default: return "UNKNOWN";
    }
}

// ---- 802.15.4 callbacks (ISR) ----

void IRAM_ATTR onRxDone(uint8_t* data, esp_ieee802154_frame_info_t* info) {
    const uint8_t phr = data[0];
    BaseType_t woken = pdFALSE;
    if (phr >= 5 && phr <= 127) {
        RxItem it;
        it.len = phr - 2;
        it.rssi = info->rssi;
        it.lqi = info->lqi;
        it.t = esp_timer_get_time();
        memcpy(it.buf, data + 1, it.len > sizeof(it.buf) ? sizeof(it.buf) : it.len);
        if (it.len > sizeof(it.buf)) it.len = sizeof(it.buf);
        xQueueSendFromISR(gRxQueue, &it, &woken);
    }
    esp_ieee802154_receive_handle_done(data);
    if (woken) portYIELD_FROM_ISR();
}

void IRAM_ATTR onRxSfd() {}

void IRAM_ATTR onTxDone(const uint8_t*, const uint8_t* ack, esp_ieee802154_frame_info_t*) {
    if (ack) esp_ieee802154_receive_handle_done(ack);
    gTxOk = true;
    gLastTxError = ESP_IEEE802154_TX_ERR_NONE;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(gTxDone, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void IRAM_ATTR onTxFailed(const uint8_t*, esp_ieee802154_tx_error_t error) {
    gTxOk = false;
    gLastTxError = error;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(gTxDone, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void IRAM_ATTR onTxSfd(uint8_t*) {}
void IRAM_ATTR onEd(int8_t) {}
esp_err_t IRAM_ATTR onEnhAck(uint8_t*, esp_ieee802154_frame_info_t*, uint8_t*) { return ESP_FAIL; }

// ---- Transmission ----

bool txRaw() {
    gLastTxSubmitError = ESP_OK;
    gLastTxError = ESP_IEEE802154_TX_ERR_NONE;
    gTxTimedOut = false;
    for (int attempt = 0; attempt < 4; attempt++) {
        xSemaphoreTake(gTxDone, 0);
        gTxOk = false;
        const esp_err_t submitError = esp_ieee802154_transmit(gTxBuf, false);
        if (submitError != ESP_OK) {
            gLastTxSubmitError = submitError;
            delayMicroseconds(400);
            continue;
        }
        if (xSemaphoreTake(gTxDone, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (gTxOk) return true;
        } else {
            gTxTimedOut = true;
        }
    }
    Serial.printf("802.15.4 TX failed: type=0x%02X len=%u state=%d submit=%s tx_error=%s(%d) timeout=%u\n",
                  gTxBuf[1 + NORMAL_HDR], gTxBuf[0], (int)esp_ieee802154_get_state(),
                  esp_err_to_name((esp_err_t)gLastTxSubmitError), txErrorName(gLastTxError),
                  (int)gLastTxError, (unsigned)gTxTimedOut);
    return false;
}

bool sendNormal(const uint8_t* dst, uint8_t type, const uint8_t* payload, size_t plen) {
    if (NORMAL_HDR + 1 + plen + 2 > 127) return false;
    uint8_t* f = gTxBuf + 1;
    f[0] = 0x41;  // data, PAN ID compression
    f[1] = 0xCC;  // dst long, src long
    f[2] = gSeq++;
    f[3] = PAN_ID & 0xFF;
    f[4] = PAN_ID >> 8;
    memcpy(f + 5, dst, 8);
    memcpy(f + 13, gApMac, 8);
    f[21] = type;
    if (plen) memcpy(f + 22, payload, plen);
    gTxBuf[0] = NORMAL_HDR + 1 + plen + 2;
    return txRaw();
}

// ---- Tag and job tracking (call while holding the lock) ----

TagSlot* touchTag(const uint8_t* mac, bool updateLastSeen = true) {
    TagSlot* slot = nullptr;
    TagSlot* oldest = &gTags[0];
    for (TagSlot& s : gTags) {
        if (s.used && !memcmp(s.t.mac, mac, 8)) {
            slot = &s;
            break;
        }
        if (!s.used) {
            if (!slot) slot = &s;
        } else if (oldest->used && s.t.lastSeenMs < oldest->t.lastSeenMs) {
            oldest = &s;
        }
    }
    if (!slot) slot = oldest;
    if (!slot->used || memcmp(slot->t.mac, mac, 8)) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        memcpy(slot->t.mac, mac, 8);
    }
    if (updateLastSeen) {
        slot->t.lastSeenMs = millis();
        slot->t.seen = true;
    }
    return slot;
}

Job* findJob(const uint8_t* mac, uint8_t argument = 0) {
    for (Job& j : gJobs)
        if (j.used && j.dataTypeArgument == argument && !memcmp(j.mac, mac, 8)) return &j;
    return nullptr;
}

Job* findJobByVersion(const uint8_t* mac, const uint8_t* ver) {
    for (Job& j : gJobs)
        if (j.used && j.selected && !memcmp(j.mac, mac, 8) && !memcmp(j.ver, ver, 8)) return &j;
    return nullptr;
}

void freeJobData(Job& j) {
    free(j.data);
    j.data = nullptr;
}

// ---- Frame processing ----

void handleAvailReq(const uint8_t* src, const uint8_t* p, size_t plen, const RxItem& it, bool full) {
    AvailDataInfo info;
    memset(&info, 0, sizeof(info));
    info.dataType = DATATYPE_NOUPDATE;
    info.nextCheckIn = gCheckin;

    uint8_t hwType = 0;
    uint8_t wakeReason = 0;
    uint8_t channel = 0;
    int8_t temperature = 0;
    uint16_t batteryMv = 0;
    uint32_t checkins = 0;
    {
        Guard g;
        TagSlot* t = touchTag(src);
        info.nextCheckIn = t->t.checkinOverride ? t->t.checkinOverride : gCheckin;
        t->t.rssi = it.rssi;
        t->t.lqi = it.lqi;
        if (t->t.checkins == 0) t->t.screenRefreshPending = true;
        t->t.checkins++;
        if (plen >= 9) {
            t->t.hasMetadata = true;
            t->t.battMv = p[4] | (p[5] << 8);
            t->t.temp = (int8_t)p[3];
            t->t.hwType = p[6];
            t->t.wakeReason = p[7];
            if (t->t.wakeReason == WAKEUP_REASON_FIRSTBOOT) t->t.screenRefreshPending = true;
        }
        if (full && plen >= 22) {
            t->t.fw = p[9] | (p[10] << 8);
            t->t.channel = p[11];
        }
        hwType = t->t.hwType;
        wakeReason = t->t.wakeReason;
        channel = t->t.channel;
        temperature = t->t.temp;
        batteryMv = t->t.battMv;
        checkins = t->t.checkins;
    }

    bool jobOffered = false;
    uint32_t offeredSize = 0;
    uint8_t offeredType = DATATYPE_NOUPDATE;
    uint8_t offeredArgument = 0;
    {
        Guard g;
        for (Job& candidate : gJobs) if (candidate.used && !memcmp(candidate.mac, src, 8)) candidate.selected = false;
        const uint8_t wake = plen >= 8 ? p[7] : 0;
        const uint8_t preferred = wake == 4 ? 0x84 : (wake == 5 ? 0x8C : 0);
        Job* j = preferred ? findJob(src, preferred) : nullptr;
        if (!j) j = findJob(src, 0);
        if (!j) {
            for (Job& candidate : gJobs) if (candidate.used && !memcmp(candidate.mac, src, 8) && candidate.state == JOB_PENDING) { j = &candidate; break; }
        }
        if (j && j->state == JOB_PENDING && j->data) {
            j->selected = true;
            memcpy(info.dataVer, j->ver, 8);
            info.dataSize = j->size;
            info.dataType = j->type;
            info.dataTypeArgument = j->dataTypeArgument;
            jobOffered = true;
            offeredSize = j->size;
            offeredType = j->type;
            offeredArgument = j->dataTypeArgument;
        }
    }

    Serial.printf("[GW +%lu ms] CHECKIN kind=%s mac=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X "
                  "api_mac=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X hw=0x%02X wake=%u "
                  "channel=%u rssi=%d lqi=%u battery_mv=%u temp=%d total=%lu "
                  "job=%s job_size=%lu job_type=0x%02X target_arg=0x%02X\n",
                  (unsigned long)millis(), full ? "full" : "short",
                  src[7], src[6], src[5], src[4], src[3], src[2], src[1], src[0],
                  src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7],
                  hwType, wakeReason, channel, it.rssi, it.lqi, batteryMv, temperature,
                  (unsigned long)checkins, jobOffered ? "offered" : "none",
                  (unsigned long)offeredSize, offeredType, offeredArgument);

    info.checksum = sumBytes((uint8_t*)&info + 1, sizeof(info) - 1);
    if (!sendNormal(src, PKT_AVAIL_DATA_INFO, (uint8_t*)&info, sizeof(info))) {
        Serial.printf("[GW +%lu ms] CHECKIN_REPLY_FAILED\n", (unsigned long)millis());
    }
}

void sendBlock(const uint8_t* dst, const uint8_t* data, size_t size, const BlockRequest& req) {
    const size_t off = (size_t)req.blockId * BLOCK_DATA_SIZE;
    const size_t blen = min(BLOCK_DATA_SIZE, size - off);

    uint16_t sum = 0;
    for (size_t i = 0; i < blen; i++) sum += data[off + i];
    gBlock[0] = blen & 0xFF;
    gBlock[1] = blen >> 8;
    gBlock[2] = sum & 0xFF;
    gBlock[3] = sum >> 8;
    memcpy(gBlock + 4, data + off, blen);

    const size_t total = blen + 4;
    const size_t parts = (total + PART_DATA_SIZE - 1) / PART_DATA_SIZE;
    memset(gBlock + total, 0, parts * PART_DATA_SIZE - total);

    uint8_t part[3 + PART_DATA_SIZE];
    for (size_t p = 0; p < parts && p < MAX_PARTS; p++) {
        if (!(req.requestedParts[p / 8] & (1 << (p % 8)))) continue;
        part[1] = req.blockId;
        part[2] = (uint8_t)p;
        memcpy(part + 3, gBlock + p * PART_DATA_SIZE, PART_DATA_SIZE);
        part[0] = sumBytes(part + 1, sizeof(part) - 1);
        sendNormal(dst, PKT_BLOCK_PART, part, sizeof(part));
    }
}

void handleBlockRequest(const uint8_t* src, const uint8_t* p, size_t plen, const RxItem& it) {
    if (plen < sizeof(BlockRequest)) return;
    BlockRequest req;
    memcpy(&req, p, sizeof(req));
    if (req.checksum != sumBytes((uint8_t*)&req + 1, sizeof(req) - 1)) return;
    if (it.t <= gLastBurstEnd) return;

    const uint8_t* data = nullptr;
    size_t size = 0;
    bool valid = false;
    uint8_t jobType = 0;
    uint8_t jobArgument = 0;
    {
        Guard g;
        touchTag(src);
        Job* j = findJobByVersion(src, req.ver);
        if (j && j->state == JOB_PENDING && j->data && !memcmp(j->ver, req.ver, 8) &&
            (size_t)req.blockId * BLOCK_DATA_SIZE < j->size) {
            j->busy = true;
            j->inTransfer = true;
            data = j->data;
            size = j->size;
            jobType = j->type;
            jobArgument = j->dataTypeArgument;
            valid = true;
        }
    }

    if (!valid) {
        Serial.printf("[GW +%lu ms] JOB_BLOCK_REJECT mac=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X "
                      "block=%u reason=job_not_found\n",
                      (unsigned long)millis(), src[7], src[6], src[5], src[4],
                      src[3], src[2], src[1], src[0], req.blockId);
        sendNormal(src, PKT_CANCEL_XFER, nullptr, 0);
        return;
    }

    uint8_t requestedParts = 0;
    for (uint8_t i = 0; i < sizeof(req.requestedParts); i++) {
        uint8_t bits = req.requestedParts[i];
        while (bits) {
            requestedParts += bits & 1;
            bits >>= 1;
        }
    }
    Serial.printf("[GW +%lu ms] JOB_BLOCK_REQUEST mac=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X "
                  "api_mac=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X block=%u "
                  "fragments=%u size=%lu data_type=0x%02X target_arg=0x%02X\n",
                  (unsigned long)millis(),
                  src[7], src[6], src[5], src[4], src[3], src[2], src[1], src[0],
                  src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7],
                  req.blockId, requestedParts, (unsigned long)size, jobType, jobArgument);

    BlockRequestAck ack;
    ack.pleaseWaitMs = BLOCK_WAIT_MS;
    ack.checksum = sumBytes((uint8_t*)&ack.pleaseWaitMs, 2);
    sendNormal(src, PKT_BLOCK_REQUEST_ACK, (uint8_t*)&ack, sizeof(ack));
    vTaskDelay(pdMS_TO_TICKS(BLOCK_WAIT_MS + BLOCK_WAIT_MARGIN_MS));
    sendBlock(src, data, size, req);

    {
        Guard g;
        Job* j = findJobByVersion(src, req.ver);
        if (j) {
            j->busy = false;
            j->blocks++;
        }
    }
    gLastBurstEnd = esp_timer_get_time();
}

void handleXferComplete(const uint8_t* src) {
    bool completed = false;
    uint32_t completedSize = 0;
    uint8_t completedType = 0;
    uint8_t completedArgument = 0;
    uint16_t completedBlocks = 0;
    uint8_t completedVersion[8] = {};
    {
        Guard g;
        touchTag(src);
        Job* j = nullptr;
        for (Job& candidate : gJobs)
            if (candidate.used && (candidate.inTransfer || candidate.selected) && !memcmp(candidate.mac, src, 8)) {
                j = &candidate;
                break;
            }
        if (j && j->state == JOB_PENDING && !j->busy) {
            completedSize = j->size;
            completedType = j->type;
            completedArgument = j->dataTypeArgument;
            completedBlocks = j->blocks;
            memcpy(completedVersion, j->ver, sizeof(completedVersion));
            j->state = JOB_DONE;
            j->inTransfer = false;
            j->selected = false;
            freeJobData(*j);
            completed = true;
            if (completedType == DATATYPE_IMG_RAW_1BPP ||
                completedType == DATATYPE_IMG_RAW_2BPP ||
                completedType == DATATYPE_IMG_ZLIB) {
                TagSlot* tag = touchTag(src, false);
                if (tag) tag->t.screenRefreshPending = false;
            }
        }
    }
    if (completed) {
        Serial.printf("[GW +%lu ms] JOB_TRANSFER_COMPLETE mac=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X "
                      "api_mac=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X size=%lu "
                      "data_type=0x%02X target_arg=0x%02X blocks=%u md5_64=%02X%02X%02X%02X%02X%02X%02X%02X "
                      "display_redraw=tag_side_state_not_confirmed\n",
                      (unsigned long)millis(),
                      src[7], src[6], src[5], src[4], src[3], src[2], src[1], src[0],
                      src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7],
                      (unsigned long)completedSize, completedType, completedArgument, completedBlocks,
                      completedVersion[0], completedVersion[1], completedVersion[2], completedVersion[3],
                      completedVersion[4], completedVersion[5], completedVersion[6], completedVersion[7]);
    } else {
        Serial.printf("[GW +%lu ms] JOB_TRANSFER_COMPLETE_UNKNOWN mac=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
                      (unsigned long)millis(),
                      src[7], src[6], src[5], src[4], src[3], src[2], src[1], src[0]);
    }
    sendNormal(src, PKT_XFER_COMPLETE_ACK, nullptr, 0);
}

void handleFrame(const RxItem& it) {
    const uint8_t* f = it.buf;
    const size_t n = it.len;
    if (n < 3 || (f[0] & 7) != 1) return; 

    const uint8_t dstMode = (f[1] >> 2) & 3;
    const uint8_t srcMode = (f[1] >> 6) & 3;
    const bool panComp = (f[0] >> 6) & 1;

    if (dstMode == 2 && srcMode == 3 && !panComp) {
        if (n < BCAST_HDR + 1) return;
        const uint16_t dstPan = f[3] | (f[4] << 8);
        const uint16_t dstAddr = f[5] | (f[6] << 8);
        if (dstPan != PAN_ID || dstAddr != 0xFFFF) return;
        const uint8_t* src = f + 9;
        const uint8_t type = f[BCAST_HDR];
        const uint8_t* payload = f + BCAST_HDR + 1;
        const size_t plen = n - BCAST_HDR - 1;

        switch (type) {
            case PKT_PING: {
                const uint8_t ch = gChannel;
                {
                    Guard g;
                    TagSlot* t = touchTag(src);
                    t->t.rssi = it.rssi;
                    t->t.lqi = it.lqi;
                }
                if (!gPingLogged) {
                    Serial.println("Tag radio: received tag discovery PING");
                    gPingLogged = true;
                }
                if (!sendNormal(src, PKT_PONG, &ch, 1)) Serial.println("Tag radio: failed to send PONG");
                break;
            }
            case PKT_AVAIL_DATA_REQ:
                if (plen >= 9) {
                    const bool full = plen >= 22;
                    const size_t checkLen = full ? 22 : 9;
                    if (payload[0] == sumBytes(payload + 1, checkLen - 1)) handleAvailReq(src, payload, plen, it, full);
                }
                break;
            case PKT_AVAIL_DATA_SHORTREQ:
                handleAvailReq(src, payload, 0, it, false);
                break;
            default:
                break;
        }
    } else if (dstMode == 3 && srcMode == 3 && panComp) {
        if (n < NORMAL_HDR + 1) return;
        if (memcmp(f + 5, gApMac, 8) != 0) return;
        const uint8_t* src = f + 13;
        const uint8_t type = f[NORMAL_HDR];
        const uint8_t* payload = f + NORMAL_HDR + 1;
        const size_t plen = n - NORMAL_HDR - 1;

        switch (type) {
            case PKT_BLOCK_REQUEST:
            case PKT_BLOCK_PARTIAL_REQUEST:
                handleBlockRequest(src, payload, plen, it);
                break;
            case PKT_XFER_COMPLETE:
                handleXferComplete(src);
                break;
            default:
                break;
        }
    }
}

void apTask(void*) {
    RxItem it;
    for (;;) {
        if (xQueueReceive(gRxQueue, &it, portMAX_DELAY) == pdTRUE) handleFrame(it);
    }
}

}  // namespace

bool tagRadioBegin(uint8_t channel, uint16_t checkinMinutes) {
    gLock = xSemaphoreCreateMutex();
    gTxDone = xSemaphoreCreateBinary();
    gRxQueue = xQueueCreate(24, sizeof(RxItem));
    if (!gLock || !gTxDone || !gRxQueue) return false;

    gChannel = constrain(channel, 11, 26);
    gCheckin = max<uint16_t>(1, checkinMinutes);
    esp_read_mac(gApMac, ESP_MAC_IEEE802154);

    esp_ieee802154_event_cb_list_t cbs = {};
    cbs.rx_done_cb = onRxDone;
    cbs.rx_sfd_done_cb = onRxSfd;
    cbs.tx_done_cb = onTxDone;
    cbs.tx_failed_cb = onTxFailed;
    cbs.tx_sfd_done_cb = onTxSfd;
    cbs.ed_done_cb = onEd;
    cbs.enh_ack_generator_cb = onEnhAck;
    if (!radioCall("register callbacks", esp_ieee802154_event_callback_list_register(cbs))) return false;

    if (!radioCall("enable", esp_ieee802154_enable())) return false;
    if (!radioCall("set coordinator", esp_ieee802154_set_coordinator(false))) return false;
    if (!radioCall("set promiscuous mode", esp_ieee802154_set_promiscuous(false))) return false;
    if (!radioCall("enable receive-when-idle", esp_ieee802154_set_rx_when_idle(true))) return false;
    if (!radioCall("set channel", esp_ieee802154_set_channel(gChannel))) return false;
    if (!radioCall("set PAN ID", esp_ieee802154_set_panid(PAN_ID))) return false;
    if (!radioCall("set extended address", esp_ieee802154_set_extended_address(gApMac))) return false;
    if (!radioCall("set transmit power", esp_ieee802154_set_txpower(20))) return false;

    // Keep idle coexistence priority low and raise it only during radio activity.
    esp_ieee802154_coex_config_t coex = {IEEE802154_IDLE, IEEE802154_MIDDLE, IEEE802154_MIDDLE};
    esp_ieee802154_set_coex_config(coex);

    if (!radioCall("start receive", esp_ieee802154_receive())) return false;
    return xTaskCreate(apTask, "tag_radio", 6144, nullptr, 5, nullptr) == pdPASS;
}

void tagRadioGetMac(uint8_t out[8]) { memcpy(out, gApMac, 8); }
uint8_t tagRadioChannel() { return gChannel; }
void tagRadioSetCheckin(uint16_t minutes) { gCheckin = constrain(minutes, 1, 1440); }
void tagRadioSetTagCheckin(const uint8_t mac[8], uint16_t minutes) { Guard g; TagSlot* t = touchTag(mac, false); t->t.checkinOverride = constrain(minutes, 0, 1440); }
uint16_t tagRadioCheckin() { return gCheckin; }

size_t tagGetDevices(TagInfo* out, size_t maxCount) {
    Guard g;
    size_t n = 0;
    for (const TagSlot& s : gTags) {
        if (s.used && n < maxCount) out[n++] = s.t;
    }
    return n;
}

TagJobState tagGetJob(const uint8_t mac[8], TagJob* out, uint8_t dataTypeArgument) {
    Guard g;
    Job* j = findJob(mac, dataTypeArgument);
    if (!j) return JOB_NONE;
    if (out) {
        out->state = j->state;
        out->size = j->size;
        out->blocksSent = j->blocks;
        out->ageMs = millis() - j->created;
        out->dataTypeArgument = j->dataTypeArgument;
    }
    return j->state;
}

bool tagQueueImage(const uint8_t mac[8], uint8_t* data, size_t size, uint8_t dataType, const char** err, uint8_t dataTypeArgument) {
    if (!data || !size) {
        *err = "The update data is empty";
        return false;
    }
    uint8_t md5[16];
    if (mbedtls_md5(data, size, md5) != 0) {
        *err = "MD5 calculation failed";
        return false;
    }
    Serial.printf("Tag radio: image version (MD5-64) %02X%02X%02X%02X%02X%02X%02X%02X\n",
                  md5[0], md5[1], md5[2], md5[3], md5[4], md5[5], md5[6], md5[7]);

    Guard g;
    Job* slot = findJob(mac, dataTypeArgument);
    if (dataType == DATATYPE_FW_UPDATE) {
        for (const Job& job : gJobs) {
            if (job.used && job.state == JOB_PENDING && job.type == DATATYPE_FW_UPDATE &&
                memcmp(job.mac, mac, sizeof(job.mac))) {
                *err = "A firmware update is already pending for another tag";
                return false;
            }
        }
    }
    if (slot) {
        if (slot->busy || slot->inTransfer) {
            *err = "The tag is receiving data; retry shortly";
            return false;
        }
        if (slot->state == JOB_PENDING && slot->type == DATATYPE_FW_UPDATE && dataType != DATATYPE_FW_UPDATE) {
            *err = "A firmware update is pending for this tag";
            return false;
        }
    } else {
        for (Job& j : gJobs) {
            if (!j.used) {
                slot = &j;
                break;
            }
        }
        if (!slot) {
            for (Job& j : gJobs) {
                if (j.state == JOB_DONE && (!slot || j.created < slot->created)) slot = &j;
            }
        }
        if (!slot) {
            *err = "The update queue is full";
            return false;
        }
    }

    freeJobData(*slot);
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    memcpy(slot->mac, mac, 8);
    memcpy(slot->ver, md5, 8);
    slot->data = data;
    slot->size = size;
    slot->type = dataType;
    slot->dataTypeArgument = dataTypeArgument;
    slot->state = JOB_PENDING;
    slot->created = millis();
    return true;
}

bool tagCancelJob(const uint8_t mac[8], uint8_t dataTypeArgument) {
    Guard g;
    Job* j = findJob(mac, dataTypeArgument);
    if (!j || j->busy || j->inTransfer) return false;
    freeJobData(*j);
    memset(j, 0, sizeof(*j));
    return true;
}
