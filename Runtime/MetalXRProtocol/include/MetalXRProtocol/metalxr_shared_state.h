#ifndef METALXR_SHARED_STATE_H
#define METALXR_SHARED_STATE_H

#include "MetalXRProtocol/metalxr_protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METALXR_SHARED_STATE_DEFAULT_NAME "/metalxr_runtime_state"
#define METALXR_SHARED_STATE_NAME_SIZE 128u
#define METALXR_SHARED_STATE_MAGIC 0x4d585253u
#define METALXR_SHARED_STATE_VERSION 2u

typedef struct MetalXRSharedTrackingState {
    uint64_t hostTimestampNs;
    MetalXRPoseSamplePayload hmd;
    MetalXRControllerInputPayload controllers[2];
} MetalXRSharedTrackingState;

typedef struct MetalXRSharedTimingState {
    uint64_t hostTimestampNs;
    uint64_t timingSamples;
    int64_t clockOffsetNs;
    uint64_t clockRttNs;
    uint64_t measuredFramePeriodNs;
    uint64_t lastDisplayHostNs;
    int64_t predictionOffsetNs;
    uint64_t averageLatencyUs;
    uint64_t maxLatencyUs;
    uint64_t lastEncodeUs;
    uint64_t lastNetworkUs;
    uint64_t lastDecodeUs;
    uint64_t lastCompositorUs;
    uint64_t lastTotalUs;
    int64_t lastPredictionErrorUs;
    uint32_t lastQueueDepth;
    uint32_t reserved;
} MetalXRSharedTimingState;

typedef struct MetalXRSharedState {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t reserved;
    uint32_t trackingSequence;
    uint32_t timingSequence;
    uint32_t hapticSequence;
    uint32_t reservedSequence;
    uint64_t hostHeartbeatNs;
    MetalXRSharedTrackingState tracking;
    MetalXRSharedTimingState timing;
    MetalXRHapticCommandPayload haptic;
} MetalXRSharedState;

typedef struct MetalXRSharedStateMapping {
    int fd;
    size_t size;
    char name[METALXR_SHARED_STATE_NAME_SIZE];
    MetalXRSharedState* state;
} MetalXRSharedStateMapping;

const char* metalxr_shared_state_default_name(void);

int metalxr_shared_state_open(
    MetalXRSharedStateMapping* mapping,
    const char* name,
    int create,
    char* errorMessage,
    size_t errorMessageSize);

void metalxr_shared_state_close(MetalXRSharedStateMapping* mapping);

void metalxr_shared_state_write_host_heartbeat(
    MetalXRSharedStateMapping* mapping,
    uint64_t hostTimestampNs);

uint64_t metalxr_shared_state_host_heartbeat_ns(MetalXRSharedStateMapping* mapping);

void metalxr_shared_state_write_tracking(
    MetalXRSharedStateMapping* mapping,
    const MetalXRSharedTrackingState* tracking);

int metalxr_shared_state_read_tracking(
    MetalXRSharedStateMapping* mapping,
    MetalXRSharedTrackingState* tracking,
    uint32_t* sequence);

void metalxr_shared_state_write_timing(
    MetalXRSharedStateMapping* mapping,
    const MetalXRSharedTimingState* timing);

int metalxr_shared_state_read_timing(
    MetalXRSharedStateMapping* mapping,
    MetalXRSharedTimingState* timing,
    uint32_t* sequence);

void metalxr_shared_state_write_haptic(
    MetalXRSharedStateMapping* mapping,
    const MetalXRHapticCommandPayload* haptic);

int metalxr_shared_state_read_haptic(
    MetalXRSharedStateMapping* mapping,
    MetalXRHapticCommandPayload* haptic,
    uint32_t* sequence);

#ifdef __cplusplus
}
#endif

#endif
