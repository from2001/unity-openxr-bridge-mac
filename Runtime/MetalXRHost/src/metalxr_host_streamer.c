#include "MetalXRProtocol/metalxr_protocol.h"
#include "MetalXRProtocol/metalxr_shared_state.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <Accelerate/Accelerate.h>
#include <IOSurface/IOSurface.h>
#include <VideoToolbox/VideoToolbox.h>

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define METALXR_DEFAULT_STREAM_PORT 47000
#define METALXR_MAX_UNITY_EXPORT_PAYLOAD_BYTES (512ull * 1024ull * 1024ull)
#define METALXR_MAX_PENDING_SHARED_SURFACES 64u

typedef enum FrameSourceMode {
    FRAME_SOURCE_SYNTHETIC = 0,
    FRAME_SOURCE_UNITY_EXPORT = 1,
} FrameSourceMode;

typedef struct StreamerOptions {
    char bindHost[256];
    int port;
    int width;
    int height;
    int fps;
    int widthExplicit;
    int heightExplicit;
    int fpsExplicit;
    int frames;
    int bitrate;
    int realtime;
    int queueDepth;
    int reconnectAttempts;
    FrameSourceMode frameSource;
    char frameExportDir[1024];
    char frameExportSocketPath[1024];
    int frameExportWaitMs;
    int clockSyncIntervalMs;
    int64_t predictionOffsetNs;
    char trackingStatePath[1024];
    char hapticCommandPath[1024];
    char timingStatePath[1024];
    char sharedStateName[METALXR_SHARED_STATE_NAME_SIZE];
    int useSharedState;
} StreamerOptions;

typedef struct UnityExportRecord {
    uint64_t sourceFrameId;
    int eye;
    uint64_t displayTimeNs;
    uint64_t referenceSpaceId;
    int width;
    int height;
    int imageRectX;
    int imageRectY;
    uint32_t imageRectWidth;
    uint32_t imageRectHeight;
    uint32_t imageArrayIndex;
    uint32_t projectionFlags;
    float posePosition[3];
    float poseOrientation[4];
    float fov[4];
    size_t bytesPerRow;
    size_t payloadBytes;
    uint32_t ioSurfaceId;
    char payloadFormat[32];
    char payloadPath[1024];
} UnityExportRecord;

typedef struct UnityExportFramePair {
    uint64_t sourceFrameId;
    UnityExportRecord eyes[2];
} UnityExportFramePair;

typedef struct UnityExportFrameSource {
    char exportDir[1024];
    char socketPath[1024];
    int socketFd;
    long indexOffset;
    int hasLatestEye[2];
    UnityExportRecord latestEye[2];
    int hasLatestPair;
    UnityExportFramePair latestPair;
    uint64_t repeatedFrames;
    uint64_t missingPolls;
} UnityExportFrameSource;

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

typedef struct HostTimingState {
    int hasClockSync;
    int64_t clockOffsetNs;
    uint64_t clockRttNs;
    uint64_t lastClockSyncSendNs;
    uint64_t measuredFramePeriodNs;
    uint64_t lastDisplayHostNs;
    uint64_t lastFrameId;
    uint64_t timingSamples;
    int64_t predictionOffsetNs;
    uint64_t totalLatencySamples;
    uint64_t totalLatencyUs;
    uint64_t maxLatencyUs;
    uint64_t lastEncodeUs;
    uint64_t lastNetworkUs;
    uint64_t lastDecodeUs;
    uint64_t lastCompositorUs;
    uint64_t lastTotalUs;
    int64_t lastPredictionErrorUs;
    uint32_t lastQueueDepth;
} HostTimingState;

typedef struct StreamSession {
    int clientFd;
    uint64_t sequence;
    volatile int failed;
    pthread_mutex_t sendLock;
    char trackingStatePath[1024];
    char hapticCommandPath[1024];
    char timingStatePath[1024];
    MetalXRSharedStateMapping sharedState;
    int hasSharedState;
    uint32_t lastSharedHapticSequence;
    uint64_t lastHapticCommandId;
    HostTimingState timing;
} StreamSession;

typedef struct HostPoseState {
    uint64_t sampleId;
    uint64_t timestampNs;
    uint32_t trackingFlags;
    float position[3];
    float orientation[4];
} HostPoseState;

typedef struct HostControllerState {
    uint64_t sampleId;
    uint64_t timestampNs;
    uint32_t hand;
    uint32_t buttons;
    uint32_t trackingFlags;
    float trigger;
    float grip;
    float thumbstick[2];
    float aimPosition[3];
    float aimOrientation[4];
    float gripPosition[3];
    float gripOrientation[4];
} HostControllerState;

typedef struct HostInputState {
    HostPoseState hmd;
    HostControllerState controllers[2];
    uint64_t poseSamples;
    uint64_t controllerSamples;
} HostInputState;

typedef struct EncoderContext {
    const char* eyeName;
    int eyeIndex;
    int width;
    int height;
    int fps;
    VTCompressionSessionRef session;
    CVPixelBufferPoolRef pixelBufferPool;
    StreamSession* stream;
    EncoderStats stats;
    pthread_mutex_t queueLock;
    int queueLockInitialized;
    uint32_t pendingFrames;
    uint32_t maxQueueDepth;
    uint32_t pendingSharedSurfaceIds[METALXR_MAX_PENDING_SHARED_SURFACES];
    uint32_t pendingSharedSurfaceCount;
} EncoderContext;

typedef struct FrameRefcon {
    uint64_t frameId;
    int eyeIndex;
    int width;
    int height;
    int imageRectX;
    int imageRectY;
    uint32_t imageRectWidth;
    uint32_t imageRectHeight;
    uint32_t imageArrayIndex;
    uint32_t projectionFlags;
    uint64_t referenceSpaceId;
    float posePosition[3];
    float poseOrientation[4];
    float fov[4];
    uint64_t captureTimeNs;
    uint64_t predictedDisplayTimeNs;
    uint64_t encodeStartTimeNs;
    uint32_t sharedIoSurfaceId;
} FrameRefcon;

static uint64_t host_time_ns(void)
{
    return metalxr_protocol_now_ns();
}

static int reserve_encoder_queue_slot(EncoderContext* context, uint64_t frameId);
static void release_encoder_queue_slot(EncoderContext* context);
static int shared_iosurface_is_pending(EncoderContext* context, uint32_t ioSurfaceId);
static int reserve_shared_iosurface(EncoderContext* context, uint32_t ioSurfaceId);
static void release_shared_iosurface(EncoderContext* context, uint32_t ioSurfaceId);

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
            "[--width N] [--height N] [--bitrate BPS] [--tracking-state-path PATH] "
            "[--haptic-command-path PATH] [--timing-state-path PATH] [--queue-depth N] "
            "[--frame-source synthetic|unity-export] [--frame-export-dir PATH] "
            "[--frame-export-socket PATH] [--frame-export-wait-ms N] "
            "[--shared-state-name NAME] [--no-shared-state] "
            "[--prediction-offset-ms N] [--clock-sync-interval-ms N] "
            "[--reconnect-attempts N] [--no-realtime]\n\n"
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
    options.queueDepth = 3;
    options.reconnectAttempts = 0;
    options.frameSource = FRAME_SOURCE_SYNTHETIC;
    options.frameExportDir[0] = '\0';
    options.frameExportSocketPath[0] = '\0';
    options.frameExportWaitMs = 3000;
    snprintf(options.sharedStateName, sizeof(options.sharedStateName), "%s", metalxr_shared_state_default_name());
    options.useSharedState = 1;
    options.clockSyncIntervalMs = 500;
    options.predictionOffsetNs = 0;
    snprintf(options.trackingStatePath, sizeof(options.trackingStatePath), "%s", "/tmp/metalxr_tracking_state.txt");
    snprintf(options.hapticCommandPath, sizeof(options.hapticCommandPath), "%s", "/tmp/metalxr_haptic_command.txt");
    snprintf(options.timingStatePath, sizeof(options.timingStatePath), "%s", "/tmp/metalxr_timing_state.txt");

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
            options.fpsExplicit = 1;
        } else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            options.width = parse_int_arg(argv[++i], "width", 16, 8192);
            options.widthExplicit = 1;
        } else if (strcmp(arg, "--height") == 0 && i + 1 < argc) {
            options.height = parse_int_arg(argv[++i], "height", 16, 8192);
            options.heightExplicit = 1;
        } else if (strcmp(arg, "--bitrate") == 0 && i + 1 < argc) {
            options.bitrate = parse_int_arg(argv[++i], "bitrate", 100000, 200000000);
        } else if (strcmp(arg, "--tracking-state-path") == 0 && i + 1 < argc) {
            snprintf(options.trackingStatePath, sizeof(options.trackingStatePath), "%s", argv[++i]);
        } else if (strcmp(arg, "--haptic-command-path") == 0 && i + 1 < argc) {
            snprintf(options.hapticCommandPath, sizeof(options.hapticCommandPath), "%s", argv[++i]);
        } else if (strcmp(arg, "--timing-state-path") == 0 && i + 1 < argc) {
            snprintf(options.timingStatePath, sizeof(options.timingStatePath), "%s", argv[++i]);
        } else if (strcmp(arg, "--queue-depth") == 0 && i + 1 < argc) {
            options.queueDepth = parse_int_arg(argv[++i], "queue depth", 1, 64);
        } else if (strcmp(arg, "--reconnect-attempts") == 0 && i + 1 < argc) {
            options.reconnectAttempts = parse_int_arg(argv[++i], "reconnect attempts", 0, 1000);
        } else if (strcmp(arg, "--frame-source") == 0 && i + 1 < argc) {
            const char* source = argv[++i];
            if (strcmp(source, "synthetic") == 0) {
                options.frameSource = FRAME_SOURCE_SYNTHETIC;
            } else if (strcmp(source, "unity-export") == 0) {
                options.frameSource = FRAME_SOURCE_UNITY_EXPORT;
            } else {
                fprintf(stderr, "Invalid frame source: %s\n", source);
                exit(2);
            }
        } else if (strcmp(arg, "--frame-export-dir") == 0 && i + 1 < argc) {
            snprintf(options.frameExportDir, sizeof(options.frameExportDir), "%s", argv[++i]);
        } else if (strcmp(arg, "--frame-export-socket") == 0 && i + 1 < argc) {
            snprintf(options.frameExportSocketPath, sizeof(options.frameExportSocketPath), "%s", argv[++i]);
        } else if (strcmp(arg, "--frame-export-wait-ms") == 0 && i + 1 < argc) {
            options.frameExportWaitMs = parse_int_arg(argv[++i], "frame export wait ms", 0, 600000);
        } else if (strcmp(arg, "--shared-state-name") == 0 && i + 1 < argc) {
            snprintf(options.sharedStateName, sizeof(options.sharedStateName), "%s", argv[++i]);
        } else if (strcmp(arg, "--no-shared-state") == 0) {
            options.useSharedState = 0;
        } else if (strcmp(arg, "--prediction-offset-ms") == 0 && i + 1 < argc) {
            options.predictionOffsetNs = (int64_t)parse_int_arg(argv[++i], "prediction offset ms", -500, 500) * 1000000;
        } else if (strcmp(arg, "--clock-sync-interval-ms") == 0 && i + 1 < argc) {
            options.clockSyncIntervalMs = parse_int_arg(argv[++i], "clock sync interval ms", 50, 60000);
        } else if (strcmp(arg, "--no-realtime") == 0) {
            options.realtime = 0;
        } else {
            print_usage(argv[0]);
            exit(2);
        }
    }

    if (options.frameSource == FRAME_SOURCE_UNITY_EXPORT &&
        options.frameExportDir[0] == '\0' &&
        options.frameExportSocketPath[0] == '\0') {
        fprintf(stderr, "--frame-export-dir or --frame-export-socket is required for --frame-source unity-export\n");
        exit(2);
    }

    return options;
}

static const char* frame_source_name(FrameSourceMode mode)
{
    switch (mode) {
        case FRAME_SOURCE_SYNTHETIC:
            return "synthetic";
        case FRAME_SOURCE_UNITY_EXPORT:
            return "unity-export";
        default:
            return "unknown";
    }
}

static uint64_t realtime_now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0;
    }

    return ((uint64_t)now.tv_sec * 1000000000ull) + (uint64_t)now.tv_nsec;
}

static uint64_t timespec_to_ns(const struct timespec* value)
{
    if (value == NULL || value->tv_sec < 0 || value->tv_nsec < 0) {
        return 0;
    }

    return ((uint64_t)value->tv_sec * 1000000000ull) + (uint64_t)value->tv_nsec;
}

static uint64_t file_modified_realtime_ns(const char* path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        return 0;
    }

#if defined(__APPLE__)
    return timespec_to_ns(&info.st_mtimespec);
#else
    return timespec_to_ns(&info.st_mtim);
#endif
}

static uint64_t file_age_ms(const char* path)
{
    const uint64_t modifiedNs = file_modified_realtime_ns(path);
    const uint64_t nowNs = realtime_now_ns();
    if (modifiedNs == 0 || nowNs == 0 || nowNs < modifiedNs) {
        return 0;
    }

    return (nowNs - modifiedNs) / 1000000ull;
}

static const char* json_find_value(const char* json, const char* key)
{
    const char* value = strstr(json, key);
    return value != NULL ? value + strlen(key) : NULL;
}

static int json_read_u64_field(const char* json, const char* key, uint64_t* output)
{
    const char* value = json_find_value(json, key);
    if (value == NULL || output == NULL) {
        return 0;
    }

    char* end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || errno != 0) {
        return 0;
    }

    *output = (uint64_t)parsed;
    return 1;
}

static int json_read_i64_field(const char* json, const char* key, int64_t* output)
{
    const char* value = json_find_value(json, key);
    if (value == NULL || output == NULL) {
        return 0;
    }

    char* end = NULL;
    errno = 0;
    const long long parsed = strtoll(value, &end, 10);
    if (end == value || errno != 0) {
        return 0;
    }

    *output = (int64_t)parsed;
    return 1;
}

static int json_read_float_field(const char* json, const char* key, float* output)
{
    const char* value = json_find_value(json, key);
    if (value == NULL || output == NULL) {
        return 0;
    }

    char* end = NULL;
    errno = 0;
    const double parsed = strtod(value, &end);
    if (end == value || errno != 0) {
        return 0;
    }

    *output = (float)parsed;
    return 1;
}

static int json_read_string_field(const char* json, const char* key, char* output, size_t outputSize)
{
    const char* value = json_find_value(json, key);
    if (value == NULL || output == NULL || outputSize == 0) {
        return 0;
    }

    const char* end = value;
    while (*end != '\0' && *end != '"') {
        if (*end == '\\') {
            return 0;
        }
        ++end;
    }
    if (*end != '"') {
        return 0;
    }

    const size_t length = (size_t)(end - value);
    if (length >= outputSize) {
        return 0;
    }

    memcpy(output, value, length);
    output[length] = '\0';
    return 1;
}

static int parse_unity_export_record(const char* line, UnityExportRecord* record)
{
    if (line == NULL || record == NULL) {
        return 0;
    }

    memset(record, 0, sizeof(*record));
    uint64_t eye = 0;
    uint64_t width = 0;
    uint64_t height = 0;
    uint64_t bytesPerRow = 0;
    uint64_t payloadBytes = 0;
    uint64_t ioSurfaceId = 0;
    uint64_t imageArrayIndex = 0;
    uint64_t imageRectWidth = 0;
    uint64_t imageRectHeight = 0;
    uint64_t referenceSpaceId = 0;
    uint64_t projectionFlags = 0;
    int64_t imageRectX = 0;
    int64_t imageRectY = 0;
    int64_t displayTime = 0;

    if (!json_read_u64_field(line, "\"frame\":", &record->sourceFrameId) ||
        !json_read_u64_field(line, "\"eye\":", &eye) ||
        !json_read_i64_field(line, "\"displayTime\":", &displayTime) ||
        !json_read_string_field(line, "\"payloadFormat\":\"", record->payloadFormat, sizeof(record->payloadFormat)) ||
        !json_read_u64_field(line, "\"width\":", &width) ||
        !json_read_u64_field(line, "\"height\":", &height) ||
        !json_read_u64_field(line, "\"bytesPerRow\":", &bytesPerRow) ||
        !json_read_u64_field(line, "\"payloadBytes\":", &payloadBytes)) {
        return 0;
    }

    const int isRawPayload =
        strcmp(record->payloadFormat, "BGRA8") == 0 ||
        strcmp(record->payloadFormat, "RGBA8") == 0;
    const int isIosurfacePayload =
        strcmp(record->payloadFormat, "IOSurfaceBGRA8") == 0 ||
        strcmp(record->payloadFormat, "IOSurfaceRGBA8") == 0;

    (void)json_read_string_field(
        line,
        "\"payloadPath\":\"",
        record->payloadPath,
        sizeof(record->payloadPath));
    (void)json_read_u64_field(line, "\"ioSurfaceId\":", &ioSurfaceId);

    if (eye > 1 || width > INT_MAX || height > INT_MAX ||
        bytesPerRow > SIZE_MAX || payloadBytes > SIZE_MAX ||
        (!isRawPayload && !isIosurfacePayload)) {
        return 0;
    }

    if ((isRawPayload && record->payloadPath[0] == '\0') ||
        (isIosurfacePayload && ioSurfaceId > UINT32_MAX) ||
        (isIosurfacePayload && ioSurfaceId == 0)) {
        return 0;
    }

    record->eye = (int)eye;
    record->displayTimeNs = displayTime > 0 ? (uint64_t)displayTime : 0;
    record->width = (int)width;
    record->height = (int)height;
    record->imageRectX = 0;
    record->imageRectY = 0;
    record->imageRectWidth = (uint32_t)record->width;
    record->imageRectHeight = (uint32_t)record->height;
    record->imageArrayIndex = 0;
    record->referenceSpaceId = 0;
    record->projectionFlags = 0;
    record->poseOrientation[3] = 1.0f;
    record->fov[0] = -0.7853982f;
    record->fov[1] = 0.7853982f;
    record->fov[2] = 0.7853982f;
    record->fov[3] = -0.7853982f;

    if (json_read_i64_field(line, "\"imageRectX\":", &imageRectX) &&
        json_read_i64_field(line, "\"imageRectY\":", &imageRectY) &&
        json_read_u64_field(line, "\"imageRectWidth\":", &imageRectWidth) &&
        json_read_u64_field(line, "\"imageRectHeight\":", &imageRectHeight) &&
        json_read_u64_field(line, "\"imageArrayIndex\":", &imageArrayIndex) &&
        imageRectX >= INT_MIN &&
        imageRectX <= INT_MAX &&
        imageRectY >= INT_MIN &&
        imageRectY <= INT_MAX &&
        imageRectWidth <= UINT32_MAX &&
        imageRectHeight <= UINT32_MAX &&
        imageArrayIndex <= UINT32_MAX) {
        record->imageRectX = (int)imageRectX;
        record->imageRectY = (int)imageRectY;
        record->imageRectWidth = (uint32_t)imageRectWidth;
        record->imageRectHeight = (uint32_t)imageRectHeight;
        record->imageArrayIndex = (uint32_t)imageArrayIndex;
    }

    if (json_read_u64_field(line, "\"referenceSpaceId\":", &referenceSpaceId)) {
        record->referenceSpaceId = referenceSpaceId;
    }
    if (json_read_u64_field(line, "\"projectionFlags\":", &projectionFlags) &&
        projectionFlags <= UINT32_MAX) {
        record->projectionFlags = (uint32_t)projectionFlags;
    }

    if (json_read_float_field(line, "\"posePositionX\":", &record->posePosition[0]) &&
        json_read_float_field(line, "\"posePositionY\":", &record->posePosition[1]) &&
        json_read_float_field(line, "\"posePositionZ\":", &record->posePosition[2]) &&
        json_read_float_field(line, "\"poseOrientationX\":", &record->poseOrientation[0]) &&
        json_read_float_field(line, "\"poseOrientationY\":", &record->poseOrientation[1]) &&
        json_read_float_field(line, "\"poseOrientationZ\":", &record->poseOrientation[2]) &&
        json_read_float_field(line, "\"poseOrientationW\":", &record->poseOrientation[3]) &&
        json_read_float_field(line, "\"fovAngleLeft\":", &record->fov[0]) &&
        json_read_float_field(line, "\"fovAngleRight\":", &record->fov[1]) &&
        json_read_float_field(line, "\"fovAngleUp\":", &record->fov[2]) &&
        json_read_float_field(line, "\"fovAngleDown\":", &record->fov[3])) {
        if (record->projectionFlags == 0 && projectionFlags != 0) {
            record->projectionFlags = 1;
        }
    }

    record->bytesPerRow = (size_t)bytesPerRow;
    record->payloadBytes = (size_t)payloadBytes;
    record->ioSurfaceId = (uint32_t)ioSurfaceId;
    return 1;
}

static void update_unity_export_source(UnityExportFrameSource* source, const UnityExportRecord* record);

static int unity_export_source_open_socket(UnityExportFrameSource* source)
{
    if (source == NULL || source->socketPath[0] == '\0') {
        return 1;
    }
    if (strlen(source->socketPath) >= sizeof(((struct sockaddr_un*)0)->sun_path)) {
        fprintf(stderr, "Unity export socket path is too long: %s\n", source->socketPath);
        return 0;
    }

    const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(AF_UNIX, SOCK_DGRAM)");
        return 0;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", source->socketPath);

    (void)unlink(source->socketPath);
    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        perror("bind(frame export socket)");
        close(fd);
        return 0;
    }

    source->socketFd = fd;
    return 1;
}

static void unity_export_source_close(UnityExportFrameSource* source)
{
    if (source == NULL) {
        return;
    }
    if (source->socketPath[0] != '\0' && source->socketFd >= 0) {
        close(source->socketFd);
        source->socketFd = -1;
    }
    if (source->socketPath[0] != '\0') {
        (void)unlink(source->socketPath);
    }
}

static void unity_export_source_poll_socket(UnityExportFrameSource* source, int* parsedRecords)
{
    if (source == NULL || source->socketFd < 0) {
        return;
    }

    for (;;) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(source->socketFd, &readSet);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        const int ready = select(source->socketFd + 1, &readSet, NULL, NULL, &timeout);
        if (ready <= 0 || !FD_ISSET(source->socketFd, &readSet)) {
            return;
        }

        char line[8192];
        const ssize_t received = recvfrom(source->socketFd, line, sizeof(line) - 1, 0, NULL, NULL);
        if (received <= 0) {
            return;
        }
        line[received] = '\0';
        UnityExportRecord record;
        if (parse_unity_export_record(line, &record)) {
            update_unity_export_source(source, &record);
            if (parsedRecords != NULL) {
                ++(*parsedRecords);
            }
        }
    }
}

static void unity_export_source_init(
    UnityExportFrameSource* source,
    const char* exportDir,
    const char* socketPath)
{
    memset(source, 0, sizeof(*source));
    source->socketFd = -1;
    if (exportDir != NULL) {
        snprintf(source->exportDir, sizeof(source->exportDir), "%s", exportDir);
    }
    if (socketPath != NULL) {
        snprintf(source->socketPath, sizeof(source->socketPath), "%s", socketPath);
    }
}

static void update_unity_export_source(UnityExportFrameSource* source, const UnityExportRecord* record)
{
    if (source == NULL || record == NULL || record->eye < 0 || record->eye > 1) {
        return;
    }

    source->latestEye[record->eye] = *record;
    source->hasLatestEye[record->eye] = 1;
    if (source->hasLatestEye[0] && source->hasLatestEye[1] &&
        source->latestEye[0].sourceFrameId == source->latestEye[1].sourceFrameId) {
        source->latestPair.sourceFrameId = source->latestEye[0].sourceFrameId;
        source->latestPair.eyes[0] = source->latestEye[0];
        source->latestPair.eyes[1] = source->latestEye[1];
        source->hasLatestPair = 1;
    }
}

static int poll_unity_export_frame_source(UnityExportFrameSource* source, UnityExportFramePair* pair)
{
    if (source == NULL) {
        return 0;
    }

    int parsedRecords = 0;
    unity_export_source_poll_socket(source, &parsedRecords);

    if (source->exportDir[0] != '\0') {
        char indexPath[1024];
        snprintf(indexPath, sizeof(indexPath), "%s/frames.jsonl", source->exportDir);

        FILE* input = fopen(indexPath, "r");
        if (input != NULL) {
            if (source->indexOffset > 0) {
                if (fseek(input, 0, SEEK_END) == 0) {
                    const long indexSize = ftell(input);
                    if (indexSize >= 0 && indexSize < source->indexOffset) {
                        source->indexOffset = 0;
                    }
                }
            }

            if (source->indexOffset > 0) {
                (void)fseek(input, source->indexOffset, SEEK_SET);
            }

            char line[8192];
            while (fgets(line, sizeof(line), input) != NULL) {
                UnityExportRecord record;
                if (parse_unity_export_record(line, &record)) {
                    update_unity_export_source(source, &record);
                    ++parsedRecords;
                }
            }

            const long nextOffset = ftell(input);
            if (nextOffset >= 0) {
                source->indexOffset = nextOffset;
            }
            fclose(input);
        }
    }

    if (!source->hasLatestPair) {
        ++source->missingPolls;
        return 0;
    }

    if (parsedRecords == 0) {
        ++source->repeatedFrames;
    }
    if (pair != NULL) {
        *pair = source->latestPair;
    }
    return 1;
}

static uint64_t unity_export_pair_age_ms(const UnityExportFramePair* pair)
{
    if (pair == NULL) {
        return 0;
    }

    if (pair->eyes[0].payloadPath[0] == '\0' || pair->eyes[1].payloadPath[0] == '\0') {
        return 0;
    }

    const uint64_t leftAgeMs = file_age_ms(pair->eyes[0].payloadPath);
    const uint64_t rightAgeMs = file_age_ms(pair->eyes[1].payloadPath);
    return leftAgeMs > rightAgeMs ? leftAgeMs : rightAgeMs;
}

static int is_plausible_host_display_time(uint64_t displayTimeNs, uint64_t captureTimeNs)
{
    if (displayTimeNs == 0 || captureTimeNs == 0) {
        return 0;
    }

    const uint64_t deltaNs =
        displayTimeNs > captureTimeNs ?
            displayTimeNs - captureTimeNs :
            captureTimeNs - displayTimeNs;
    return deltaNs < 2000000000ull;
}

static int validate_unity_export_pair_dimensions(
    const UnityExportFramePair* pair,
    int expectedWidth,
    int expectedHeight)
{
    if (pair == NULL) {
        return 0;
    }

    for (size_t eye = 0; eye < 2; ++eye) {
        const UnityExportRecord* record = &pair->eyes[eye];
        if (record->eye != (int)eye ||
            record->width != expectedWidth ||
            record->height != expectedHeight) {
            return 0;
        }
    }

    return 1;
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
    metadata.flags =
        flags |
        (frame->projectionFlags != 0 ? METALXR_VIDEO_FRAME_FLAG_PROJECTION_METADATA : 0u);
    metadata.imageRectX = frame->imageRectX;
    metadata.imageRectY = frame->imageRectY;
    metadata.imageRectWidth = frame->imageRectWidth;
    metadata.imageRectHeight = frame->imageRectHeight;
    metadata.imageArrayIndex = frame->imageArrayIndex;
    metadata.projectionFlags = frame->projectionFlags;
    metadata.referenceSpaceId = frame->referenceSpaceId;
    memcpy(metadata.posePosition, frame->posePosition, sizeof(metadata.posePosition));
    memcpy(metadata.poseOrientation, frame->poseOrientation, sizeof(metadata.poseOrientation));
    memcpy(metadata.fov, frame->fov, sizeof(metadata.fov));

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

    if (context == NULL || frame == NULL || context->stream == NULL || context->stream->failed ||
        status != noErr || sampleBuffer == NULL || !CMSampleBufferDataIsReady(sampleBuffer)) {
        if (context != NULL) {
            ++context->stats.droppedFrames;
            if (frame != NULL) {
                release_shared_iosurface(context, frame->sharedIoSurfaceId);
            }
            release_encoder_queue_slot(context);
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
            release_shared_iosurface(context, frame->sharedIoSurfaceId);
            release_encoder_queue_slot(context);
            free_buffer(&encoded);
            free(frame);
            return;
        }
    }

    if (!append_sample_buffer_annex_b(&encoded, sampleBuffer) || encoded.size == 0) {
        ++context->stats.droppedFrames;
        release_shared_iosurface(context, frame->sharedIoSurfaceId);
        release_encoder_queue_slot(context);
        free_buffer(&encoded);
        free(frame);
        return;
    }

    if (!stream_video_frame(context->stream, frame, &encoded, latencyUs, flags)) {
        ++context->stats.droppedFrames;
        release_shared_iosurface(context, frame->sharedIoSurfaceId);
        release_encoder_queue_slot(context);
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

    release_shared_iosurface(context, frame->sharedIoSurfaceId);
    release_encoder_queue_slot(context);
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

static void set_cf_int(CFMutableDictionaryRef dictionary, CFStringRef key, int value)
{
    if (dictionary == NULL || key == NULL) {
        return;
    }

    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    if (number == NULL) {
        return;
    }
    CFDictionarySetValue(dictionary, key, number);
    CFRelease(number);
}

static CVPixelBufferPoolRef create_encoder_pixel_buffer_pool(
    int width,
    int height,
    int minBufferCount)
{
    CFMutableDictionaryRef poolAttributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef pixelAttributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef iosurfaceProperties = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    if (poolAttributes == NULL || pixelAttributes == NULL || iosurfaceProperties == NULL) {
        if (poolAttributes != NULL) {
            CFRelease(poolAttributes);
        }
        if (pixelAttributes != NULL) {
            CFRelease(pixelAttributes);
        }
        if (iosurfaceProperties != NULL) {
            CFRelease(iosurfaceProperties);
        }
        return NULL;
    }

    set_cf_int(poolAttributes, kCVPixelBufferPoolMinimumBufferCountKey, minBufferCount);
    set_cf_int(pixelAttributes, kCVPixelBufferWidthKey, width);
    set_cf_int(pixelAttributes, kCVPixelBufferHeightKey, height);
    set_cf_int(pixelAttributes, kCVPixelBufferPixelFormatTypeKey, kCVPixelFormatType_32BGRA);
    CFDictionarySetValue(pixelAttributes, kCVPixelBufferIOSurfacePropertiesKey, iosurfaceProperties);
    CFDictionarySetValue(pixelAttributes, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);

    CVPixelBufferPoolRef pool = NULL;
    const CVReturn status = CVPixelBufferPoolCreate(
        kCFAllocatorDefault,
        poolAttributes,
        pixelAttributes,
        &pool);

    CFRelease(iosurfaceProperties);
    CFRelease(pixelAttributes);
    CFRelease(poolAttributes);

    if (status != kCVReturnSuccess) {
        return NULL;
    }
    return pool;
}

static CVPixelBufferRef create_encoder_pool_pixel_buffer(EncoderContext* context)
{
    if (context == NULL || context->pixelBufferPool == NULL) {
        return NULL;
    }

    CVPixelBufferRef pixelBuffer = NULL;
    const CVReturn status = CVPixelBufferPoolCreatePixelBuffer(
        kCFAllocatorDefault,
        context->pixelBufferPool,
        &pixelBuffer);
    if (status != kCVReturnSuccess) {
        return NULL;
    }

    return pixelBuffer;
}

static CVPixelBufferRef create_synthetic_pixel_buffer(EncoderContext* context, uint64_t frameId)
{
    CVPixelBufferRef pixelBuffer = create_encoder_pool_pixel_buffer(context);
    if (pixelBuffer == NULL) {
        return NULL;
    }

    fill_synthetic_frame(pixelBuffer, frameId, context->eyeIndex);
    return pixelBuffer;
}

static int swizzle_rgba_to_bgra_rows(
    const uint8_t* sourceBase,
    size_t sourceBytesPerRow,
    uint8_t* destinationBase,
    size_t destinationBytesPerRow,
    int width,
    int height)
{
    if (sourceBase == NULL || destinationBase == NULL ||
        width <= 0 || height <= 0 ||
        sourceBytesPerRow < (size_t)width * 4u ||
        destinationBytesPerRow < (size_t)width * 4u) {
        return 0;
    }

    vImage_Buffer source;
    source.data = (void*)sourceBase;
    source.height = (vImagePixelCount)height;
    source.width = (vImagePixelCount)width;
    source.rowBytes = sourceBytesPerRow;

    vImage_Buffer destination;
    destination.data = destinationBase;
    destination.height = (vImagePixelCount)height;
    destination.width = (vImagePixelCount)width;
    destination.rowBytes = destinationBytesPerRow;

    const uint8_t permuteMap[4] = { 2, 1, 0, 3 };
    return vImagePermuteChannels_ARGB8888(
               &source,
               &destination,
               permuteMap,
               kvImageNoFlags) == kvImageNoError;
}

static int read_file_exact(const char* path, uint8_t* data, size_t size)
{
    FILE* input = fopen(path, "rb");
    if (input == NULL) {
        return 0;
    }

    const size_t readBytes = fread(data, 1, size, input);
    const int ok = readBytes == size && ferror(input) == 0;
    fclose(input);
    return ok;
}

static int unity_export_record_is_iosurface(const UnityExportRecord* record)
{
    return record != NULL &&
           (strcmp(record->payloadFormat, "IOSurfaceBGRA8") == 0 ||
            strcmp(record->payloadFormat, "IOSurfaceRGBA8") == 0);
}

static CVPixelBufferRef copy_iosurface_to_encoder_pixel_buffer(
    EncoderContext* context,
    const UnityExportRecord* record,
    IOSurfaceRef surface)
{
    if (context == NULL || record == NULL || surface == NULL ||
        (strcmp(record->payloadFormat, "IOSurfaceBGRA8") != 0 &&
         strcmp(record->payloadFormat, "IOSurfaceRGBA8") != 0) ||
        (int)IOSurfaceGetWidth(surface) != record->width ||
        (int)IOSurfaceGetHeight(surface) != record->height ||
        IOSurfaceGetBytesPerRow(surface) < (size_t)record->width * 4u) {
        return NULL;
    }

    CVPixelBufferRef copiedPixelBuffer = create_encoder_pool_pixel_buffer(context);
    if (copiedPixelBuffer == NULL) {
        return NULL;
    }

    if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
        CVPixelBufferRelease(copiedPixelBuffer);
        return NULL;
    }
    if (CVPixelBufferLockBaseAddress(copiedPixelBuffer, 0) != kCVReturnSuccess) {
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        CVPixelBufferRelease(copiedPixelBuffer);
        return NULL;
    }

    const uint8_t* sourceBase = (const uint8_t*)IOSurfaceGetBaseAddress(surface);
    const size_t sourceBytesPerRow = IOSurfaceGetBytesPerRow(surface);
    uint8_t* destinationBase = (uint8_t*)CVPixelBufferGetBaseAddress(copiedPixelBuffer);
    const size_t destinationBytesPerRow = CVPixelBufferGetBytesPerRow(copiedPixelBuffer);
    int copied = 0;
    if (strcmp(record->payloadFormat, "IOSurfaceRGBA8") == 0) {
        copied = swizzle_rgba_to_bgra_rows(
            sourceBase,
            sourceBytesPerRow,
            destinationBase,
            destinationBytesPerRow,
            record->width,
            record->height);
    } else {
        const size_t copiedRowBytes = (size_t)record->width * 4u;
        copied = 1;
        for (int y = 0; y < record->height; ++y) {
            const uint8_t* sourceRow = sourceBase + ((size_t)y * sourceBytesPerRow);
            uint8_t* destinationRow = destinationBase + ((size_t)y * destinationBytesPerRow);
            memcpy(destinationRow, sourceRow, copiedRowBytes);
        }
    }

    CVPixelBufferUnlockBaseAddress(copiedPixelBuffer, 0);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    if (!copied) {
        CVPixelBufferRelease(copiedPixelBuffer);
        return NULL;
    }

    return copiedPixelBuffer;
}

static CVPixelBufferRef create_unity_export_iosurface_pixel_buffer(
    EncoderContext* context,
    const UnityExportRecord* record,
    uint32_t* sharedIoSurfaceId)
{
    if (context == NULL || record == NULL ||
        !unity_export_record_is_iosurface(record) ||
        record->ioSurfaceId == 0 ||
        record->width != context->width ||
        record->height != context->height ||
        record->bytesPerRow < (size_t)record->width * 4u) {
        return NULL;
    }

    IOSurfaceRef surface = IOSurfaceLookup(record->ioSurfaceId);
    if (surface == NULL) {
        return NULL;
    }
    if ((int)IOSurfaceGetWidth(surface) != record->width ||
        (int)IOSurfaceGetHeight(surface) != record->height ||
        IOSurfaceGetBytesPerRow(surface) < (size_t)record->width * 4u) {
        CFRelease(surface);
        return NULL;
    }

    const int sourceIsRgba = strcmp(record->payloadFormat, "IOSurfaceRGBA8") == 0;
    const int reuseHazard = shared_iosurface_is_pending(context, record->ioSurfaceId);
    if (sourceIsRgba || reuseHazard) {
        CVPixelBufferRef copiedPixelBuffer =
            copy_iosurface_to_encoder_pixel_buffer(context, record, surface);
        if (copiedPixelBuffer != NULL && reuseHazard) {
            printf("{\"event\":\"iosurface_reuse_guard\",\"eye\":%d,"
                   "\"source_frame\":%" PRIu64 ",\"io_surface_id\":%u,"
                   "\"mode\":\"host_copy\"}\n",
                   record->eye,
                   record->sourceFrameId,
                   record->ioSurfaceId);
            fflush(stdout);
        }
        CFRelease(surface);
        return copiedPixelBuffer;
    }

    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (attributes == NULL) {
        CFRelease(surface);
        return NULL;
    }

    set_cf_int(attributes, kCVPixelBufferWidthKey, record->width);
    set_cf_int(attributes, kCVPixelBufferHeightKey, record->height);
    const OSType pixelFormat = kCVPixelFormatType_32BGRA;
    set_cf_int(attributes, kCVPixelBufferPixelFormatTypeKey, (int)pixelFormat);
    CFDictionarySetValue(attributes, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);

    CVPixelBufferRef pixelBuffer = NULL;
    const CVReturn status = CVPixelBufferCreateWithIOSurface(
        kCFAllocatorDefault,
        surface,
        attributes,
        &pixelBuffer);

    CFRelease(attributes);
    CFRelease(surface);

    if (status != kCVReturnSuccess || pixelBuffer == NULL) {
        return NULL;
    }

    if ((int)CVPixelBufferGetWidth(pixelBuffer) != context->width ||
        (int)CVPixelBufferGetHeight(pixelBuffer) != context->height ||
        CVPixelBufferGetPixelFormatType(pixelBuffer) != pixelFormat) {
        CVPixelBufferRelease(pixelBuffer);
        return NULL;
    }

    if (sharedIoSurfaceId != NULL) {
        *sharedIoSurfaceId = record->ioSurfaceId;
    }
    return pixelBuffer;
}

static CVPixelBufferRef create_unity_export_pixel_buffer(
    EncoderContext* context,
    const UnityExportRecord* record,
    uint32_t* sharedIoSurfaceId)
{
    if (sharedIoSurfaceId != NULL) {
        *sharedIoSurfaceId = 0;
    }
    if (unity_export_record_is_iosurface(record)) {
        return create_unity_export_iosurface_pixel_buffer(context, record, sharedIoSurfaceId);
    }

    if (context == NULL || record == NULL ||
        (strcmp(record->payloadFormat, "BGRA8") != 0 &&
         strcmp(record->payloadFormat, "RGBA8") != 0) ||
        record->width <= 0 || record->height <= 0 ||
        record->width != context->width ||
        record->height != context->height ||
        record->bytesPerRow < (size_t)record->width * 4u ||
        record->bytesPerRow > SIZE_MAX / (size_t)record->height ||
        record->payloadBytes < record->bytesPerRow * (size_t)record->height ||
        record->payloadBytes > METALXR_MAX_UNITY_EXPORT_PAYLOAD_BYTES) {
        return NULL;
    }

    uint8_t* payload = (uint8_t*)malloc(record->payloadBytes);
    if (payload == NULL) {
        return NULL;
    }

    if (!read_file_exact(record->payloadPath, payload, record->payloadBytes)) {
        free(payload);
        return NULL;
    }

    CVPixelBufferRef pixelBuffer = create_encoder_pool_pixel_buffer(context);
    if (pixelBuffer == NULL) {
        free(payload);
        return NULL;
    }

    if (CVPixelBufferLockBaseAddress(pixelBuffer, 0) != kCVReturnSuccess) {
        CVPixelBufferRelease(pixelBuffer);
        free(payload);
        return NULL;
    }

    uint8_t* base = (uint8_t*)CVPixelBufferGetBaseAddress(pixelBuffer);
    const size_t destinationBytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
    const size_t copiedRowBytes = (size_t)record->width * 4u;
    const int sourceIsRgba = strcmp(record->payloadFormat, "RGBA8") == 0;
    if (sourceIsRgba) {
        if (!swizzle_rgba_to_bgra_rows(
                payload,
                record->bytesPerRow,
                base,
                destinationBytesPerRow,
                record->width,
                record->height)) {
            CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
            CVPixelBufferRelease(pixelBuffer);
            free(payload);
            return NULL;
        }
    } else {
        for (int y = 0; y < record->height; ++y) {
            const uint8_t* sourceRow = payload + ((size_t)y * record->bytesPerRow);
            uint8_t* destinationRow = base + ((size_t)y * destinationBytesPerRow);
            memcpy(destinationRow, sourceRow, copiedRowBytes);
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
    free(payload);
    return pixelBuffer;
}

static int create_encoder(EncoderContext* context, const StreamerOptions* options, StreamSession* stream)
{
    context->width = options->width;
    context->height = options->height;
    context->fps = options->fps;
    context->stream = stream;
    context->maxQueueDepth = (uint32_t)options->queueDepth;
    pthread_mutex_init(&context->queueLock, NULL);
    context->queueLockInitialized = 1;
    context->pixelBufferPool = create_encoder_pixel_buffer_pool(
        options->width,
        options->height,
        options->queueDepth + 2);
    if (context->pixelBufferPool == NULL) {
        fprintf(stderr, "CVPixelBufferPoolCreate failed for %s %dx%d\n",
                context->eyeName,
                options->width,
                options->height);
        return 0;
    }
    printf("{\"event\":\"pixel_buffer_pool\",\"eye\":%d,\"eye_name\":\"%s\","
           "\"width\":%d,\"height\":%d,\"min_buffers\":%d,"
           "\"iosurface\":true,\"metal_compatible\":true}\n",
           context->eyeIndex,
           context->eyeName,
           options->width,
           options->height,
           options->queueDepth + 2);
    fflush(stdout);

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

static int submit_pixel_buffer_frame(
    EncoderContext* context,
    uint64_t frameId,
    uint64_t captureTimeNs,
    uint64_t predictedDisplayTimeNs,
    CVPixelBufferRef pixelBuffer,
    uint32_t sharedIoSurfaceId,
    const UnityExportRecord* unityRecord)
{
    if (!reserve_encoder_queue_slot(context, frameId)) {
        return 1;
    }

    if (sharedIoSurfaceId != 0 && !reserve_shared_iosurface(context, sharedIoSurfaceId)) {
        fprintf(stderr,
                "{\"event\":\"drop\",\"reason\":\"shared_iosurface_pending\","
                "\"frame\":%" PRIu64 ",\"eye\":%d,\"io_surface_id\":%u}\n",
                frameId,
                context->eyeIndex,
                sharedIoSurfaceId);
        ++context->stats.droppedFrames;
        release_encoder_queue_slot(context);
        return 0;
    }

    if (pixelBuffer == NULL) {
        fprintf(stderr, "Missing pixel buffer for %s frame %" PRIu64 "\n",
                context->eyeName,
                frameId);
        ++context->stats.droppedFrames;
        release_shared_iosurface(context, sharedIoSurfaceId);
        release_encoder_queue_slot(context);
        return 0;
    }

    FrameRefcon* frame = (FrameRefcon*)calloc(1, sizeof(*frame));
    if (frame == NULL) {
        release_shared_iosurface(context, sharedIoSurfaceId);
        release_encoder_queue_slot(context);
        return 0;
    }

    frame->frameId = frameId;
    frame->eyeIndex = context->eyeIndex;
    frame->width = context->width;
    frame->height = context->height;
    frame->imageRectX = 0;
    frame->imageRectY = 0;
    frame->imageRectWidth = (uint32_t)context->width;
    frame->imageRectHeight = (uint32_t)context->height;
    frame->imageArrayIndex = 0;
    frame->projectionFlags = 0;
    frame->referenceSpaceId = 0;
    frame->poseOrientation[3] = 1.0f;
    frame->fov[0] = -0.7853982f;
    frame->fov[1] = 0.7853982f;
    frame->fov[2] = 0.7853982f;
    frame->fov[3] = -0.7853982f;
    if (unityRecord != NULL) {
        frame->imageRectX = unityRecord->imageRectX;
        frame->imageRectY = unityRecord->imageRectY;
        frame->imageRectWidth = unityRecord->imageRectWidth;
        frame->imageRectHeight = unityRecord->imageRectHeight;
        frame->imageArrayIndex = unityRecord->imageArrayIndex;
        frame->projectionFlags = unityRecord->projectionFlags;
        frame->referenceSpaceId = unityRecord->referenceSpaceId;
        memcpy(frame->posePosition, unityRecord->posePosition, sizeof(frame->posePosition));
        memcpy(frame->poseOrientation, unityRecord->poseOrientation, sizeof(frame->poseOrientation));
        memcpy(frame->fov, unityRecord->fov, sizeof(frame->fov));
    }
    frame->captureTimeNs = captureTimeNs;
    frame->predictedDisplayTimeNs = predictedDisplayTimeNs;
    frame->encodeStartTimeNs = host_time_ns();
    frame->sharedIoSurfaceId = sharedIoSurfaceId;

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

    if (status != noErr) {
        fprintf(stderr, "VTCompressionSessionEncodeFrame failed for %s frame %" PRIu64 ": %d\n",
                context->eyeName,
                frameId,
        (int)status);
        ++context->stats.droppedFrames;
        free(frame);
        release_shared_iosurface(context, sharedIoSurfaceId);
        release_encoder_queue_slot(context);
        return 0;
    }

    ++context->stats.submittedFrames;
    return 1;
}

static int encode_synthetic_eye_frame(
    EncoderContext* context,
    uint64_t frameId,
    uint64_t captureTimeNs,
    uint64_t predictedDisplayTimeNs)
{
    CVPixelBufferRef pixelBuffer = create_synthetic_pixel_buffer(context, frameId);
    if (pixelBuffer == NULL) {
        fprintf(stderr, "Failed to create synthetic pixel buffer for %s frame %" PRIu64 "\n",
                context->eyeName,
                frameId);
        ++context->stats.droppedFrames;
        return 0;
    }

    const int ok = submit_pixel_buffer_frame(
        context,
        frameId,
        captureTimeNs,
        predictedDisplayTimeNs,
        pixelBuffer,
        0,
        NULL);
    CVPixelBufferRelease(pixelBuffer);
    return ok;
}

static int encode_unity_export_eye_frame(
    EncoderContext* context,
    uint64_t frameId,
    uint64_t captureTimeNs,
    uint64_t predictedDisplayTimeNs,
    const UnityExportRecord* record)
{
    if (record == NULL || record->width != context->width || record->height != context->height) {
        fprintf(stderr,
                "{\"event\":\"drop\",\"reason\":\"unity_export_dimensions\","
                "\"frame\":%" PRIu64 ",\"eye\":%d,\"expected_width\":%d,"
                "\"expected_height\":%d}\n",
                frameId,
                context->eyeIndex,
                context->width,
                context->height);
        ++context->stats.droppedFrames;
        return 0;
    }

    uint32_t sharedIoSurfaceId = 0;
    CVPixelBufferRef pixelBuffer =
        create_unity_export_pixel_buffer(context, record, &sharedIoSurfaceId);
    if (pixelBuffer == NULL) {
        fprintf(stderr,
                "{\"event\":\"drop\",\"reason\":\"unity_export_payload\","
                "\"frame\":%" PRIu64 ",\"source_frame\":%" PRIu64 ","
                "\"eye\":%d,\"payload\":\"%s\",\"io_surface_id\":%u}\n",
                frameId,
                record->sourceFrameId,
                record->eye,
                record->payloadPath,
                record->ioSurfaceId);
        ++context->stats.droppedFrames;
        return 0;
    }

    const int ok = submit_pixel_buffer_frame(
        context,
        frameId,
        captureTimeNs,
        predictedDisplayTimeNs,
        pixelBuffer,
        sharedIoSurfaceId,
        record);
    CVPixelBufferRelease(pixelBuffer);
    return ok;
}

static void destroy_encoder(EncoderContext* context)
{
    if (context->session != NULL) {
        VTCompressionSessionCompleteFrames(context->session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(context->session);
        CFRelease(context->session);
        context->session = NULL;
    }
    if (context->pixelBufferPool != NULL) {
        CVPixelBufferPoolRelease(context->pixelBufferPool);
        context->pixelBufferPool = NULL;
    }
    if (context->queueLockInitialized) {
        pthread_mutex_destroy(&context->queueLock);
        context->queueLockInitialized = 0;
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

static uint64_t add_signed_ns(uint64_t value, int64_t offsetNs)
{
    if (offsetNs >= 0) {
        return value + (uint64_t)offsetNs;
    }

    const uint64_t magnitude = (uint64_t)(-offsetNs);
    return value > magnitude ? value - magnitude : 0;
}

static uint64_t elapsed_us(uint64_t newerNs, uint64_t olderNs)
{
    return newerNs >= olderNs ? (newerNs - olderNs) / 1000ull : 0;
}

static int64_t signed_elapsed_us(uint64_t newerNs, uint64_t olderNs)
{
    if (newerNs >= olderNs) {
        return (int64_t)((newerNs - olderNs) / 1000ull);
    }

    return -(int64_t)((olderNs - newerNs) / 1000ull);
}

static uint64_t host_time_from_client_time(const HostTimingState* timing, uint64_t clientTimeNs)
{
    if (timing == NULL || !timing->hasClockSync || clientTimeNs == 0) {
        return 0;
    }

    const int64_t hostTimeNs = (int64_t)clientTimeNs - timing->clockOffsetNs;
    return hostTimeNs > 0 ? (uint64_t)hostTimeNs : 0;
}

static HostTimingState make_default_timing_state(const StreamerOptions* options)
{
    HostTimingState timing;
    memset(&timing, 0, sizeof(timing));
    timing.measuredFramePeriodNs = 1000000000ull / (uint64_t)options->fps;
    timing.predictionOffsetNs = options->predictionOffsetNs;
    timing.lastFrameId = UINT64_MAX;
    return timing;
}

static uint64_t next_predicted_display_time_ns(
    const HostTimingState* timing,
    uint64_t captureTimeNs,
    uint64_t fallbackFramePeriodNs)
{
    const uint64_t periodNs =
        timing != NULL && timing->measuredFramePeriodNs > 0 ?
            timing->measuredFramePeriodNs :
            fallbackFramePeriodNs;

    if (timing != NULL && timing->lastDisplayHostNs > 0 &&
        captureTimeNs >= timing->lastDisplayHostNs &&
        captureTimeNs - timing->lastDisplayHostNs < 2000000000ull) {
        uint64_t predictedNs = timing->lastDisplayHostNs;
        while (predictedNs <= captureTimeNs) {
            predictedNs += periodNs;
        }
        return add_signed_ns(predictedNs, timing->predictionOffsetNs);
    }

    return add_signed_ns(captureTimeNs + periodNs, timing != NULL ? timing->predictionOffsetNs : 0);
}

static void write_timing_state_file(const StreamSession* session)
{
    if (session == NULL || session->timingStatePath[0] == '\0') {
        return;
    }

    char tempPath[1200];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", session->timingStatePath);
    FILE* output = fopen(tempPath, "w");
    if (output == NULL) {
        return;
    }

    const HostTimingState* timing = &session->timing;
    const uint64_t averageLatencyUs =
        timing->totalLatencySamples > 0 ?
            timing->totalLatencyUs / timing->totalLatencySamples :
            0;
    fprintf(output,
            "timing %" PRIu64 " %" PRIu64 " %" PRId64 " %" PRIu64 " %" PRIu64
            " %" PRIu64 " %" PRId64 " %" PRIu64 " %" PRIu64 " %" PRIu64
            " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRId64 " %u\n",
            timing->timingSamples,
            host_time_ns(),
            timing->clockOffsetNs,
            timing->clockRttNs,
            timing->measuredFramePeriodNs,
            timing->lastDisplayHostNs,
            timing->predictionOffsetNs,
            averageLatencyUs,
            timing->maxLatencyUs,
            timing->lastEncodeUs,
            timing->lastNetworkUs,
            timing->lastDecodeUs,
            timing->lastCompositorUs,
            timing->lastTotalUs,
            timing->lastPredictionErrorUs,
            timing->lastQueueDepth);
    fclose(output);
    (void)rename(tempPath, session->timingStatePath);
}

static uint64_t touch_shared_state_host_heartbeat(StreamSession* session)
{
    if (session == NULL || !session->hasSharedState) {
        return 0;
    }

    const uint64_t timestampNs = host_time_ns();
    metalxr_shared_state_write_host_heartbeat(&session->sharedState, timestampNs);
    return timestampNs;
}

static void fill_shared_timing_state(const StreamSession* session, MetalXRSharedTimingState* shared)
{
    if (shared == NULL) {
        return;
    }
    memset(shared, 0, sizeof(*shared));
    if (session == NULL) {
        return;
    }

    const HostTimingState* timing = &session->timing;
    shared->hostTimestampNs = timing->lastDisplayHostNs;
    shared->timingSamples = timing->timingSamples;
    shared->clockOffsetNs = timing->clockOffsetNs;
    shared->clockRttNs = timing->clockRttNs;
    shared->measuredFramePeriodNs = timing->measuredFramePeriodNs;
    shared->lastDisplayHostNs = timing->lastDisplayHostNs;
    shared->predictionOffsetNs = timing->predictionOffsetNs;
    shared->averageLatencyUs =
        timing->totalLatencySamples > 0 ?
            timing->totalLatencyUs / timing->totalLatencySamples :
            0;
    shared->maxLatencyUs = timing->maxLatencyUs;
    shared->lastEncodeUs = timing->lastEncodeUs;
    shared->lastNetworkUs = timing->lastNetworkUs;
    shared->lastDecodeUs = timing->lastDecodeUs;
    shared->lastCompositorUs = timing->lastCompositorUs;
    shared->lastTotalUs = timing->lastTotalUs;
    shared->lastPredictionErrorUs = timing->lastPredictionErrorUs;
    shared->lastQueueDepth = timing->lastQueueDepth;
}

static void publish_timing_state(StreamSession* session)
{
    if (session == NULL) {
        return;
    }

    if (session->hasSharedState) {
        MetalXRSharedTimingState shared;
        const uint64_t hostTimestampNs = touch_shared_state_host_heartbeat(session);
        fill_shared_timing_state(session, &shared);
        shared.hostTimestampNs = hostTimestampNs;
        metalxr_shared_state_write_timing(&session->sharedState, &shared);
    }
    write_timing_state_file(session);
}

static int reserve_encoder_queue_slot(EncoderContext* context, uint64_t frameId)
{
    pthread_mutex_lock(&context->queueLock);
    if (context->pendingFrames >= context->maxQueueDepth) {
        const uint32_t pending = context->pendingFrames;
        pthread_mutex_unlock(&context->queueLock);
        ++context->stats.droppedFrames;
        fprintf(stderr,
                "{\"event\":\"drop\",\"reason\":\"queue_depth\",\"frame\":%" PRIu64
                ",\"eye\":%d,\"pending\":%u,\"limit\":%u}\n",
                frameId,
                context->eyeIndex,
                pending,
                context->maxQueueDepth);
        return 0;
    }

    ++context->pendingFrames;
    pthread_mutex_unlock(&context->queueLock);
    return 1;
}

static void release_encoder_queue_slot(EncoderContext* context)
{
    pthread_mutex_lock(&context->queueLock);
    if (context->pendingFrames > 0) {
        --context->pendingFrames;
    }
    pthread_mutex_unlock(&context->queueLock);
}

static int shared_iosurface_is_pending(EncoderContext* context, uint32_t ioSurfaceId)
{
    if (context == NULL || ioSurfaceId == 0) {
        return 0;
    }

    int pending = 0;
    pthread_mutex_lock(&context->queueLock);
    for (uint32_t i = 0; i < context->pendingSharedSurfaceCount; ++i) {
        if (context->pendingSharedSurfaceIds[i] == ioSurfaceId) {
            pending = 1;
            break;
        }
    }
    pthread_mutex_unlock(&context->queueLock);
    return pending;
}

static int reserve_shared_iosurface(EncoderContext* context, uint32_t ioSurfaceId)
{
    if (context == NULL || ioSurfaceId == 0) {
        return 1;
    }

    int reserved = 0;
    pthread_mutex_lock(&context->queueLock);
    for (uint32_t i = 0; i < context->pendingSharedSurfaceCount; ++i) {
        if (context->pendingSharedSurfaceIds[i] == ioSurfaceId) {
            pthread_mutex_unlock(&context->queueLock);
            return 0;
        }
    }

    if (context->pendingSharedSurfaceCount < METALXR_MAX_PENDING_SHARED_SURFACES) {
        context->pendingSharedSurfaceIds[context->pendingSharedSurfaceCount] = ioSurfaceId;
        ++context->pendingSharedSurfaceCount;
        reserved = 1;
    }
    pthread_mutex_unlock(&context->queueLock);
    return reserved;
}

static void release_shared_iosurface(EncoderContext* context, uint32_t ioSurfaceId)
{
    if (context == NULL || ioSurfaceId == 0) {
        return;
    }

    pthread_mutex_lock(&context->queueLock);
    for (uint32_t i = 0; i < context->pendingSharedSurfaceCount; ++i) {
        if (context->pendingSharedSurfaceIds[i] != ioSurfaceId) {
            continue;
        }

        const uint32_t last = context->pendingSharedSurfaceCount - 1u;
        context->pendingSharedSurfaceIds[i] = context->pendingSharedSurfaceIds[last];
        context->pendingSharedSurfaceIds[last] = 0;
        --context->pendingSharedSurfaceCount;
        break;
    }
    pthread_mutex_unlock(&context->queueLock);
}

static HostInputState make_default_input_state(void)
{
    HostInputState state;
    memset(&state, 0, sizeof(state));
    state.hmd.orientation[3] = 1.0f;
    state.hmd.trackingFlags = METALXR_TRACKING_ORIENTATION_VALID |
                              METALXR_TRACKING_POSITION_VALID;
    for (size_t hand = 0; hand < 2; ++hand) {
        state.controllers[hand].hand = (uint32_t)hand;
        state.controllers[hand].aimOrientation[3] = 1.0f;
        state.controllers[hand].gripOrientation[3] = 1.0f;
    }
    return state;
}

static void write_tracking_state_file(const StreamSession* session, const HostInputState* state)
{
    if (session == NULL || state == NULL || session->trackingStatePath[0] == '\0') {
        return;
    }

    char tempPath[1200];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", session->trackingStatePath);
    FILE* output = fopen(tempPath, "w");
    if (output == NULL) {
        return;
    }

    fprintf(output,
            "hmd %" PRIu64 " %" PRIu64 " %u %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
            state->hmd.sampleId,
            state->hmd.timestampNs,
            state->hmd.trackingFlags,
            state->hmd.position[0],
            state->hmd.position[1],
            state->hmd.position[2],
            state->hmd.orientation[0],
            state->hmd.orientation[1],
            state->hmd.orientation[2],
            state->hmd.orientation[3]);

    for (size_t hand = 0; hand < 2; ++hand) {
        const HostControllerState* controller = &state->controllers[hand];
        fprintf(output,
                "controller %u %" PRIu64 " %" PRIu64 " %u %u %.9g %.9g %.9g %.9g "
                "%.9g %.9g %.9g %.9g %.9g %.9g %.9g "
                "%.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
                controller->hand,
                controller->sampleId,
                controller->timestampNs,
                controller->trackingFlags,
                controller->buttons,
                controller->trigger,
                controller->grip,
                controller->thumbstick[0],
                controller->thumbstick[1],
                controller->aimPosition[0],
                controller->aimPosition[1],
                controller->aimPosition[2],
                controller->aimOrientation[0],
                controller->aimOrientation[1],
                controller->aimOrientation[2],
                controller->aimOrientation[3],
                controller->gripPosition[0],
                controller->gripPosition[1],
                controller->gripPosition[2],
                controller->gripOrientation[0],
                controller->gripOrientation[1],
                controller->gripOrientation[2],
                controller->gripOrientation[3]);
    }

    fclose(output);
    (void)rename(tempPath, session->trackingStatePath);
}

static void fill_shared_tracking_state(
    const HostInputState* state,
    uint64_t hostTimestampNs,
    MetalXRSharedTrackingState* shared)
{
    if (shared == NULL) {
        return;
    }
    memset(shared, 0, sizeof(*shared));
    shared->hostTimestampNs = hostTimestampNs;
    if (state == NULL) {
        return;
    }

    shared->hmd.sampleId = state->hmd.sampleId;
    shared->hmd.timestampNs = state->hmd.timestampNs;
    shared->hmd.trackingFlags = state->hmd.trackingFlags;
    memcpy(shared->hmd.position, state->hmd.position, sizeof(shared->hmd.position));
    memcpy(shared->hmd.orientation, state->hmd.orientation, sizeof(shared->hmd.orientation));

    for (size_t hand = 0; hand < 2; ++hand) {
        const HostControllerState* source = &state->controllers[hand];
        MetalXRControllerInputPayload* target = &shared->controllers[hand];
        target->sampleId = source->sampleId;
        target->timestampNs = source->timestampNs;
        target->hand = source->hand;
        target->buttons = source->buttons;
        target->trackingFlags = source->trackingFlags;
        target->trigger = source->trigger;
        target->grip = source->grip;
        memcpy(target->thumbstick, source->thumbstick, sizeof(target->thumbstick));
        memcpy(target->aimPosition, source->aimPosition, sizeof(target->aimPosition));
        memcpy(target->aimOrientation, source->aimOrientation, sizeof(target->aimOrientation));
        memcpy(target->gripPosition, source->gripPosition, sizeof(target->gripPosition));
        memcpy(target->gripOrientation, source->gripOrientation, sizeof(target->gripOrientation));
    }
}

static void publish_tracking_state(StreamSession* session, const HostInputState* state)
{
    if (session == NULL || state == NULL) {
        return;
    }

    if (session->hasSharedState) {
        MetalXRSharedTrackingState shared;
        const uint64_t hostTimestampNs = touch_shared_state_host_heartbeat(session);
        fill_shared_tracking_state(state, hostTimestampNs, &shared);
        metalxr_shared_state_write_tracking(&session->sharedState, &shared);
    }
    write_tracking_state_file(session, state);
}

static int socket_has_data(int fd)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    const int ready = select(fd + 1, &readSet, NULL, NULL, &timeout);
    return ready > 0 && FD_ISSET(fd, &readSet);
}

static void handle_pose_sample(StreamSession* session, HostInputState* state, const MetalXRPoseSamplePayload* pose)
{
    state->hmd.sampleId = pose->sampleId;
    state->hmd.timestampNs = pose->timestampNs;
    state->hmd.trackingFlags = pose->trackingFlags;
    memcpy(state->hmd.position, pose->position, sizeof(state->hmd.position));
    memcpy(state->hmd.orientation, pose->orientation, sizeof(state->hmd.orientation));
    ++state->poseSamples;

    if (state->poseSamples <= 3 || (state->poseSamples % 300) == 0) {
        printf("{\"event\":\"pose\",\"sample\":%" PRIu64 ",\"flags\":%u,"
               "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}\n",
               pose->sampleId,
               pose->trackingFlags,
               pose->position[0],
               pose->position[1],
               pose->position[2]);
        fflush(stdout);
    }

    publish_tracking_state(session, state);
}

static void handle_controller_input(
    StreamSession* session,
    HostInputState* state,
    const MetalXRControllerInputPayload* input)
{
    if (input->hand > METALXR_CONTROLLER_HAND_RIGHT) {
        return;
    }

    HostControllerState* controller = &state->controllers[input->hand];
    controller->sampleId = input->sampleId;
    controller->timestampNs = input->timestampNs;
    controller->hand = input->hand;
    controller->buttons = input->buttons;
    controller->trackingFlags = input->trackingFlags;
    controller->trigger = input->trigger;
    controller->grip = input->grip;
    memcpy(controller->thumbstick, input->thumbstick, sizeof(controller->thumbstick));
    memcpy(controller->aimPosition, input->aimPosition, sizeof(controller->aimPosition));
    memcpy(controller->aimOrientation, input->aimOrientation, sizeof(controller->aimOrientation));
    memcpy(controller->gripPosition, input->gripPosition, sizeof(controller->gripPosition));
    memcpy(controller->gripOrientation, input->gripOrientation, sizeof(controller->gripOrientation));
    ++state->controllerSamples;

    if (state->controllerSamples <= 6 || (state->controllerSamples % 600) == 0) {
        printf("{\"event\":\"controller\",\"sample\":%" PRIu64 ",\"hand\":%u,"
               "\"buttons\":%u,\"trigger\":%.4f,\"grip\":%.4f}\n",
               input->sampleId,
               input->hand,
               input->buttons,
               input->trigger,
               input->grip);
        fflush(stdout);
    }

    write_tracking_state_file(session, state);
}

static int send_timing_sample(StreamSession* session, const MetalXRTimingSamplePayload* sample)
{
    pthread_mutex_lock(&session->sendLock);
    MetalXRPacketHeader header = metalxr_make_header(
        METALXR_PACKET_TIMING_SAMPLE,
        session->sequence++,
        metalxr_protocol_now_ns(),
        sizeof(*sample));
    const int sent = metalxr_protocol_send_packet(session->clientFd, &header, sample);
    if (!sent) {
        session->failed = 1;
    }
    pthread_mutex_unlock(&session->sendLock);
    return sent;
}

static void send_clock_sync_if_due(StreamSession* session, uint64_t intervalNs)
{
    const uint64_t nowNs = host_time_ns();
    if (session == NULL || session->failed ||
        (session->timing.lastClockSyncSendNs != 0 &&
         nowNs - session->timing.lastClockSyncSendNs < intervalNs)) {
        return;
    }

    MetalXRTimingSamplePayload sample;
    memset(&sample, 0, sizeof(sample));
    sample.frameId = UINT64_MAX;
    sample.hostCaptureTimeNs = nowNs;
    sample.flags = METALXR_TIMING_FLAG_CLOCK_SYNC;
    if (send_timing_sample(session, &sample)) {
        session->timing.lastClockSyncSendNs = nowNs;
    }
}

static void update_measured_frame_period(HostTimingState* timing, uint64_t frameId, uint64_t displayHostNs)
{
    if (timing == NULL || displayHostNs == 0 || frameId == timing->lastFrameId) {
        return;
    }

    if (timing->lastFrameId != UINT64_MAX && frameId > timing->lastFrameId &&
        displayHostNs > timing->lastDisplayHostNs) {
        const uint64_t frameDelta = frameId - timing->lastFrameId;
        const uint64_t measuredNs = (displayHostNs - timing->lastDisplayHostNs) / frameDelta;
        if (measuredNs > 1000000ull && measuredNs < 1000000000ull) {
            if (timing->measuredFramePeriodNs == 0) {
                timing->measuredFramePeriodNs = measuredNs;
            } else {
                timing->measuredFramePeriodNs =
                    ((timing->measuredFramePeriodNs * 7ull) + measuredNs) / 8ull;
            }
        }
    }

    timing->lastFrameId = frameId;
    timing->lastDisplayHostNs = displayHostNs;
}

static void handle_timing_sample(
    StreamSession* session,
    const MetalXRTimingSamplePayload* sample,
    uint64_t hostReceiveTimeNs)
{
    HostTimingState* timing = &session->timing;

    if ((sample->flags & METALXR_TIMING_FLAG_CLOCK_SYNC) != 0) {
        if (sample->hostCaptureTimeNs != 0 && hostReceiveTimeNs >= sample->hostCaptureTimeNs &&
            sample->clientReceiveTimeNs != 0) {
            const uint64_t rttNs = hostReceiveTimeNs - sample->hostCaptureTimeNs;
            const uint64_t midpointNs = sample->hostCaptureTimeNs + (rttNs / 2ull);
            const int64_t offsetNs = (int64_t)sample->clientReceiveTimeNs - (int64_t)midpointNs;
            timing->clockOffsetNs = timing->hasClockSync ?
                ((timing->clockOffsetNs * 7) + offsetNs) / 8 :
                offsetNs;
            timing->clockRttNs = rttNs;
            timing->hasClockSync = 1;
            ++timing->timingSamples;

            if (timing->timingSamples <= 3 || (timing->timingSamples % 120) == 0) {
                printf("{\"event\":\"clock_sync\",\"offset_ns\":%" PRId64
                       ",\"rtt_us\":%" PRIu64 ",\"queue_depth\":%u}\n",
                       timing->clockOffsetNs,
                       timing->clockRttNs / 1000ull,
                       sample->queueDepth);
                fflush(stdout);
            }

            publish_timing_state(session);
        }
        return;
    }

    if ((sample->flags & METALXR_TIMING_FLAG_FRAME_DISPLAY) == 0) {
        return;
    }

    const uint64_t clientDisplayNs =
        sample->clientCompositorSubmitTimeNs != 0 ?
            sample->clientCompositorSubmitTimeNs :
            sample->clientDisplayTimeNs;
    const uint64_t displayHostNs = host_time_from_client_time(timing, clientDisplayNs);
    const uint64_t receiveHostNs = host_time_from_client_time(timing, sample->clientReceiveTimeNs);

    ++timing->timingSamples;
    update_measured_frame_period(timing, sample->frameId, displayHostNs);
    timing->lastEncodeUs = elapsed_us(sample->encodeEndTimeNs, sample->encodeStartTimeNs);
    timing->lastNetworkUs = elapsed_us(receiveHostNs, sample->encodeEndTimeNs);
    timing->lastDecodeUs = elapsed_us(sample->clientDecodeEndTimeNs, sample->clientDecodeStartTimeNs);
    timing->lastCompositorUs = elapsed_us(clientDisplayNs, sample->clientDecodeEndTimeNs);
    timing->lastTotalUs = elapsed_us(displayHostNs, sample->hostCaptureTimeNs);
    timing->lastPredictionErrorUs = signed_elapsed_us(displayHostNs, sample->predictedDisplayTimeNs);
    timing->lastQueueDepth = sample->queueDepth;

    if (timing->lastTotalUs > 0) {
        ++timing->totalLatencySamples;
        timing->totalLatencyUs += timing->lastTotalUs;
        if (timing->lastTotalUs > timing->maxLatencyUs) {
            timing->maxLatencyUs = timing->lastTotalUs;
        }
    }

    if (timing->totalLatencySamples <= 8 || (timing->totalLatencySamples % 120) == 0) {
        printf("{\"event\":\"latency\",\"frame\":%" PRIu64
               ",\"encode_us\":%" PRIu64 ",\"network_us\":%" PRIu64
               ",\"decode_us\":%" PRIu64 ",\"compositor_us\":%" PRIu64
               ",\"total_us\":%" PRIu64 ",\"prediction_error_us\":%" PRId64
               ",\"queue_depth\":%u,\"period_ns\":%" PRIu64 "}\n",
               sample->frameId,
               timing->lastEncodeUs,
               timing->lastNetworkUs,
               timing->lastDecodeUs,
               timing->lastCompositorUs,
               timing->lastTotalUs,
               timing->lastPredictionErrorUs,
               timing->lastQueueDepth,
               timing->measuredFramePeriodNs);
        fflush(stdout);
    }

    publish_timing_state(session);
}

static void drain_client_input_packets(StreamSession* session, HostInputState* state)
{
    while (!session->failed && socket_has_data(session->clientFd)) {
        MetalXRPacketHeader header;
        uint8_t payload[sizeof(MetalXRControllerInputPayload)];
        if (!metalxr_protocol_recv_packet(session->clientFd, &header, payload, sizeof(payload))) {
            session->failed = 1;
            return;
        }

        if (header.type == METALXR_PACKET_POSE_SAMPLE &&
            header.payloadSize == sizeof(MetalXRPoseSamplePayload)) {
            MetalXRPoseSamplePayload pose;
            memcpy(&pose, payload, sizeof(pose));
            handle_pose_sample(session, state, &pose);
        } else if (header.type == METALXR_PACKET_CONTROLLER_INPUT &&
                   header.payloadSize == sizeof(MetalXRControllerInputPayload)) {
            MetalXRControllerInputPayload input;
            memcpy(&input, payload, sizeof(input));
            handle_controller_input(session, state, &input);
        } else if (header.type == METALXR_PACKET_TIMING_SAMPLE &&
                   header.payloadSize == sizeof(MetalXRTimingSamplePayload)) {
            MetalXRTimingSamplePayload sample;
            memcpy(&sample, payload, sizeof(sample));
            handle_timing_sample(session, &sample, host_time_ns());
        }
    }
}

static int send_haptic_command(StreamSession* session, const MetalXRHapticCommandPayload* command)
{
    pthread_mutex_lock(&session->sendLock);
    MetalXRPacketHeader header = metalxr_make_header(
        METALXR_PACKET_HAPTIC_COMMAND,
        session->sequence++,
        metalxr_protocol_now_ns(),
        sizeof(*command));
    const int sent = metalxr_protocol_send_packet(session->clientFd, &header, command);
    if (!sent) {
        session->failed = 1;
    }
    pthread_mutex_unlock(&session->sendLock);
    return sent;
}

static int forward_haptic_command(StreamSession* session, const MetalXRHapticCommandPayload* command)
{
    if (session == NULL || command == NULL ||
        command->commandId == session->lastHapticCommandId ||
        command->hand > METALXR_CONTROLLER_HAND_RIGHT) {
        return 0;
    }

    session->lastHapticCommandId = command->commandId;
    if (send_haptic_command(session, command)) {
        printf("{\"event\":\"haptic\",\"command\":%" PRIu64 ",\"hand\":%u,"
               "\"amplitude\":%.4f,\"duration_us\":%u}\n",
               command->commandId,
               command->hand,
               command->amplitude,
               command->durationUs);
        fflush(stdout);
        return 1;
    }

    return 0;
}

static void poll_haptic_command_shared_state(StreamSession* session)
{
    if (session == NULL || !session->hasSharedState) {
        return;
    }

    (void)touch_shared_state_host_heartbeat(session);

    MetalXRHapticCommandPayload command;
    memset(&command, 0, sizeof(command));
    uint32_t sequence = 0;
    if (!metalxr_shared_state_read_haptic(&session->sharedState, &command, &sequence) ||
        sequence == session->lastSharedHapticSequence ||
        command.commandId == 0) {
        return;
    }

    session->lastSharedHapticSequence = sequence;
    (void)forward_haptic_command(session, &command);
}

static void poll_haptic_command_file(StreamSession* session)
{
    if (session == NULL || session->hapticCommandPath[0] == '\0') {
        return;
    }

    FILE* input = fopen(session->hapticCommandPath, "r");
    if (input == NULL) {
        return;
    }

    MetalXRHapticCommandPayload command;
    memset(&command, 0, sizeof(command));
    const int scanned = fscanf(input,
                               "%" SCNu64 " %" SCNu64 " %u %f %f %u",
                               &command.commandId,
                               &command.timestampNs,
                               &command.hand,
                               &command.amplitude,
                               &command.frequencyHz,
                               &command.durationUs);
    fclose(input);

    if (scanned != 6) {
        return;
    }

    (void)forward_haptic_command(session, &command);
}

static void poll_haptic_commands(StreamSession* session)
{
    poll_haptic_command_shared_state(session);
    poll_haptic_command_file(session);
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
                       METALXR_CAPABILITY_POSE_INPUT |
                       METALXR_CAPABILITY_CONTROLLER_INPUT |
                       METALXR_CAPABILITY_HAPTICS |
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

static int client_dimension_limit(uint32_t value)
{
    if (value == 0 || value > 8192u) {
        return 0;
    }
    return (int)value;
}

static int client_preferred_fps(uint32_t value)
{
    if (value == 0) {
        return 0;
    }
    if (value > 240u) {
        return 240;
    }
    return (int)value;
}

static int apply_client_device_profile(
    StreamerOptions* activeOptions,
    const MetalXRHelloPayload* clientHello,
    int dimensionsCanResize,
    char* errorMessage,
    size_t errorMessageSize)
{
    if (activeOptions == NULL || clientHello == NULL) {
        return 0;
    }

    const int maxWidth = client_dimension_limit(clientHello->maxVideoWidth);
    const int maxHeight = client_dimension_limit(clientHello->maxVideoHeight);
    const int preferredFps = client_preferred_fps(clientHello->preferredFps);
    int resizedWidth = 0;
    int resizedHeight = 0;
    int fpsFromClient = 0;
    int fpsCapped = 0;

    if (maxWidth > 0 && activeOptions->width > maxWidth) {
        if (dimensionsCanResize && !activeOptions->widthExplicit) {
            activeOptions->width = maxWidth;
            resizedWidth = 1;
        } else {
            snprintf(errorMessage,
                     errorMessageSize,
                     "Stream width %d exceeds Quest device profile max width %d",
                     activeOptions->width,
                     maxWidth);
            return 0;
        }
    }

    if (maxHeight > 0 && activeOptions->height > maxHeight) {
        if (dimensionsCanResize && !activeOptions->heightExplicit) {
            activeOptions->height = maxHeight;
            resizedHeight = 1;
        } else {
            snprintf(errorMessage,
                     errorMessageSize,
                     "Stream height %d exceeds Quest device profile max height %d",
                     activeOptions->height,
                     maxHeight);
            return 0;
        }
    }

    if (preferredFps > 0) {
        if (!activeOptions->fpsExplicit) {
            activeOptions->fps = preferredFps;
            fpsFromClient = 1;
        } else if (activeOptions->fps > preferredFps) {
            activeOptions->fps = preferredFps;
            fpsCapped = 1;
        }
    }

    printf("{\"event\":\"stream_profile\","
           "\"client_max_width\":%u,\"client_max_height\":%u,"
           "\"client_preferred_fps\":%u,\"effective_width\":%d,"
           "\"effective_height\":%d,\"effective_fps\":%d,"
           "\"resized_width\":%s,\"resized_height\":%s,"
           "\"fps_from_client\":%s,\"fps_capped\":%s}\n",
           clientHello->maxVideoWidth,
           clientHello->maxVideoHeight,
           clientHello->preferredFps,
           activeOptions->width,
           activeOptions->height,
           activeOptions->fps,
           resizedWidth ? "true" : "false",
           resizedHeight ? "true" : "false",
           fpsFromClient ? "true" : "false",
           fpsCapped ? "true" : "false");
    fflush(stdout);

    return 1;
}

static int configure_unity_export_stream(
    StreamerOptions* activeOptions,
    UnityExportFrameSource* source,
    UnityExportFramePair* initialPair)
{
    if (activeOptions == NULL || activeOptions->frameSource != FRAME_SOURCE_UNITY_EXPORT) {
        return 1;
    }

    unity_export_source_init(source, activeOptions->frameExportDir, activeOptions->frameExportSocketPath);
    if (!unity_export_source_open_socket(source)) {
        return 0;
    }

    const uint64_t waitStartNs = host_time_ns();
    const uint64_t waitDeadlineNs =
        waitStartNs + ((uint64_t)activeOptions->frameExportWaitMs * 1000000ull);
    while (!poll_unity_export_frame_source(source, initialPair) &&
           source->socketPath[0] != '\0' &&
           host_time_ns() < waitDeadlineNs) {
        struct timespec sleepTime;
        sleepTime.tv_sec = 0;
        sleepTime.tv_nsec = 10000000;
        (void)nanosleep(&sleepTime, NULL);
    }

    if (!source->hasLatestPair) {
        fprintf(stderr,
                "No complete Unity frame export pair found in dir=%s socket=%s. "
                "Start Unity Play Mode with METALXR_FRAME_EXPORT_DIR or METALXR_FRAME_EXPORT_SOCKET set before using unity-export source.\n",
                activeOptions->frameExportDir[0] != '\0' ? activeOptions->frameExportDir : "-",
                activeOptions->frameExportSocketPath[0] != '\0' ? activeOptions->frameExportSocketPath : "-");
        unity_export_source_close(source);
        return 0;
    }

    if (initialPair->eyes[0].width != initialPair->eyes[1].width ||
        initialPair->eyes[0].height != initialPair->eyes[1].height) {
        fprintf(stderr,
                "Unity export eye dimensions do not match: left=%dx%d right=%dx%d\n",
                initialPair->eyes[0].width,
                initialPair->eyes[0].height,
                initialPair->eyes[1].width,
                initialPair->eyes[1].height);
        unity_export_source_close(source);
        return 0;
    }

    activeOptions->width = initialPair->eyes[0].width;
    activeOptions->height = initialPair->eyes[0].height;
    printf("{\"event\":\"frame_source\",\"source\":\"unity-export\","
           "\"state\":\"ready\",\"source_frame\":%" PRIu64 ",\"width\":%d,"
           "\"height\":%d,\"age_ms\":%" PRIu64 ",\"export_dir\":\"%s\","
           "\"export_socket\":\"%s\"}\n",
           initialPair->sourceFrameId,
           activeOptions->width,
           activeOptions->height,
           unity_export_pair_age_ms(initialPair),
           activeOptions->frameExportDir[0] != '\0' ? activeOptions->frameExportDir : "-",
           activeOptions->frameExportSocketPath[0] != '\0' ? activeOptions->frameExportSocketPath : "-");
    fflush(stdout);
    return 1;
}

static int stream_client(int clientFd, const StreamerOptions* options)
{
    MetalXRHelloPayload clientHello;
    uint64_t firstSequence = 0;
    if (!receive_client_hello(clientFd, &clientHello, &firstSequence)) {
        return 0;
    }

    StreamerOptions activeOptions = *options;
    UnityExportFrameSource unityExportSource;
    memset(&unityExportSource, 0, sizeof(unityExportSource));
    unityExportSource.socketFd = -1;
    UnityExportFramePair initialUnityPair;
    memset(&initialUnityPair, 0, sizeof(initialUnityPair));

    StreamSession stream;
    memset(&stream, 0, sizeof(stream));
    stream.clientFd = clientFd;
    stream.sequence = firstSequence + 1;
    snprintf(stream.trackingStatePath, sizeof(stream.trackingStatePath), "%s", activeOptions.trackingStatePath);
    snprintf(stream.hapticCommandPath, sizeof(stream.hapticCommandPath), "%s", activeOptions.hapticCommandPath);
    snprintf(stream.timingStatePath, sizeof(stream.timingStatePath), "%s", activeOptions.timingStatePath);
    if (activeOptions.useSharedState) {
        char sharedStateError[256];
        sharedStateError[0] = '\0';
        stream.hasSharedState = metalxr_shared_state_open(&stream.sharedState,
                                                          activeOptions.sharedStateName,
                                                          1,
                                                          sharedStateError,
                                                          sizeof(sharedStateError));
        if (stream.hasSharedState) {
            (void)touch_shared_state_host_heartbeat(&stream);
            printf("{\"event\":\"shared_state\",\"state\":\"ready\",\"name\":\"%s\"}\n",
                   activeOptions.sharedStateName);
            fflush(stdout);
        } else {
            fprintf(stderr,
                    "{\"event\":\"shared_state\",\"state\":\"fallback\",\"reason\":\"%s\"}\n",
                    sharedStateError[0] != '\0' ? sharedStateError : "open failed");
        }
    }
    stream.timing = make_default_timing_state(&activeOptions);
    pthread_mutex_init(&stream.sendLock, NULL);
    HostInputState inputState = make_default_input_state();

    if (!configure_unity_export_stream(&activeOptions, &unityExportSource, &initialUnityPair)) {
        if (stream.hasSharedState) {
            metalxr_shared_state_close(&stream.sharedState);
        }
        unity_export_source_close(&unityExportSource);
        pthread_mutex_destroy(&stream.sendLock);
        return 0;
    }

    char profileError[256];
    profileError[0] = '\0';
    if (!apply_client_device_profile(&activeOptions,
                                     &clientHello,
                                     activeOptions.frameSource == FRAME_SOURCE_SYNTHETIC,
                                     profileError,
                                     sizeof(profileError))) {
        (void)send_error(clientFd,
                         stream.sequence++,
                         METALXR_ERROR_UNSUPPORTED_CAPABILITY,
                         profileError[0] != '\0' ? profileError : "Quest device profile is incompatible with stream options");
        fprintf(stderr, "Rejected stream profile: %s\n",
                profileError[0] != '\0' ? profileError : "Quest device profile is incompatible with stream options");
        if (stream.hasSharedState) {
            metalxr_shared_state_close(&stream.sharedState);
        }
        unity_export_source_close(&unityExportSource);
        pthread_mutex_destroy(&stream.sendLock);
        return 0;
    }
    stream.timing = make_default_timing_state(&activeOptions);
    publish_timing_state(&stream);

    if (!send_hello_ack(clientFd, &stream, &activeOptions)) {
        if (stream.hasSharedState) {
            metalxr_shared_state_close(&stream.sharedState);
        }
        pthread_mutex_destroy(&stream.sendLock);
        return 0;
    }

    EncoderContext left;
    EncoderContext right;
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    left.eyeName = "left";
    left.eyeIndex = 0;
    left.stream = &stream;
    right.eyeName = "right";
    right.eyeIndex = 1;
    right.stream = &stream;
    int ok = 1;

    if (!create_encoder(&left, &activeOptions, &stream) ||
        !create_encoder(&right, &activeOptions, &stream)) {
        ok = 0;
    }

    const uint64_t startNs = host_time_ns();
    const uint64_t framePeriodNs = 1000000000ull / (uint64_t)activeOptions.fps;
    const uint64_t clockSyncIntervalNs = (uint64_t)activeOptions.clockSyncIntervalMs * 1000000ull;
    for (int frame = 0; ok && !stream.failed && (activeOptions.frames == 0 || frame < activeOptions.frames); ++frame) {
        drain_client_input_packets(&stream, &inputState);
        poll_haptic_commands(&stream);
        send_clock_sync_if_due(&stream, clockSyncIntervalNs);

        const uint64_t captureTimeNs = host_time_ns();
        uint64_t predictedDisplayTimeNs =
            next_predicted_display_time_ns(&stream.timing, captureTimeNs, framePeriodNs);

        if (activeOptions.frameSource == FRAME_SOURCE_UNITY_EXPORT) {
            UnityExportFramePair pair;
            if (!poll_unity_export_frame_source(&unityExportSource, &pair)) {
                fprintf(stderr,
                        "{\"event\":\"drop\",\"reason\":\"unity_export_missing\","
                        "\"frame\":%d,\"missing_polls\":%" PRIu64 "}\n",
                        frame,
                        unityExportSource.missingPolls);
                ok = 0;
            } else if (!validate_unity_export_pair_dimensions(&pair, activeOptions.width, activeOptions.height)) {
                fprintf(stderr,
                        "{\"event\":\"drop\",\"reason\":\"unity_export_dimensions\","
                        "\"frame\":%d,\"source_frame\":%" PRIu64 ","
                        "\"expected_width\":%d,\"expected_height\":%d}\n",
                        frame,
                        pair.sourceFrameId,
                        activeOptions.width,
                        activeOptions.height);
                ok = 0;
            } else {
                if (is_plausible_host_display_time(pair.eyes[0].displayTimeNs, captureTimeNs)) {
                    predictedDisplayTimeNs = pair.eyes[0].displayTimeNs;
                }
                if (frame < 4 || (frame % (activeOptions.fps * 2)) == 0) {
                    printf("{\"event\":\"frame_source\",\"source\":\"unity-export\","
                           "\"frame\":%d,\"source_frame\":%" PRIu64 ","
                           "\"age_ms\":%" PRIu64 ",\"repeated\":%" PRIu64 "}\n",
                           frame,
                           pair.sourceFrameId,
                           unity_export_pair_age_ms(&pair),
                           unityExportSource.repeatedFrames);
                    fflush(stdout);
                }

                if (!encode_unity_export_eye_frame(&left,
                                                   (uint64_t)frame,
                                                   captureTimeNs,
                                                   predictedDisplayTimeNs,
                                                   &pair.eyes[0]) ||
                    !encode_unity_export_eye_frame(&right,
                                                   (uint64_t)frame,
                                                   captureTimeNs,
                                                   predictedDisplayTimeNs,
                                                   &pair.eyes[1])) {
                    ok = 0;
                }
            }
        } else if (!encode_synthetic_eye_frame(&left, (uint64_t)frame, captureTimeNs, predictedDisplayTimeNs) ||
                   !encode_synthetic_eye_frame(&right, (uint64_t)frame, captureTimeNs, predictedDisplayTimeNs)) {
            ok = 0;
        }

        if (activeOptions.realtime) {
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
    unity_export_source_close(&unityExportSource);
    pthread_mutex_destroy(&stream.sendLock);
    if (stream.hasSharedState) {
        metalxr_shared_state_close(&stream.sharedState);
    }
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

    printf("MetalXR host streamer listening on %s:%d width=%d height=%d fps=%d bitrate=%d frames=%d queue_depth=%d reconnect_attempts=%d source=%s export_dir=%s export_socket=%s export_wait_ms=%d prediction_offset_ns=%" PRId64 " shared_state=%s tracking=%s haptics=%s timing=%s\n",
           options.bindHost,
           options.port,
           options.width,
           options.height,
           options.fps,
           options.bitrate,
           options.frames,
           options.queueDepth,
           options.reconnectAttempts,
           frame_source_name(options.frameSource),
           options.frameExportDir[0] != '\0' ? options.frameExportDir : "-",
           options.frameExportSocketPath[0] != '\0' ? options.frameExportSocketPath : "-",
           options.frameExportWaitMs,
           options.predictionOffsetNs,
           options.useSharedState ? options.sharedStateName : "disabled",
           options.trackingStatePath,
           options.hapticCommandPath,
           options.timingStatePath);
    fflush(stdout);

    int exitCode = 0;
    int reconnectsUsed = 0;
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
            const int canReconnect = options.frames == 0 || reconnectsUsed < options.reconnectAttempts;
            fprintf(stderr,
                    "{\"event\":\"client_disconnect\",\"reconnects_used\":%d,"
                    "\"reconnect_attempts\":%d,\"recovering\":%s}\n",
                    reconnectsUsed,
                    options.reconnectAttempts,
                    canReconnect ? "true" : "false");
            if (!canReconnect) {
                fprintf(stderr, "Quest stream client disconnected before the stream completed\n");
                exitCode = 1;
                break;
            }

            if (options.frames > 0) {
                ++reconnectsUsed;
            }
            printf("{\"event\":\"reconnect_wait\",\"reconnects_used\":%d,"
                   "\"reconnect_attempts\":%d,\"frames\":%d}\n",
                   reconnectsUsed,
                   options.reconnectAttempts,
                   options.frames);
            printf("Waiting for Quest stream reconnect...\n");
            fflush(stdout);
            continue;
        }

        if (options.frames > 0) {
            break;
        }

        printf("{\"event\":\"reconnect_wait\",\"reconnects_used\":%d,"
               "\"reconnect_attempts\":%d,\"frames\":%d}\n",
               reconnectsUsed,
               options.reconnectAttempts,
               options.frames);
        printf("Waiting for Quest stream reconnect...\n");
        fflush(stdout);
    }

    close(serverFd);
    return exitCode;
}
