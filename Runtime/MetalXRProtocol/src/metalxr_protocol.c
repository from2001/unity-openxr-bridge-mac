#include "MetalXRProtocol/metalxr_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int write_all(int fd, const void* data, size_t size)
{
    const uint8_t* cursor = (const uint8_t*)data;
    size_t remaining = size;
    while (remaining > 0) {
        const ssize_t written = write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (written == 0) {
            return 0;
        }
        cursor += (size_t)written;
        remaining -= (size_t)written;
    }
    return 1;
}

static int read_all(int fd, void* data, size_t size)
{
    uint8_t* cursor = (uint8_t*)data;
    size_t remaining = size;
    while (remaining > 0) {
        const ssize_t bytesRead = read(fd, cursor, remaining);
        if (bytesRead < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (bytesRead == 0) {
            return 0;
        }
        cursor += (size_t)bytesRead;
        remaining -= (size_t)bytesRead;
    }
    return 1;
}

const char* metalxr_packet_type_name(uint16_t type)
{
    switch ((MetalXRPacketType)type) {
        case METALXR_PACKET_HELLO: return "HELLO";
        case METALXR_PACKET_HELLO_ACK: return "HELLO_ACK";
        case METALXR_PACKET_HEARTBEAT: return "HEARTBEAT";
        case METALXR_PACKET_ERROR: return "ERROR";
        case METALXR_PACKET_VIDEO_FRAME: return "VIDEO_FRAME";
        case METALXR_PACKET_POSE_SAMPLE: return "POSE_SAMPLE";
        case METALXR_PACKET_CONTROLLER_INPUT: return "CONTROLLER_INPUT";
        case METALXR_PACKET_HAPTIC_COMMAND: return "HAPTIC_COMMAND";
        case METALXR_PACKET_TIMING_SAMPLE: return "TIMING_SAMPLE";
        case METALXR_PACKET_LOG: return "LOG";
        default: return "UNKNOWN";
    }
}

const char* metalxr_error_code_name(uint32_t code)
{
    switch ((MetalXRErrorCode)code) {
        case METALXR_ERROR_NONE: return "NONE";
        case METALXR_ERROR_VERSION_MISMATCH: return "VERSION_MISMATCH";
        case METALXR_ERROR_BAD_MAGIC: return "BAD_MAGIC";
        case METALXR_ERROR_BAD_PACKET: return "BAD_PACKET";
        case METALXR_ERROR_UNSUPPORTED_CAPABILITY: return "UNSUPPORTED_CAPABILITY";
        default: return "UNKNOWN";
    }
}

uint64_t metalxr_protocol_now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return ((uint64_t)now.tv_sec * 1000000000ull) + (uint64_t)now.tv_nsec;
}

MetalXRPacketHeader metalxr_make_header(
    uint16_t type,
    uint64_t sequence,
    uint64_t timestampNs,
    uint32_t payloadSize)
{
    MetalXRPacketHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = METALXR_PROTOCOL_MAGIC;
    header.headerSize = (uint16_t)sizeof(MetalXRPacketHeader);
    header.type = type;
    header.versionMajor = METALXR_PROTOCOL_VERSION_MAJOR;
    header.versionMinor = METALXR_PROTOCOL_VERSION_MINOR;
    header.sequence = sequence;
    header.timestampNs = timestampNs;
    header.payloadSize = payloadSize;
    return header;
}

int metalxr_protocol_validate_header(
    const MetalXRPacketHeader* header,
    uint32_t expectedPayloadSize,
    MetalXRErrorPayload* errorPayload)
{
    if (errorPayload != NULL) {
        memset(errorPayload, 0, sizeof(*errorPayload));
    }

    if (header == NULL) {
        if (errorPayload != NULL) {
            errorPayload->code = METALXR_ERROR_BAD_PACKET;
            snprintf(errorPayload->message, sizeof(errorPayload->message), "%s", "Missing packet header");
        }
        return 0;
    }

    if (header->magic != METALXR_PROTOCOL_MAGIC) {
        if (errorPayload != NULL) {
            errorPayload->code = METALXR_ERROR_BAD_MAGIC;
            snprintf(errorPayload->message,
                     sizeof(errorPayload->message),
                     "Bad protocol magic: 0x%08x",
                     header->magic);
        }
        return 0;
    }

    if (header->headerSize != sizeof(MetalXRPacketHeader)) {
        if (errorPayload != NULL) {
            errorPayload->code = METALXR_ERROR_BAD_PACKET;
            snprintf(errorPayload->message,
                     sizeof(errorPayload->message),
                     "Bad header size: %u",
                     header->headerSize);
        }
        return 0;
    }

    if (header->versionMajor != METALXR_PROTOCOL_VERSION_MAJOR) {
        if (errorPayload != NULL) {
            errorPayload->code = METALXR_ERROR_VERSION_MISMATCH;
            snprintf(errorPayload->message,
                     sizeof(errorPayload->message),
                     "Protocol major version mismatch: remote=%u local=%u",
                     header->versionMajor,
                     METALXR_PROTOCOL_VERSION_MAJOR);
        }
        return 0;
    }

    if (expectedPayloadSize != UINT32_MAX && header->payloadSize != expectedPayloadSize) {
        if (errorPayload != NULL) {
            errorPayload->code = METALXR_ERROR_BAD_PACKET;
            snprintf(errorPayload->message,
                     sizeof(errorPayload->message),
                     "Bad payload size for %s: got=%u expected=%u",
                     metalxr_packet_type_name(header->type),
                     header->payloadSize,
                     expectedPayloadSize);
        }
        return 0;
    }

    return 1;
}

int metalxr_protocol_send_packet(
    int fd,
    const MetalXRPacketHeader* header,
    const void* payload)
{
    if (header == NULL) {
        return 0;
    }

    if (!write_all(fd, header, sizeof(*header))) {
        return 0;
    }

    if (header->payloadSize > 0 && payload != NULL) {
        return write_all(fd, payload, header->payloadSize);
    }

    return header->payloadSize == 0;
}

int metalxr_protocol_recv_packet(
    int fd,
    MetalXRPacketHeader* header,
    void* payload,
    size_t payloadCapacity)
{
    if (header == NULL) {
        return 0;
    }

    if (!read_all(fd, header, sizeof(*header))) {
        return 0;
    }

    if (header->payloadSize == 0) {
        return 1;
    }

    if (payload == NULL || payloadCapacity < header->payloadSize) {
        return 0;
    }

    return read_all(fd, payload, header->payloadSize);
}
