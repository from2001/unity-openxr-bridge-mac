#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct EncoderOptions {
    int width;
    int height;
    int fps;
    int frames;
    int bitrate;
    int realtime;
    char outputPrefix[1024];
} EncoderOptions;

typedef struct EncoderStats {
    uint64_t submittedFrames;
    uint64_t encodedFrames;
    uint64_t droppedFrames;
    uint64_t totalBytes;
    uint64_t totalLatencyUs;
    uint64_t maxLatencyUs;
} EncoderStats;

typedef struct EncoderContext {
    const char* eyeName;
    int eyeIndex;
    int width;
    int height;
    int fps;
    VTCompressionSessionRef session;
    FILE* elementaryStream;
    FILE* metadata;
    EncoderStats stats;
} EncoderContext;

typedef struct FrameRefcon {
    uint64_t frameId;
    int eyeIndex;
    int width;
    int height;
    int64_t timestampNs;
    int64_t predictedDisplayTimeNs;
    uint64_t submitTimeNs;
} FrameRefcon;

static uint64_t host_time_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return ((uint64_t)now.tv_sec * 1000000000ull) + (uint64_t)now.tv_nsec;
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
            "Usage: %s [--output-prefix PATH] [--frames N] [--fps N] "
            "[--width N] [--height N] [--bitrate BPS] [--realtime]\n",
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

static EncoderOptions parse_options(int argc, char** argv)
{
    EncoderOptions options;
    memset(&options, 0, sizeof(options));
    options.width = 640;
    options.height = 360;
    options.fps = 60;
    options.frames = 120;
    options.bitrate = 8000000;
    snprintf(options.outputPrefix, sizeof(options.outputPrefix), "%s", "/tmp/metalxr_host_encoder");

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--output-prefix") == 0 && i + 1 < argc) {
            snprintf(options.outputPrefix, sizeof(options.outputPrefix), "%s", argv[++i]);
        } else if (strcmp(arg, "--frames") == 0 && i + 1 < argc) {
            options.frames = parse_int_arg(argv[++i], "frames", 1, 1000000);
        } else if (strcmp(arg, "--fps") == 0 && i + 1 < argc) {
            options.fps = parse_int_arg(argv[++i], "fps", 1, 240);
        } else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            options.width = parse_int_arg(argv[++i], "width", 16, 8192);
        } else if (strcmp(arg, "--height") == 0 && i + 1 < argc) {
            options.height = parse_int_arg(argv[++i], "height", 16, 8192);
        } else if (strcmp(arg, "--bitrate") == 0 && i + 1 < argc) {
            options.bitrate = parse_int_arg(argv[++i], "bitrate", 100000, 200000000);
        } else if (strcmp(arg, "--realtime") == 0) {
            options.realtime = 1;
        } else {
            print_usage(argv[0]);
            exit(2);
        }
    }

    return options;
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

static int write_annex_b_nal(FILE* output, const uint8_t* data, size_t length)
{
    static const uint8_t startCode[] = { 0x00, 0x00, 0x00, 0x01 };
    if (fwrite(startCode, 1, sizeof(startCode), output) != sizeof(startCode)) {
        return 0;
    }

    return fwrite(data, 1, length, output) == length;
}

static int write_h264_parameter_sets(FILE* output, CMFormatDescriptionRef formatDescription)
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

        if (!write_annex_b_nal(output, parameterSet, parameterSetSize)) {
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

static size_t write_sample_buffer_annex_b(FILE* output, CMSampleBufferRef sampleBuffer)
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

    size_t bytesWritten = 0;
    size_t offset = 0;
    while (offset + 4 <= totalLength) {
        const uint32_t nalLength = read_big_endian_u32((const uint8_t*)dataPointer + offset);
        offset += 4;
        if (nalLength == 0 || offset + nalLength > totalLength) {
            break;
        }

        if (!write_annex_b_nal(output, (const uint8_t*)dataPointer + offset, nalLength)) {
            break;
        }

        bytesWritten += 4 + nalLength;
        offset += nalLength;
    }

    return bytesWritten;
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
        frame != NULL && completeNs >= frame->submitTimeNs ?
            (completeNs - frame->submitTimeNs) / 1000ull :
            0;

    if (status != noErr || sampleBuffer == NULL || !CMSampleBufferDataIsReady(sampleBuffer)) {
        ++context->stats.droppedFrames;
        if (context->metadata != NULL && frame != NULL) {
            fprintf(context->metadata,
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

    size_t bytesWritten = 0;
    if (is_keyframe(sampleBuffer)) {
        CMFormatDescriptionRef formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (formatDescription != NULL) {
            (void)write_h264_parameter_sets(context->elementaryStream, formatDescription);
        }
    }
    bytesWritten += write_sample_buffer_annex_b(context->elementaryStream, sampleBuffer);
    fflush(context->elementaryStream);

    ++context->stats.encodedFrames;
    context->stats.totalBytes += bytesWritten;
    context->stats.totalLatencyUs += latencyUs;
    if (latencyUs > context->stats.maxLatencyUs) {
        context->stats.maxLatencyUs = latencyUs;
    }

    if (context->metadata != NULL && frame != NULL) {
        fprintf(context->metadata,
                "{\"event\":\"encoded\",\"frame\":%" PRIu64 ",\"eye\":%d,"
                "\"eye_name\":\"%s\",\"width\":%d,\"height\":%d,"
                "\"timestamp_ns\":%" PRId64 ",\"predicted_display_time_ns\":%" PRId64 ","
                "\"latency_us\":%" PRIu64 ",\"bytes\":%zu}\n",
                frame->frameId,
                frame->eyeIndex,
                context->eyeName,
                frame->width,
                frame->height,
                frame->timestampNs,
                frame->predictedDisplayTimeNs,
                latencyUs,
                bytesWritten);
        fflush(context->metadata);
    }

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

static int open_output_files(EncoderContext* context, const EncoderOptions* options)
{
    char streamPath[1200];
    snprintf(streamPath,
             sizeof(streamPath),
             "%s_%s.h264",
             options->outputPrefix,
             context->eyeName);
    context->elementaryStream = fopen(streamPath, "wb");
    if (context->elementaryStream == NULL) {
        fprintf(stderr, "Failed to open output stream: %s\n", streamPath);
        return 0;
    }

    if (context->eyeIndex == 0) {
        char metadataPath[1200];
        snprintf(metadataPath, sizeof(metadataPath), "%s_metadata.jsonl", options->outputPrefix);
        context->metadata = fopen(metadataPath, "w");
        if (context->metadata == NULL) {
            fprintf(stderr, "Failed to open metadata: %s\n", metadataPath);
            return 0;
        }
    }

    return 1;
}

static int create_encoder(EncoderContext* context, const EncoderOptions* options, FILE* sharedMetadata)
{
    context->width = options->width;
    context->height = options->height;
    context->fps = options->fps;
    if (context->eyeIndex != 0) {
        context->metadata = sharedMetadata;
    }

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
    int64_t timestampNs,
    int64_t predictedDisplayTimeNs)
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
    frame->timestampNs = timestampNs;
    frame->predictedDisplayTimeNs = predictedDisplayTimeNs;
    frame->submitTimeNs = host_time_ns();

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

    if (context->elementaryStream != NULL) {
        fclose(context->elementaryStream);
        context->elementaryStream = NULL;
    }
}

static void write_summary(FILE* metadata, const EncoderContext* context)
{
    const uint64_t averageLatency =
        context->stats.encodedFrames > 0 ?
            context->stats.totalLatencyUs / context->stats.encodedFrames :
            0;
    fprintf(metadata,
            "{\"event\":\"summary\",\"eye\":%d,\"eye_name\":\"%s\","
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

int main(int argc, char** argv)
{
    const EncoderOptions options = parse_options(argc, argv);
    EncoderContext left = { "left", 0, 0, 0, 0, NULL, NULL, NULL, { 0 } };
    EncoderContext right = { "right", 1, 0, 0, 0, NULL, NULL, NULL, { 0 } };

    if (!open_output_files(&left, &options)) {
        destroy_encoder(&left);
        return 1;
    }
    if (!open_output_files(&right, &options)) {
        destroy_encoder(&left);
        destroy_encoder(&right);
        if (left.metadata != NULL) {
            fclose(left.metadata);
        }
        return 1;
    }

    if (!create_encoder(&left, &options, left.metadata) ||
        !create_encoder(&right, &options, left.metadata)) {
        destroy_encoder(&left);
        destroy_encoder(&right);
        if (left.metadata != NULL) {
            fclose(left.metadata);
        }
        return 1;
    }

    const uint64_t startNs = host_time_ns();
    const uint64_t framePeriodNs = 1000000000ull / (uint64_t)options.fps;
    for (int frame = 0; frame < options.frames; ++frame) {
        const int64_t timestampNs = (int64_t)(host_time_ns() - startNs);
        const int64_t predictedDisplayTimeNs = (int64_t)((uint64_t)(frame + 1) * framePeriodNs);

        (void)encode_eye_frame(&left, (uint64_t)frame, timestampNs, predictedDisplayTimeNs);
        (void)encode_eye_frame(&right, (uint64_t)frame, timestampNs, predictedDisplayTimeNs);

        if (options.realtime) {
            sleep_until_ns(startNs + ((uint64_t)(frame + 1) * framePeriodNs));
        }
    }

    VTCompressionSessionCompleteFrames(left.session, kCMTimeInvalid);
    VTCompressionSessionCompleteFrames(right.session, kCMTimeInvalid);

    write_summary(left.metadata, &left);
    write_summary(left.metadata, &right);
    fflush(left.metadata);

    printf("MetalXR host encoder wrote %s_left.h264 and %s_right.h264\n",
           options.outputPrefix,
           options.outputPrefix);
    printf("left: submitted=%" PRIu64 " encoded=%" PRIu64 " dropped=%" PRIu64
           " bytes=%" PRIu64 " max_latency_us=%" PRIu64 "\n",
           left.stats.submittedFrames,
           left.stats.encodedFrames,
           left.stats.droppedFrames,
           left.stats.totalBytes,
           left.stats.maxLatencyUs);
    printf("right: submitted=%" PRIu64 " encoded=%" PRIu64 " dropped=%" PRIu64
           " bytes=%" PRIu64 " max_latency_us=%" PRIu64 "\n",
           right.stats.submittedFrames,
           right.stats.encodedFrames,
           right.stats.droppedFrames,
           right.stats.totalBytes,
           right.stats.maxLatencyUs);

    destroy_encoder(&left);
    destroy_encoder(&right);
    if (left.metadata != NULL) {
        fclose(left.metadata);
    }

    return left.stats.droppedFrames == 0 && right.stats.droppedFrames == 0 ? 0 : 1;
}
