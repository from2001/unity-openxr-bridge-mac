#include "MetalXRProtocol/metalxr_protocol.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define METALXR_DEFAULT_STREAM_PORT 47000
#define METALXR_VIDEO_FRAME_FLAG_KEYFRAME 0x00000001u

typedef struct StreamerOptions {
    char bindHost[256];
    int port;
    int width;
    int height;
    int fps;
    int frames;
    int bitrate;
    int realtime;
} StreamerOptions;

typedef struct ByteBuffer {
    uint8_t* data;
    size_t size;
    size_t capacity;
} ByteBuffer;

typedef struct EncoderStats {
    uint64_t submittedFrames;
    uint64_t encodedFrames;
    uint64_t droppedFrames;
    uint64_t totalBytes;
    uint64_t totalLatencyUs;
    uint64_t maxLatencyUs;
} EncoderStats;

typedef struct StreamSession {
    int clientFd;
    uint64_t sequence;
    volatile int failed;
    pthread_mutex_t sendLock;
} StreamSession;

typedef struct EncoderContext {
    const char* eyeName;
    int eyeIndex;
    int width;
    int height;
    int fps;
    VTCompressionSessionRef session;
    StreamSession* stream;
    EncoderStats stats;
} EncoderContext;

typedef struct FrameRefcon {
    uint64_t frameId;
    int eyeIndex;
    int width;
    int height;
    uint64_t captureTimeNs;
    uint64_t predictedDisplayTimeNs;
    uint64_t encodeStartTimeNs;
} FrameRefcon;

static uint64_t host_time_ns(void)
{
    return metalxr_protocol_now_ns();
}

static void sleep_until_ns(uint64_t targetNs)
{
    for (;;) {
        const uint64_t now = host_time_ns();
        if (now >= targetNs) {
            return;
        }

        const uint64_t remaining = targetNs - now;
        struct timespec sleepTime;
        sleepTime.tv_sec = (time_t)(remaining / 1000000000ull);
        sleepTime.tv_nsec = (long)(remaining % 1000000000ull);
        if (nanosleep(&sleepTime, NULL) == 0 || errno != EINTR) {
            return;
        }
    }
}

static void print_usage(const char* argv0)
{
    fprintf(stderr,
            "Usage: %s [--bind-host HOST] [--port N] [--frames N] [--fps N] "
            "[--width N] [--height N] [--bitrate BPS] [--no-realtime]\n\n"
            "frames=0 streams until the client disconnects. The default bind host is 127.0.0.1,\n"
            "which is suitable for adb reverse. Use --bind-host 0.0.0.0 for Wi-Fi tests.\n",
            argv0);
}

static int parse_int_arg(const char* value, const char* name, int minValue, int maxValue)
{
    char* end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value || end == NULL || *end != '\0' || parsed < minValue || parsed > maxValue) {
        fprintf(stderr, "Invalid %s: %s\n", name, value);
        exit(2);
    }

    return (int)parsed;
}

static StreamerOptions parse_options(int argc, char** argv)
{
    StreamerOptions options;
    memset(&options, 0, sizeof(options));
    snprintf(options.bindHost, sizeof(options.bindHost), "%s", "127.0.0.1");
    options.port = METALXR_DEFAULT_STREAM_PORT;
    options.width = 640;
    options.height = 360;
    options.fps = 60;
    options.frames = 0;
    options.bitrate = 8000000;
    options.realtime = 1;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if ((strcmp(arg, "--bind-host") == 0 || strcmp(arg, "--bind") == 0) && i + 1 < argc) {
            snprintf(options.bindHost, sizeof(options.bindHost), "%s", argv[++i]);
        } else if (strcmp(arg, "--port") == 0 && i + 1 < argc) {
            options.port = parse_int_arg(argv[++i], "port", 1, 65535);
        } else if (strcmp(arg, "--frames") == 0 && i + 1 < argc) {
            options.frames = parse_int_arg(argv[++i], "frames", 0, 1000000);
        } else if (strcmp(arg, "--fps") == 0 && i + 1 < argc) {
            options.fps = parse_int_arg(argv[++i], "fps", 1, 240);
        } else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            options.width = parse_int_arg(argv[++i], "width", 16, 8192);
        } else if (strcmp(arg, "--height") == 0 && i + 1 < argc) {
            options.height = parse_int_arg(argv[++i], "height", 16, 8192);
        } else if (strcmp(arg, "--bitrate") == 0 && i + 1 < argc) {
            options.bitrate = parse_int_arg(argv[++i], "bitrate", 100000, 200000000);
        } else if (strcmp(arg, "--no-realtime") == 0) {
            options.realtime = 0;
        } else {
            print_usage(argv[0]);
            exit(2);
        }
    }

    return options;
}

static int append_bytes(ByteBuffer* buffer, const void* data, size_t size)
{
    if (size == 0) {
        return 1;
    }

    if (buffer == NULL || data == NULL) {
        return 0;
    }

    if (buffer->size + size > buffer->capacity) {
        size_t nextCapacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (nextCapacity < buffer->size + size) {
            nextCapacity *= 2;
        }

        uint8_t* nextData = (uint8_t*)realloc(buffer->data, nextCapacity);
        if (nextData == NULL) {
            return 0;
        }

        buffer->data = nextData;
        buffer->capacity = nextCapacity;
    }

    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static void free_buffer(ByteBuffer* buffer)
{
    if (buffer == NULL) {
        return;
    }

    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

static int append_annex_b_nal(ByteBuffer* buffer, const uint8_t* data, size_t length)
{
    static const uint8_t startCode[] = { 0x00, 0x00, 0x00, 0x01 };
    return append_bytes(buffer, startCode, sizeof(startCode)) &&
           append_bytes(buffer, data, length);
}

static int append_h264_parameter_sets(ByteBuffer* buffer, CMFormatDescriptionRef formatDescription)
{
    for (size_t index = 0; index < 2; ++index) {
        const uint8_t* parameterSet = NULL;
        size_t parameterSetSize = 0;
        size_t parameterSetCount = 0;
        int nalUnitHeaderLength = 0;
        const OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            formatDescription,
            index,
            &parameterSet,
            &parameterSetSize,
            &parameterSetCount,
            &nalUnitHeaderLength);
        if (status != noErr) {
            return 0;
        }

        if (!append_annex_b_nal(buffer, parameterSet, parameterSetSize)) {
            return 0;
        }
    }

    return 1;
}

static uint32_t read_big_endian_u32(const uint8_t* bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static int append_sample_buffer_annex_b(ByteBuffer* buffer, CMSampleBufferRef sampleBuffer)
{
    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (blockBuffer == NULL) {
        return 0;
    }

    char* dataPointer = NULL;
    size_t lengthAtOffset = 0;
    size_t totalLength = 0;
    OSStatus status = CMBlockBufferGetDataPointer(
        blockBuffer,
        0,
        &lengthAtOffset,
        &totalLength,
        &dataPointer);
    if (status != noErr || dataPointer == NULL || totalLength == 0) {
        return 0;
    }

    size_t offset = 0;
    while (offset + 4 <= totalLength) {
        const uint32_t nalLength = read_big_endian_u32((const uint8_t*)dataPointer + offset);
        offset += 4;
        if (nalLength == 0 || offset + nalLength > totalLength) {
            break;
        }

        if (!append_annex_b_nal(buffer, (const uint8_t*)dataPointer + offset, nalLength)) {
            return 0;
        }

        offset += nalLength;
    }

    return buffer->size > 0;
}

static CFNumberRef make_cf_number_sint32(int value)
{
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
}

static OSStatus set_int_property(VTCompressionSessionRef session, CFStringRef key, int value)
{
    CFNumberRef number = make_cf_number_sint32(value);
    if (number == NULL) {
        return kVTParameterErr;
    }

    const OSStatus status = VTSessionSetProperty(session, key, number);
    CFRelease(number);
    return status;
}

static bool is_keyframe(CMSampleBufferRef sampleBuffer)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments == NULL || CFArrayGetCount(attachments) == 0) {
        return true;
    }

    CFDictionaryRef attachment = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    return !CFDictionaryContainsKey(attachment, kCMSampleAttachmentKey_NotSync);
}

static int stream_video_frame(
    StreamSession* session,
    const FrameRefcon* frame,
    const ByteBuffer* encoded,
    uint64_t encoderLatencyUs,
    uint32_t flags)
{
    if (session == NULL || frame == NULL || encoded == NULL || encoded->data == NULL || encoded->size == 0) {
        return 0;
    }

    if (encoded->size > UINT32_MAX - sizeof(MetalXRVideoFramePayload)) {
        return 0;
    }

    MetalXRVideoFramePayload metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.frameId = frame->frameId;
    metadata.eye = (uint32_t)frame->eyeIndex;
    metadata.codec = METALXR_CODEC_H264;
    metadata.width = (uint32_t)frame->width;
    metadata.height = (uint32_t)frame->height;
    metadata.timestampNs = frame->captureTimeNs;
    metadata.predictedDisplayTimeNs = frame->predictedDisplayTimeNs;
    metadata.encoderLatencyUs = encoderLatencyUs;
    metadata.payloadBytes = (uint32_t)encoded->size;
    metadata.flags = flags;

    const uint32_t payloadSize = (uint32_t)(sizeof(metadata) + encoded->size);
    uint8_t* payload = (uint8_t*)malloc(payloadSize);
    if (payload == NULL) {
        return 0;
    }

    memcpy(payload, &metadata, sizeof(metadata));
    memcpy(payload + sizeof(metadata), encoded->data, encoded->size);

    pthread_mutex_lock(&session->sendLock);
    MetalXRPacketHeader header = metalxr_make_header(
        METALXR_PACKET_VIDEO_FRAME,
        session->sequence++,
        metalxr_protocol_now_ns(),
        payloadSize);
    const int sent = metalxr_protocol_send_packet(session->clientFd, &header, payload);
    if (!sent) {
        session->failed = 1;
    }
    pthread_mutex_unlock(&session->sendLock);

    free(payload);
    return sent;
}

static void compression_output_callback(
    void* outputCallbackRefCon,
    void* sourceFrameRefCon,
    OSStatus status,
    VTEncodeInfoFlags infoFlags,
    CMSampleBufferRef sampleBuffer)
{
    (void)infoFlags;

    EncoderContext* context = (EncoderContext*)outputCallbackRefCon;
    FrameRefcon* frame = (FrameRefcon*)sourceFrameRefCon;
    const uint64_t completeNs = host_time_ns();
    const uint64_t latencyUs =
        frame != NULL && completeNs >= frame->encodeStartTimeNs ?
            (completeNs - frame->encodeStartTimeNs) / 1000ull :
            0;

    if (context == NULL || context->stream == NULL || context->stream->failed ||
        status != noErr || sampleBuffer == NULL || !CMSampleBufferDataIsReady(sampleBuffer)) {
        if (context != NULL) {
            ++context->stats.droppedFrames;
        }
        if (frame != NULL) {
            fprintf(stderr,
                    "{\"event\":\"drop\",\"frame\":%" PRIu64 ",\"eye\":%d,"
                    "\"status\":%d,\"latency_us\":%" PRIu64 "}\n",
                    frame->frameId,
                    frame->eyeIndex,
                    (int)status,
                    latencyUs);
        }
        free(frame);
        return;
    }

    ByteBuffer encoded;
    memset(&encoded, 0, sizeof(encoded));
    uint32_t flags = 0;
    if (is_keyframe(sampleBuffer)) {
        flags |= METALXR_VIDEO_FRAME_FLAG_KEYFRAME;
        CMFormatDescriptionRef formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (formatDescription != NULL && !append_h264_parameter_sets(&encoded, formatDescription)) {
            ++context->stats.droppedFrames;
            free_buffer(&encoded);
            free(frame);
            return;
        }
    }

    if (!append_sample_buffer_annex_b(&encoded, sampleBuffer) || encoded.size == 0) {
        ++context->stats.droppedFrames;
        free_buffer(&encoded);
        free(frame);
        return;
    }

    if (!stream_video_frame(context->stream, frame, &encoded, latencyUs, flags)) {
        ++context->stats.droppedFrames;
        free_buffer(&encoded);
        free(frame);
        return;
    }

    ++context->stats.encodedFrames;
    context->stats.totalBytes += encoded.size;
    context->stats.totalLatencyUs += latencyUs;
    if (latencyUs > context->stats.maxLatencyUs) {
        context->stats.maxLatencyUs = latencyUs;
    }

    if (context->stats.encodedFrames <= 4 || (context->stats.encodedFrames % (uint64_t)(context->fps * 2)) == 0) {
        printf("{\"event\":\"streamed\",\"frame\":%" PRIu64 ",\"eye\":%d,"
               "\"eye_name\":\"%s\",\"width\":%d,\"height\":%d,"
               "\"latency_us\":%" PRIu64 ",\"bytes\":%zu,\"keyframe\":%s}\n",
               frame->frameId,
               frame->eyeIndex,
               context->eyeName,
               frame->width,
               frame->height,
               latencyUs,
               encoded.size,
               (flags & METALXR_VIDEO_FRAME_FLAG_KEYFRAME) != 0 ? "true" : "false");
        fflush(stdout);
    }

    free_buffer(&encoded);
    free(frame);
}

static void fill_synthetic_frame(CVPixelBufferRef pixelBuffer, uint64_t frameId, int eye)
{
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);

    uint8_t* base = (uint8_t*)CVPixelBufferGetBaseAddress(pixelBuffer);
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
    const int width = (int)CVPixelBufferGetWidth(pixelBuffer);
    const int height = (int)CVPixelBufferGetHeight(pixelBuffer);

    for (int y = 0; y < height; ++y) {
        uint8_t* row = base + ((size_t)y * bytesPerRow);
        for (int x = 0; x < width; ++x) {
            const uint8_t red = (uint8_t)((x + (int)frameId * 3 + eye * 60) & 0xff);
            const uint8_t green = (uint8_t)((y + (int)frameId * 2) & 0xff);
            const uint8_t blue = (uint8_t)(((x / 8) ^ (y / 8) ^ (int)frameId ^ (eye * 31)) & 0xff);
            row[(size_t)x * 4 + 0] = blue;
            row[(size_t)x * 4 + 1] = green;
            row[(size_t)x * 4 + 2] = red;
            row[(size_t)x * 4 + 3] = 255;
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
}

static CVPixelBufferRef create_synthetic_pixel_buffer(int width, int height, uint64_t frameId, int eye)
{
    CVPixelBufferRef pixelBuffer = NULL;
    const OSStatus status = CVPixelBufferCreate(
        kCFAllocatorDefault,
        (size_t)width,
        (size_t)height,
        kCVPixelFormatType_32BGRA,
        NULL,
        &pixelBuffer);
    if (status != kCVReturnSuccess || pixelBuffer == NULL) {
        return NULL;
    }

    fill_synthetic_frame(pixelBuffer, frameId, eye);
    return pixelBuffer;
}

static int create_encoder(EncoderContext* context, const StreamerOptions* options, StreamSession* stream)
{
    context->width = options->width;
    context->height = options->height;
    context->fps = options->fps;
    context->stream = stream;

    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        options->width,
        options->height,
        kCMVideoCodecType_H264,
        NULL,
        NULL,
        NULL,
        compression_output_callback,
        context,
        &context->session);
    if (status != noErr || context->session == NULL) {
        fprintf(stderr, "VTCompressionSessionCreate failed for %s: %d\n",
                context->eyeName,
                (int)status);
        return 0;
    }

    status = VTSessionSetProperty(context->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    if (status == noErr) {
        status = VTSessionSetProperty(
            context->session,
            kVTCompressionPropertyKey_AllowFrameReordering,
            kCFBooleanFalse);
    }
    if (status == noErr) {
        status = VTSessionSetProperty(
            context->session,
            kVTCompressionPropertyKey_ProfileLevel,
            kVTProfileLevel_H264_Baseline_AutoLevel);
    }
    if (status == noErr) {
        status = set_int_property(context->session, kVTCompressionPropertyKey_AverageBitRate, options->bitrate);
    }
    if (status == noErr) {
        status = set_int_property(context->session, kVTCompressionPropertyKey_ExpectedFrameRate, options->fps);
    }
    if (status == noErr) {
        status = set_int_property(context->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, options->fps);
    }
    if (status != noErr) {
        fprintf(stderr, "Failed to configure encoder for %s: %d\n", context->eyeName, (int)status);
        return 0;
    }

    status = VTCompressionSessionPrepareToEncodeFrames(context->session);
    if (status != noErr) {
        fprintf(stderr, "VTCompressionSessionPrepareToEncodeFrames failed for %s: %d\n",
                context->eyeName,
                (int)status);
        return 0;
    }

    return 1;
}

static int encode_eye_frame(
    EncoderContext* context,
    uint64_t frameId,
    uint64_t captureTimeNs,
    uint64_t predictedDisplayTimeNs)
{
    CVPixelBufferRef pixelBuffer =
        create_synthetic_pixel_buffer(context->width, context->height, frameId, context->eyeIndex);
    if (pixelBuffer == NULL) {
        fprintf(stderr, "Failed to create pixel buffer for %s frame %" PRIu64 "\n",
                context->eyeName,
                frameId);
        ++context->stats.droppedFrames;
        return 0;
    }

    FrameRefcon* frame = (FrameRefcon*)calloc(1, sizeof(*frame));
    if (frame == NULL) {
        CVPixelBufferRelease(pixelBuffer);
        return 0;
    }

    frame->frameId = frameId;
    frame->eyeIndex = context->eyeIndex;
    frame->width = context->width;
    frame->height = context->height;
    frame->captureTimeNs = captureTimeNs;
    frame->predictedDisplayTimeNs = predictedDisplayTimeNs;
    frame->encodeStartTimeNs = host_time_ns();

    const CMTime presentationTimeStamp = CMTimeMake((int64_t)frameId, context->fps);
    const CMTime duration = CMTimeMake(1, context->fps);
    const OSStatus status = VTCompressionSessionEncodeFrame(
        context->session,
        pixelBuffer,
        presentationTimeStamp,
        duration,
        NULL,
        frame,
        NULL);
    CVPixelBufferRelease(pixelBuffer);

    if (status != noErr) {
        fprintf(stderr, "VTCompressionSessionEncodeFrame failed for %s frame %" PRIu64 ": %d\n",
                context->eyeName,
                frameId,
                (int)status);
        ++context->stats.droppedFrames;
        free(frame);
        return 0;
    }

    ++context->stats.submittedFrames;
    return 1;
}

static void destroy_encoder(EncoderContext* context)
{
    if (context->session != NULL) {
        VTCompressionSessionCompleteFrames(context->session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(context->session);
        CFRelease(context->session);
        context->session = NULL;
    }
}

static void write_summary(const EncoderContext* context)
{
    const uint64_t averageLatency =
        context->stats.encodedFrames > 0 ?
            context->stats.totalLatencyUs / context->stats.encodedFrames :
            0;
    printf("{\"event\":\"summary\",\"eye\":%d,\"eye_name\":\"%s\","
           "\"submitted\":%" PRIu64 ",\"encoded\":%" PRIu64 ","
           "\"dropped\":%" PRIu64 ",\"bytes\":%" PRIu64 ","
           "\"avg_latency_us\":%" PRIu64 ",\"max_latency_us\":%" PRIu64 "}\n",
           context->eyeIndex,
           context->eyeName,
           context->stats.submittedFrames,
           context->stats.encodedFrames,
           context->stats.droppedFrames,
           context->stats.totalBytes,
           averageLatency,
           context->stats.maxLatencyUs);
}

static int create_server_socket(const StreamerOptions* options)
{
    char portBuffer[16];
    snprintf(portBuffer, sizeof(portBuffer), "%d", options->port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* results = NULL;
    const char* bindHost = strcmp(options->bindHost, "*") == 0 ? NULL : options->bindHost;
    const int gai = getaddrinfo(bindHost, portBuffer, &hints, &results);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo failed for %s:%s: %s\n",
                bindHost != NULL ? bindHost : "*",
                portBuffer,
                gai_strerror(gai));
        return -1;
    }

    int serverFd = -1;
    for (struct addrinfo* result = results; result != NULL; result = result->ai_next) {
        serverFd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (serverFd < 0) {
            continue;
        }

        int one = 1;
        (void)setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(serverFd, result->ai_addr, result->ai_addrlen) == 0 &&
            listen(serverFd, 1) == 0) {
            break;
        }

        close(serverFd);
        serverFd = -1;
    }

    freeaddrinfo(results);
    return serverFd;
}

static void describe_client(int clientFd, char* buffer, size_t bufferSize)
{
    struct sockaddr_storage address;
    socklen_t addressLength = sizeof(address);
    if (getpeername(clientFd, (struct sockaddr*)&address, &addressLength) != 0) {
        snprintf(buffer, bufferSize, "%s", "unknown");
        return;
    }

    void* rawAddress = NULL;
    int port = 0;
    if (address.ss_family == AF_INET) {
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)&address;
        rawAddress = &ipv4->sin_addr;
        port = ntohs(ipv4->sin_port);
    } else if (address.ss_family == AF_INET6) {
        struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)&address;
        rawAddress = &ipv6->sin6_addr;
        port = ntohs(ipv6->sin6_port);
    }

    char host[INET6_ADDRSTRLEN];
    if (rawAddress == NULL || inet_ntop(address.ss_family, rawAddress, host, sizeof(host)) == NULL) {
        snprintf(buffer, bufferSize, "%s", "unknown");
        return;
    }

    snprintf(buffer, bufferSize, "%s:%d", host, port);
}

static int send_error(int fd, uint64_t sequence, uint32_t code, const char* message)
{
    MetalXRErrorPayload errorPayload;
    memset(&errorPayload, 0, sizeof(errorPayload));
    errorPayload.code = code;
    snprintf(errorPayload.message, sizeof(errorPayload.message), "%s", message);

    MetalXRPacketHeader errorHeader = metalxr_make_header(
        METALXR_PACKET_ERROR,
        sequence,
        metalxr_protocol_now_ns(),
        sizeof(errorPayload));
    return metalxr_protocol_send_packet(fd, &errorHeader, &errorPayload);
}

static int receive_client_hello(int fd, MetalXRHelloPayload* hello, uint64_t* firstSequence)
{
    MetalXRPacketHeader header;
    if (!metalxr_protocol_recv_packet(fd, &header, hello, sizeof(*hello))) {
        fprintf(stderr, "Failed to receive Quest HELLO\n");
        return 0;
    }

    if (firstSequence != NULL) {
        *firstSequence = header.sequence;
    }

    MetalXRErrorPayload errorPayload;
    if (!metalxr_protocol_validate_header(&header, sizeof(*hello), &errorPayload) ||
        header.type != METALXR_PACKET_HELLO) {
        if (errorPayload.code == METALXR_ERROR_NONE) {
            errorPayload.code = METALXR_ERROR_BAD_PACKET;
            snprintf(errorPayload.message,
                     sizeof(errorPayload.message),
                     "Expected HELLO, got %s",
                     metalxr_packet_type_name(header.type));
        }
        (void)send_error(fd, header.sequence + 1, errorPayload.code, errorPayload.message);
        fprintf(stderr, "Rejected client hello: %s\n", errorPayload.message);
        return 0;
    }

    if ((hello->capabilities & METALXR_CAPABILITY_H264) == 0) {
        (void)send_error(fd, header.sequence + 1, METALXR_ERROR_UNSUPPORTED_CAPABILITY, "Quest client lacks H.264 capability");
        fprintf(stderr, "Rejected client hello: H.264 capability is missing\n");
        return 0;
    }

    printf("Quest HELLO role=%u caps=0x%08x max=%ux%u fps=%u name=%s\n",
           hello->role,
           hello->capabilities,
           hello->maxVideoWidth,
           hello->maxVideoHeight,
           hello->preferredFps,
           hello->deviceName);
    return 1;
}

static int send_hello_ack(int fd, StreamSession* session, const StreamerOptions* options)
{
    MetalXRHelloPayload ack;
    memset(&ack, 0, sizeof(ack));
    ack.role = METALXR_ROLE_HOST;
    ack.capabilities = METALXR_CAPABILITY_H264 |
                       METALXR_CAPABILITY_STEREO_SEPARATE_EYES |
                       METALXR_CAPABILITY_LOG_STREAM;
    ack.maxVideoWidth = (uint32_t)options->width;
    ack.maxVideoHeight = (uint32_t)options->height;
    ack.preferredFps = (uint32_t)options->fps;
    ack.controlPort = (uint32_t)options->port;
    ack.mediaPort = (uint32_t)options->port;
    snprintf(ack.deviceName, sizeof(ack.deviceName), "%s", "MetalXR Mac Host Streamer");

    MetalXRPacketHeader ackHeader = metalxr_make_header(
        METALXR_PACKET_HELLO_ACK,
        session->sequence++,
        metalxr_protocol_now_ns(),
        sizeof(ack));
    return metalxr_protocol_send_packet(fd, &ackHeader, &ack);
}

static int stream_client(int clientFd, const StreamerOptions* options)
{
    MetalXRHelloPayload clientHello;
    uint64_t firstSequence = 0;
    if (!receive_client_hello(clientFd, &clientHello, &firstSequence)) {
        return 0;
    }

    StreamSession stream;
    memset(&stream, 0, sizeof(stream));
    stream.clientFd = clientFd;
    stream.sequence = firstSequence + 1;
    pthread_mutex_init(&stream.sendLock, NULL);

    if (!send_hello_ack(clientFd, &stream, options)) {
        pthread_mutex_destroy(&stream.sendLock);
        return 0;
    }

    EncoderContext left = { "left", 0, 0, 0, 0, NULL, &stream, { 0 } };
    EncoderContext right = { "right", 1, 0, 0, 0, NULL, &stream, { 0 } };
    int ok = 1;

    if (!create_encoder(&left, options, &stream) ||
        !create_encoder(&right, options, &stream)) {
        ok = 0;
    }

    const uint64_t startNs = host_time_ns();
    const uint64_t framePeriodNs = 1000000000ull / (uint64_t)options->fps;
    for (int frame = 0; ok && !stream.failed && (options->frames == 0 || frame < options->frames); ++frame) {
        const uint64_t captureTimeNs = host_time_ns();
        const uint64_t predictedDisplayTimeNs = captureTimeNs + framePeriodNs;

        if (!encode_eye_frame(&left, (uint64_t)frame, captureTimeNs, predictedDisplayTimeNs) ||
            !encode_eye_frame(&right, (uint64_t)frame, captureTimeNs, predictedDisplayTimeNs)) {
            ok = 0;
        }

        if (options->realtime) {
            sleep_until_ns(startNs + ((uint64_t)(frame + 1) * framePeriodNs));
        }
    }

    if (left.session != NULL) {
        VTCompressionSessionCompleteFrames(left.session, kCMTimeInvalid);
    }
    if (right.session != NULL) {
        VTCompressionSessionCompleteFrames(right.session, kCMTimeInvalid);
    }

    write_summary(&left);
    write_summary(&right);
    fflush(stdout);

    ok = ok && !stream.failed && left.stats.droppedFrames == 0 && right.stats.droppedFrames == 0;

    destroy_encoder(&left);
    destroy_encoder(&right);
    pthread_mutex_destroy(&stream.sendLock);
    return ok;
}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);

    const StreamerOptions options = parse_options(argc, argv);
    const int serverFd = create_server_socket(&options);
    if (serverFd < 0) {
        fprintf(stderr, "Failed to listen on %s:%d\n", options.bindHost, options.port);
        return 1;
    }

    printf("MetalXR host streamer listening on %s:%d width=%d height=%d fps=%d bitrate=%d frames=%d\n",
           options.bindHost,
           options.port,
           options.width,
           options.height,
           options.fps,
           options.bitrate,
           options.frames);
    fflush(stdout);

    int exitCode = 0;
    for (;;) {
        struct sockaddr_storage address;
        socklen_t addressLength = sizeof(address);
        const int clientFd = accept(serverFd, (struct sockaddr*)&address, &addressLength);
        if (clientFd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            exitCode = 1;
            break;
        }

        int one = 1;
        (void)setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char clientDescription[128];
        describe_client(clientFd, clientDescription, sizeof(clientDescription));
        printf("Quest stream client connected from %s\n", clientDescription);
        fflush(stdout);

        const int ok = stream_client(clientFd, &options);
        close(clientFd);

        if (!ok) {
            fprintf(stderr, "Quest stream client disconnected before the stream completed\n");
            exitCode = 1;
        }

        if (options.frames > 0) {
            break;
        }

        printf("Waiting for Quest stream reconnect...\n");
        fflush(stdout);
    }

    close(serverFd);
    return exitCode;
}
