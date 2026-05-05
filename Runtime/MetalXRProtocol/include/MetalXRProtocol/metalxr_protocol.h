#ifndef METALXR_PROTOCOL_H
#define METALXR_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METALXR_PROTOCOL_MAGIC 0x4d585250u
#define METALXR_PROTOCOL_VERSION_MAJOR 0u
#define METALXR_PROTOCOL_VERSION_MINOR 1u
#define METALXR_PROTOCOL_DEVICE_NAME_SIZE 64u
#define METALXR_PROTOCOL_ERROR_MESSAGE_SIZE 128u
#define METALXR_PROTOCOL_LOG_MESSAGE_SIZE 192u

typedef enum MetalXRPacketType {
    METALXR_PACKET_HELLO = 1,
    METALXR_PACKET_HELLO_ACK = 2,
    METALXR_PACKET_HEARTBEAT = 3,
    METALXR_PACKET_ERROR = 4,
    METALXR_PACKET_VIDEO_FRAME = 10,
    METALXR_PACKET_POSE_SAMPLE = 20,
    METALXR_PACKET_CONTROLLER_INPUT = 21,
    METALXR_PACKET_HAPTIC_COMMAND = 22,
    METALXR_PACKET_TIMING_SAMPLE = 30,
    METALXR_PACKET_LOG = 40
} MetalXRPacketType;

typedef enum MetalXRRole {
    METALXR_ROLE_HOST = 1,
    METALXR_ROLE_QUEST_CLIENT = 2
} MetalXRRole;

typedef enum MetalXRCapabilityFlags {
    METALXR_CAPABILITY_H264 = 0x00000001u,
    METALXR_CAPABILITY_HEVC = 0x00000002u,
    METALXR_CAPABILITY_STEREO_SEPARATE_EYES = 0x00000004u,
    METALXR_CAPABILITY_STEREO_TEXTURE_ARRAY = 0x00000008u,
    METALXR_CAPABILITY_POSE_INPUT = 0x00000010u,
    METALXR_CAPABILITY_CONTROLLER_INPUT = 0x00000020u,
    METALXR_CAPABILITY_HAPTICS = 0x00000040u,
    METALXR_CAPABILITY_LOG_STREAM = 0x00000080u
} MetalXRCapabilityFlags;

typedef enum MetalXRCodec {
    METALXR_CODEC_H264 = 1,
    METALXR_CODEC_HEVC = 2
} MetalXRCodec;

typedef enum MetalXREye {
    METALXR_EYE_LEFT = 0,
    METALXR_EYE_RIGHT = 1
} MetalXREye;

typedef enum MetalXRTrackingFlags {
    METALXR_TRACKING_ORIENTATION_VALID = 0x00000001u,
    METALXR_TRACKING_POSITION_VALID = 0x00000002u,
    METALXR_TRACKING_ORIENTATION_TRACKED = 0x00000004u,
    METALXR_TRACKING_POSITION_TRACKED = 0x00000008u
} MetalXRTrackingFlags;

typedef enum MetalXRControllerHand {
    METALXR_CONTROLLER_HAND_LEFT = 0,
    METALXR_CONTROLLER_HAND_RIGHT = 1
} MetalXRControllerHand;

typedef enum MetalXRControllerButtonFlags {
    METALXR_CONTROLLER_BUTTON_PRIMARY = 0x00000001u,
    METALXR_CONTROLLER_BUTTON_SECONDARY = 0x00000002u,
    METALXR_CONTROLLER_BUTTON_MENU = 0x00000004u,
    METALXR_CONTROLLER_BUTTON_THUMBSTICK = 0x00000008u
} MetalXRControllerButtonFlags;

typedef enum MetalXRErrorCode {
    METALXR_ERROR_NONE = 0,
    METALXR_ERROR_VERSION_MISMATCH = 1,
    METALXR_ERROR_BAD_MAGIC = 2,
    METALXR_ERROR_BAD_PACKET = 3,
    METALXR_ERROR_UNSUPPORTED_CAPABILITY = 4
} MetalXRErrorCode;

#pragma pack(push, 1)

typedef struct MetalXRPacketHeader {
    uint32_t magic;
    uint16_t headerSize;
    uint16_t type;
    uint16_t versionMajor;
    uint16_t versionMinor;
    uint32_t flags;
    uint64_t sequence;
    uint64_t timestampNs;
    uint32_t payloadSize;
    uint32_t reserved;
} MetalXRPacketHeader;

typedef struct MetalXRHelloPayload {
    uint32_t role;
    uint32_t capabilities;
    uint32_t maxVideoWidth;
    uint32_t maxVideoHeight;
    uint32_t preferredFps;
    uint32_t controlPort;
    uint32_t mediaPort;
    uint32_t reserved;
    char deviceName[METALXR_PROTOCOL_DEVICE_NAME_SIZE];
} MetalXRHelloPayload;

typedef struct MetalXRHeartbeatPayload {
    uint64_t sessionId;
    uint64_t monotonicTimeNs;
    uint64_t lastFrameId;
    uint32_t batteryPercent;
    uint32_t flags;
} MetalXRHeartbeatPayload;

typedef struct MetalXRErrorPayload {
    uint32_t code;
    char message[METALXR_PROTOCOL_ERROR_MESSAGE_SIZE];
} MetalXRErrorPayload;

typedef struct MetalXRVideoFramePayload {
    uint64_t frameId;
    uint32_t eye;
    uint32_t codec;
    uint32_t width;
    uint32_t height;
    uint64_t timestampNs;
    uint64_t predictedDisplayTimeNs;
    uint64_t encoderLatencyUs;
    uint32_t payloadBytes;
    uint32_t flags;
} MetalXRVideoFramePayload;

typedef struct MetalXRPoseSamplePayload {
    uint64_t sampleId;
    uint64_t timestampNs;
    uint64_t predictedDisplayTimeNs;
    float position[3];
    float orientation[4];
    uint32_t trackingFlags;
    uint32_t reserved;
} MetalXRPoseSamplePayload;

typedef struct MetalXRControllerInputPayload {
    uint64_t sampleId;
    uint64_t timestampNs;
    uint32_t hand;
    uint32_t buttons;
    float trigger;
    float grip;
    float thumbstick[2];
    uint32_t trackingFlags;
    uint32_t reserved;
    float aimPosition[3];
    float aimOrientation[4];
    float gripPosition[3];
    float gripOrientation[4];
} MetalXRControllerInputPayload;

typedef struct MetalXRHapticCommandPayload {
    uint64_t commandId;
    uint64_t timestampNs;
    uint32_t hand;
    float amplitude;
    float frequencyHz;
    uint32_t durationUs;
} MetalXRHapticCommandPayload;

typedef struct MetalXRTimingSamplePayload {
    uint64_t frameId;
    uint64_t hostCaptureTimeNs;
    uint64_t predictedDisplayTimeNs;
    uint64_t encodeStartTimeNs;
    uint64_t encodeEndTimeNs;
    uint64_t clientReceiveTimeNs;
    uint64_t clientDisplayTimeNs;
} MetalXRTimingSamplePayload;

typedef struct MetalXRLogPayload {
    uint64_t timestampNs;
    uint32_t level;
    char message[METALXR_PROTOCOL_LOG_MESSAGE_SIZE];
} MetalXRLogPayload;

#pragma pack(pop)

const char* metalxr_packet_type_name(uint16_t type);
const char* metalxr_error_code_name(uint32_t code);
uint64_t metalxr_protocol_now_ns(void);

MetalXRPacketHeader metalxr_make_header(
    uint16_t type,
    uint64_t sequence,
    uint64_t timestampNs,
    uint32_t payloadSize);

int metalxr_protocol_validate_header(
    const MetalXRPacketHeader* header,
    uint32_t expectedPayloadSize,
    MetalXRErrorPayload* errorPayload);

int metalxr_protocol_send_packet(
    int fd,
    const MetalXRPacketHeader* header,
    const void* payload);

int metalxr_protocol_recv_packet(
    int fd,
    MetalXRPacketHeader* header,
    void* payload,
    size_t payloadCapacity);

#ifdef __cplusplus
}
#endif

#endif
