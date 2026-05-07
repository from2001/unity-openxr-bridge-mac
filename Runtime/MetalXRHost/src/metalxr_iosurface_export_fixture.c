#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t fixture_time_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return ((uint64_t)now.tv_sec * 1000000000ull) + (uint64_t)now.tv_nsec;
}

static void set_cf_int(CFMutableDictionaryRef dictionary, const void* key, int value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    if (number == NULL) {
        return;
    }
    CFDictionarySetValue(dictionary, key, number);
    CFRelease(number);
}

static IOSurfaceRef create_fixture_surface(int width, int height)
{
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (properties == NULL) {
        return NULL;
    }

    set_cf_int(properties, kIOSurfaceWidth, width);
    set_cf_int(properties, kIOSurfaceHeight, height);
    set_cf_int(properties, kIOSurfaceBytesPerElement, 4);
    set_cf_int(properties, kIOSurfacePixelFormat, kCVPixelFormatType_32BGRA);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFDictionarySetValue(properties, kIOSurfaceIsGlobal, kCFBooleanTrue);
#pragma clang diagnostic pop

    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    return surface;
}

static int fill_fixture_surface(IOSurfaceRef surface, int width, int height, int eye)
{
    if (surface == NULL || IOSurfaceLock(surface, 0, NULL) != kIOReturnSuccess) {
        return 0;
    }

    uint8_t* base = (uint8_t*)IOSurfaceGetBaseAddress(surface);
    const size_t bytesPerRow = IOSurfaceGetBytesPerRow(surface);
    if (base == NULL || bytesPerRow < (size_t)width * 4u) {
        IOSurfaceUnlock(surface, 0, NULL);
        return 0;
    }

    for (int y = 0; y < height; ++y) {
        uint8_t* row = base + ((size_t)y * bytesPerRow);
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = row + ((size_t)x * 4u);
            pixel[0] = (uint8_t)((x * 3 + eye * 64) & 0xff);
            pixel[1] = (uint8_t)((y * 5 + eye * 31) & 0xff);
            pixel[2] = (uint8_t)((x + y + eye * 120) & 0xff);
            pixel[3] = 255;
        }
    }

    IOSurfaceUnlock(surface, 0, NULL);
    return 1;
}

static int write_fixture_record(
    FILE* index,
    const char* fixtureDir,
    uint64_t frame,
    int eye,
    int width,
    int height,
    size_t bytesPerRow,
    uint32_t ioSurfaceId,
    uint64_t displayTime)
{
    char recordPath[1024];
    snprintf(recordPath,
             sizeof(recordPath),
             "%s/frame_%06" PRIu64 "_eye_%d.json",
             fixtureDir,
             frame,
             eye);

    FILE* record = fopen(recordPath, "w");
    if (record == NULL) {
        return 0;
    }

    const char* format =
        "{\"event\":\"frame_export\",\"frame\":%" PRIu64 ",\"eye\":%d,"
        "\"displayTime\":%" PRIu64 ",\"swapchain\":1,\"imageIndex\":0,"
        "\"texture\":\"iosurface-fixture\",\"pixelFormat\":80,"
        "\"payloadFormat\":\"IOSurfaceBGRA8\","
        "\"width\":%d,\"height\":%d,\"bytesPerRow\":%zu,\"payloadBytes\":0,"
        "\"ioSurfaceId\":%u,"
        "\"frameSlotId\":%u,\"frameSlotGeneration\":%" PRIu64 ","
        "\"frameSlotState\":\"ready\",\"frameSlotFence\":\"fixture-ready\","
        "\"sourceRect\":{\"x\":0,\"y\":0,\"width\":%d,\"height\":%d},"
        "\"imageRectX\":0,\"imageRectY\":0,\"imageRectWidth\":%d,\"imageRectHeight\":%d,"
        "\"imageArrayIndex\":0,\"projectionFlags\":1,\"referenceSpaceId\":0,"
        "\"posePositionX\":0,\"posePositionY\":0,\"posePositionZ\":0,"
        "\"poseOrientationX\":0,\"poseOrientationY\":0,"
        "\"poseOrientationZ\":0,\"poseOrientationW\":1,"
        "\"fovAngleLeft\":-0.7853982,\"fovAngleRight\":0.7853982,"
        "\"fovAngleUp\":0.7853982,\"fovAngleDown\":-0.7853982,"
        "\"arrayIndex\":0,\"storageMode\":0,\"mode\":\"iosurface-fixture\","
        "\"payloadPath\":\"\"}\n";

    fprintf(record,
            format,
            frame,
            eye,
            displayTime,
            width,
            height,
            bytesPerRow,
            ioSurfaceId,
            ioSurfaceId,
            frame + 1u,
            width,
            height,
            width,
            height);
    fclose(record);

    fprintf(index,
            format,
            frame,
            eye,
            displayTime,
            width,
            height,
            bytesPerRow,
            ioSurfaceId,
            ioSurfaceId,
            frame + 1u,
            width,
            height,
            width,
            height);
    fflush(index);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s <fixture-dir> <width> <height> <frames> <hold-seconds>\n", argv[0]);
        return 2;
    }

    const char* fixtureDir = argv[1];
    const int width = atoi(argv[2]);
    const int height = atoi(argv[3]);
    const int frames = atoi(argv[4]);
    const int holdSeconds = atoi(argv[5]);
    if (width <= 0 || height <= 0 || frames <= 0 || holdSeconds <= 0) {
        fprintf(stderr, "invalid fixture dimensions or duration\n");
        return 2;
    }

    if (mkdir(fixtureDir, 0755) != 0) {
        /* Existing mktemp-created directories are expected. */
    }

    IOSurfaceRef surfaces[2] = {
        create_fixture_surface(width, height),
        create_fixture_surface(width, height),
    };
    if (surfaces[0] == NULL || surfaces[1] == NULL ||
        !fill_fixture_surface(surfaces[0], width, height, 0) ||
        !fill_fixture_surface(surfaces[1], width, height, 1)) {
        fprintf(stderr, "failed to create IOSurface fixture surfaces\n");
        if (surfaces[0] != NULL) {
            CFRelease(surfaces[0]);
        }
        if (surfaces[1] != NULL) {
            CFRelease(surfaces[1]);
        }
        return 1;
    }

    char indexPath[1024];
    snprintf(indexPath, sizeof(indexPath), "%s/frames.jsonl", fixtureDir);
    FILE* index = fopen(indexPath, "w");
    if (index == NULL) {
        fprintf(stderr, "failed to write %s\n", indexPath);
        CFRelease(surfaces[0]);
        CFRelease(surfaces[1]);
        return 1;
    }

    const uint64_t displayBase = fixture_time_ns();
    for (int frame = 0; frame < frames; ++frame) {
        const uint64_t displayTime = displayBase + ((uint64_t)(frame + 1) * 16666666ull);
        for (int eye = 0; eye < 2; ++eye) {
            if (!write_fixture_record(index,
                                      fixtureDir,
                                      (uint64_t)frame,
                                      eye,
                                      width,
                                      height,
                                      IOSurfaceGetBytesPerRow(surfaces[eye]),
                                      IOSurfaceGetID(surfaces[eye]),
                                      displayTime)) {
                fprintf(stderr, "failed to write IOSurface fixture record\n");
                fclose(index);
                CFRelease(surfaces[0]);
                CFRelease(surfaces[1]);
                return 1;
            }
        }
    }

    fclose(index);
    printf("iosurface_fixture dir=%s left=%u right=%u\n",
           fixtureDir,
           IOSurfaceGetID(surfaces[0]),
           IOSurfaceGetID(surfaces[1]));
    fflush(stdout);
    sleep((unsigned int)holdSeconds);

    CFRelease(surfaces[0]);
    CFRelease(surfaces[1]);
    return 0;
}
