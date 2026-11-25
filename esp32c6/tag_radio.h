#pragma once
#include <Arduino.h>

// Gateway communication with tags over IEEE 802.15.4.
//
// Packet formats match the upstream tag firmware and shared libraries:
//   tag  -> PING (bcast)            AP -> PONG (channel)
//   tag  -> AvailDataReq (bcast)    AP -> AvailDataInfo (dataVer = MD5[0..7], size, type, nextCheckIn)
//   tag  -> BlockRequest (unicast)  AP -> BlockRequestAck + BlockPart x N (99 B, max 42 per 4096 B block)
//   tag  -> XferComplete            AP -> XferCompleteAck
// The tag then renders the image itself.

constexpr size_t TAG_MAX_DEVICES = 16;

enum TagJobState : uint8_t { JOB_NONE = 0, JOB_PENDING = 1, JOB_DONE = 2 };

struct TagInfo {
    uint8_t mac[8];
    uint32_t lastSeenMs;
    bool seen;
    bool hasMetadata;
    uint32_t checkins;
    uint16_t battMv;
    uint16_t fw;
    int8_t rssi;
    int8_t temp;
    uint8_t lqi;
    uint8_t hwType;
    uint8_t channel;
    uint8_t wakeReason;
    bool screenRefreshPending;
    uint16_t checkinOverride; 
};

struct TagJob {
    TagJobState state;
    uint32_t size;
    uint16_t blocksSent;
    uint32_t ageMs;
    uint8_t dataTypeArgument;
};

// IEEE 802.15.4 channel (11..26). Tags scan for the AP on channels 11, 15, 20, 25, and 26 (C6 does not support 27).
bool tagRadioBegin(uint8_t channel, uint16_t checkinMinutes);
void tagRadioGetMac(uint8_t out[8]);
uint8_t tagRadioChannel();
void tagRadioSetCheckin(uint16_t minutes);
void tagRadioSetTagCheckin(const uint8_t mac[8], uint16_t minutes);
uint16_t tagRadioCheckin();

size_t tagGetDevices(TagInfo* out, size_t maxCount);
TagJobState tagGetJob(const uint8_t mac[8], TagJob* out, uint8_t dataTypeArgument = 0);

// Queues an image for a tag. On success, the AP takes ownership of the buffer and frees it.
bool tagQueueImage(const uint8_t mac[8], uint8_t* data, size_t size, uint8_t dataType,
                    const char** err, uint8_t dataTypeArgument = 0);
bool tagCancelJob(const uint8_t mac[8], uint8_t dataTypeArgument = 0);
