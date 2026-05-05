#include "MetalXRRuntime/openxr_minimal.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const XrVersion kSupportedApiVersion = XR_MAKE_VERSION(1, 0, 0);
static const XrVersion kRuntimeVersion = XR_MAKE_VERSION(0, 3, 0);
static const XrSystemId kDummySystemId = 1;
static const XrDuration kFramePeriodNs = 11111111;
static const uint32_t kRecommendedEyeWidth = 1832;
static const uint32_t kRecommendedEyeHeight = 1920;

enum {
    kViewCount = 2,
    kSwapchainImageCount = 3,
    kMaxSessionSwapchains = 16,
};

static const uint32_t kInstanceMagic = 0x4d584931;
static const uint32_t kSessionMagic = 0x4d585331;
static const uint32_t kSpaceMagic = 0x4d585850;
static const uint32_t kSwapchainMagic = 0x4d585343;

enum {
    kMetalPixelFormatRGBA8Unorm = 70,
    kMetalPixelFormatRGBA8UnormSrgb = 71,
    kMetalPixelFormatBGRA8Unorm = 80,
    kMetalPixelFormatBGRA8UnormSrgb = 81,
    kMetalPixelFormatRGB10A2Unorm = 90,
    kMetalPixelFormatRGBA16Float = 115,
};

enum {
    kMetalTextureType2D = 2,
    kMetalTextureType2DArray = 3,
    kMetalStorageModePrivate = 2,
    kMetalTextureUsageShaderRead = 0x0001,
    kMetalTextureUsageShaderWrite = 0x0002,
    kMetalTextureUsageRenderTarget = 0x0004,
    kMetalTextureUsagePixelFormatView = 0x0010,
};

typedef struct MetalXrExtension {
    const char* name;
    uint32_t version;
} MetalXrExtension;

static const MetalXrExtension kSupportedExtensions[] = {
    { "XR_KHR_metal_enable", 3 },
    { "XR_KHRX2_metal_enable", 1 },
};

typedef struct MetalXrEventQueue {
    XrEventDataSessionStateChanged events[64];
    uint32_t head;
    uint32_t count;
} MetalXrEventQueue;

struct XrInstance_T {
    uint32_t magic;
    uint64_t id;
    XrSession session;
};

struct XrSession_T {
    uint32_t magic;
    uint64_t id;
    XrInstance instance;
    XrSystemId systemId;
    XrSessionState state;
    void* metalCommandQueue;
    void* metalDevice;
    XrBool32 running;
    XrBool32 frameBegun;
    uint64_t frameIndex;
    XrTime nextFrameTime;
    XrSpace spaces[16];
    uint32_t spaceCount;
    XrSwapchain swapchains[kMaxSessionSwapchains];
    uint32_t swapchainCount;
};

struct XrSpace_T {
    uint32_t magic;
    uint64_t id;
    XrSession session;
    XrReferenceSpaceType type;
    XrPosef poseInReferenceSpace;
};

struct XrSwapchain_T {
    uint32_t magic;
    uint64_t id;
    XrSession session;
    XrSwapchainUsageFlags usageFlags;
    int64_t format;
    uint32_t width;
    uint32_t height;
    uint32_t faceCount;
    uint32_t arraySize;
    uint32_t mipCount;
    uint32_t sampleCount;
    void* textures[kSwapchainImageCount];
    uint32_t imageCount;
    uint32_t acquiredImageIndex;
    XrBool32 imageAcquired;
    XrBool32 imageWaited;
    uint64_t acquireCount;
    uint64_t releaseCount;
};

typedef void* MetalXrObjcId;
typedef void* MetalXrObjcClass;
typedef void* MetalXrSel;

typedef MetalXrObjcClass (*PFN_objc_getClass)(const char* name);
typedef MetalXrSel (*PFN_sel_registerName)(const char* name);

typedef MetalXrObjcId (*PFN_objc_msgSend_id)(MetalXrObjcId receiver, MetalXrSel selector);
typedef MetalXrObjcId (*PFN_objc_msgSend_id_id)(
    MetalXrObjcId receiver,
    MetalXrSel selector,
    MetalXrObjcId value);
typedef void (*PFN_objc_msgSend_void)(MetalXrObjcId receiver, MetalXrSel selector);
typedef void (*PFN_objc_msgSend_void_uint)(MetalXrObjcId receiver, MetalXrSel selector, uint64_t value);

typedef struct MetalXrObjcBridge {
    void* library;
    PFN_objc_getClass objc_getClass;
    PFN_sel_registerName sel_registerName;
    void* objc_msgSend;
    XrBool32 loaded;
    XrBool32 available;
} MetalXrObjcBridge;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static MetalXrEventQueue g_eventQueue;
static uint64_t g_nextInstanceId = 1;
static uint64_t g_nextSessionId = 1;
static uint64_t g_nextSpaceId = 1;
static uint64_t g_nextSwapchainId = 1;
static void* g_metalDevice;
static MetalXrObjcBridge g_objcBridge;

static void metalxr_release_objc(void* object);

static void metalxr_log(const char* format, ...)
{
    FILE* output = stderr;
    const char* logPath = getenv("METALXR_RUNTIME_LOG");
    if (logPath != NULL && logPath[0] != '\0') {
        FILE* logFile = fopen(logPath, "a");
        if (logFile != NULL) {
            output = logFile;
        }
    }

    fprintf(output, "[MetalXRRuntime] ");

    va_list args;
    va_start(args, format);
    vfprintf(output, format, args);
    va_end(args);

    fprintf(output, "\n");

    if (output != stderr) {
        fclose(output);
    }
}

static void metalxr_copy_string(char* destination, size_t destinationSize, const char* source)
{
    if (destination == NULL || destinationSize == 0) {
        return;
    }

    snprintf(destination, destinationSize, "%s", source != NULL ? source : "");
}

static XrTime metalxr_now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return ((XrTime)now.tv_sec * 1000000000) + (XrTime)now.tv_nsec;
}

static int api_versions_overlap(XrVersion minVersion, XrVersion maxVersion, XrVersion supportedVersion)
{
    const uint16_t minMajor = XR_VERSION_MAJOR(minVersion);
    const uint16_t minMinor = XR_VERSION_MINOR(minVersion);
    const uint16_t maxMajor = XR_VERSION_MAJOR(maxVersion);
    const uint16_t maxMinor = XR_VERSION_MINOR(maxVersion);
    const uint16_t supportedMajor = XR_VERSION_MAJOR(supportedVersion);
    const uint16_t supportedMinor = XR_VERSION_MINOR(supportedVersion);

    if (supportedMajor < minMajor || supportedMajor > maxMajor) {
        return 0;
    }

    if (supportedMajor == minMajor && supportedMinor < minMinor) {
        return 0;
    }

    if (supportedMajor == maxMajor && supportedMinor > maxMinor) {
        return 0;
    }

    return 1;
}

static int metalxr_is_supported_extension(const char* name)
{
    if (name == NULL) {
        return 0;
    }

    for (size_t i = 0; i < sizeof(kSupportedExtensions) / sizeof(kSupportedExtensions[0]); ++i) {
        if (strcmp(name, kSupportedExtensions[i].name) == 0) {
            return 1;
        }
    }

    return 0;
}

static int metalxr_is_instance(XrInstance instance)
{
    return instance != NULL && instance->magic == kInstanceMagic;
}

static int metalxr_is_session(XrSession session)
{
    return session != NULL && session->magic == kSessionMagic && metalxr_is_instance(session->instance);
}

static int metalxr_is_space(XrSpace space)
{
    return space != NULL && space->magic == kSpaceMagic && metalxr_is_session(space->session);
}

static int metalxr_is_swapchain(XrSwapchain swapchain)
{
    return swapchain != NULL &&
           swapchain->magic == kSwapchainMagic &&
           metalxr_is_session(swapchain->session);
}

static const char* metalxr_session_state_name(XrSessionState state)
{
    switch (state) {
        case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNRECOGNIZED";
    }
}

static const char* metalxr_result_name(XrResult result)
{
    switch (result) {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
        case XR_EVENT_UNAVAILABLE: return "XR_EVENT_UNAVAILABLE";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
        case XR_ERROR_SIZE_INSUFFICIENT: return "XR_ERROR_SIZE_INSUFFICIENT";
        case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
        case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_SESSION_RUNNING: return "XR_ERROR_SESSION_RUNNING";
        case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
        case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
        case XR_ERROR_SYSTEM_INVALID: return "XR_ERROR_SYSTEM_INVALID";
        case XR_ERROR_SWAPCHAIN_RECT_INVALID: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
        case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case XR_ERROR_SESSION_NOT_READY: return "XR_ERROR_SESSION_NOT_READY";
        case XR_ERROR_SESSION_NOT_STOPPING: return "XR_ERROR_SESSION_NOT_STOPPING";
        case XR_ERROR_REFERENCE_SPACE_UNSUPPORTED: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
        case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
        case XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED: return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
        case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
        default: return "XR_UNKNOWN_RESULT";
    }
}

static const char* metalxr_structure_type_name(XrStructureType type)
{
    switch (type) {
        case XR_TYPE_UNKNOWN: return "XR_TYPE_UNKNOWN";
        case XR_TYPE_API_LAYER_PROPERTIES: return "XR_TYPE_API_LAYER_PROPERTIES";
        case XR_TYPE_EXTENSION_PROPERTIES: return "XR_TYPE_EXTENSION_PROPERTIES";
        case XR_TYPE_INSTANCE_CREATE_INFO: return "XR_TYPE_INSTANCE_CREATE_INFO";
        case XR_TYPE_SYSTEM_GET_INFO: return "XR_TYPE_SYSTEM_GET_INFO";
        case XR_TYPE_SYSTEM_PROPERTIES: return "XR_TYPE_SYSTEM_PROPERTIES";
        case XR_TYPE_VIEW_LOCATE_INFO: return "XR_TYPE_VIEW_LOCATE_INFO";
        case XR_TYPE_VIEW: return "XR_TYPE_VIEW";
        case XR_TYPE_SESSION_CREATE_INFO: return "XR_TYPE_SESSION_CREATE_INFO";
        case XR_TYPE_SWAPCHAIN_CREATE_INFO: return "XR_TYPE_SWAPCHAIN_CREATE_INFO";
        case XR_TYPE_SESSION_BEGIN_INFO: return "XR_TYPE_SESSION_BEGIN_INFO";
        case XR_TYPE_VIEW_STATE: return "XR_TYPE_VIEW_STATE";
        case XR_TYPE_FRAME_END_INFO: return "XR_TYPE_FRAME_END_INFO";
        case XR_TYPE_EVENT_DATA_BUFFER: return "XR_TYPE_EVENT_DATA_BUFFER";
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: return "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED";
        case XR_TYPE_INSTANCE_PROPERTIES: return "XR_TYPE_INSTANCE_PROPERTIES";
        case XR_TYPE_FRAME_WAIT_INFO: return "XR_TYPE_FRAME_WAIT_INFO";
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION: return "XR_TYPE_COMPOSITION_LAYER_PROJECTION";
        case XR_TYPE_REFERENCE_SPACE_CREATE_INFO: return "XR_TYPE_REFERENCE_SPACE_CREATE_INFO";
        case XR_TYPE_VIEW_CONFIGURATION_VIEW: return "XR_TYPE_VIEW_CONFIGURATION_VIEW";
        case XR_TYPE_SPACE_LOCATION: return "XR_TYPE_SPACE_LOCATION";
        case XR_TYPE_FRAME_STATE: return "XR_TYPE_FRAME_STATE";
        case XR_TYPE_VIEW_CONFIGURATION_PROPERTIES: return "XR_TYPE_VIEW_CONFIGURATION_PROPERTIES";
        case XR_TYPE_FRAME_BEGIN_INFO: return "XR_TYPE_FRAME_BEGIN_INFO";
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW: return "XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW";
        case XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO: return "XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO";
        case XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO: return "XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO";
        case XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO: return "XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO";
        case XR_TYPE_GRAPHICS_BINDING_METAL_KHR: return "XR_TYPE_GRAPHICS_BINDING_METAL_KHR";
        case XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR: return "XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR";
        case XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR: return "XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR";
        default: return "XR_TYPE_UNRECOGNIZED";
    }
}

static XrPosef metalxr_identity_pose(void)
{
    XrPosef pose;
    memset(&pose, 0, sizeof(pose));
    pose.orientation.w = 1.0f;
    return pose;
}

static XrFovf metalxr_default_fov(void)
{
    XrFovf fov;
    fov.angleLeft = -0.7853982f;
    fov.angleRight = 0.7853982f;
    fov.angleUp = 0.7853982f;
    fov.angleDown = -0.7853982f;
    return fov;
}

static void metalxr_clear_events_for_session(XrSession session)
{
    pthread_mutex_lock(&g_mutex);

    XrEventDataSessionStateChanged kept[64];
    uint32_t keptCount = 0;
    for (uint32_t i = 0; i < g_eventQueue.count; ++i) {
        const uint32_t index = (g_eventQueue.head + i) % 64;
        if (g_eventQueue.events[index].session != session) {
            kept[keptCount++] = g_eventQueue.events[index];
        }
    }

    memset(&g_eventQueue, 0, sizeof(g_eventQueue));
    for (uint32_t i = 0; i < keptCount; ++i) {
        g_eventQueue.events[i] = kept[i];
    }
    g_eventQueue.count = keptCount;

    pthread_mutex_unlock(&g_mutex);
}

static void metalxr_release_swapchain(XrSwapchain swapchain)
{
    if (!metalxr_is_swapchain(swapchain)) {
        return;
    }

    XrSession session = swapchain->session;
    if (metalxr_is_session(session)) {
        for (uint32_t i = 0; i < session->swapchainCount; ++i) {
            if (session->swapchains[i] == swapchain) {
                for (uint32_t j = i; j + 1 < session->swapchainCount; ++j) {
                    session->swapchains[j] = session->swapchains[j + 1];
                }
                --session->swapchainCount;
                break;
            }
        }
    }

    for (uint32_t i = 0; i < swapchain->imageCount; ++i) {
        metalxr_release_objc(swapchain->textures[i]);
        swapchain->textures[i] = NULL;
    }

    swapchain->magic = 0;
    free(swapchain);
}

static void metalxr_release_session(XrSession session)
{
    if (!metalxr_is_session(session)) {
        return;
    }

    metalxr_clear_events_for_session(session);

    for (uint32_t i = 0; i < session->spaceCount; ++i) {
        if (session->spaces[i] != NULL && metalxr_is_space(session->spaces[i])) {
            session->spaces[i]->magic = 0;
            free(session->spaces[i]);
        }
    }

    while (session->swapchainCount > 0) {
        metalxr_release_swapchain(session->swapchains[session->swapchainCount - 1]);
    }

    metalxr_release_objc(session->metalDevice);

    if (metalxr_is_instance(session->instance) && session->instance->session == session) {
        session->instance->session = NULL;
    }

    session->magic = 0;
    free(session);
}

static void metalxr_queue_session_state(XrSession session, XrSessionState state)
{
    if (!metalxr_is_session(session)) {
        return;
    }

    session->state = state;
    metalxr_log("session %" PRIu64 " state -> %s", session->id, metalxr_session_state_name(state));

    XrEventDataSessionStateChanged event;
    memset(&event, 0, sizeof(event));
    event.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    event.session = session;
    event.state = state;
    event.time = metalxr_now_ns();

    pthread_mutex_lock(&g_mutex);

    if (g_eventQueue.count == 64) {
        metalxr_log("event queue full; dropping session state %s", metalxr_session_state_name(state));
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    const uint32_t index = (g_eventQueue.head + g_eventQueue.count) % 64;
    g_eventQueue.events[index] = event;
    ++g_eventQueue.count;

    pthread_mutex_unlock(&g_mutex);
}

static XrResult metalxr_fill_view_configuration_views(
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrViewConfigurationView* views)
{
    if (viewCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *viewCountOutput = kViewCount;

    if (viewCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (views == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (viewCapacityInput < kViewCount) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    for (uint32_t i = 0; i < kViewCount; ++i) {
        views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        views[i].recommendedImageRectWidth = kRecommendedEyeWidth;
        views[i].maxImageRectWidth = kRecommendedEyeWidth;
        views[i].recommendedImageRectHeight = kRecommendedEyeHeight;
        views[i].maxImageRectHeight = kRecommendedEyeHeight;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }

    return XR_SUCCESS;
}

static const int64_t* metalxr_supported_swapchain_formats(uint32_t* formatCount)
{
    static const int64_t formats[] = {
        kMetalPixelFormatBGRA8Unorm,
        kMetalPixelFormatBGRA8UnormSrgb,
        kMetalPixelFormatRGBA8Unorm,
        kMetalPixelFormatRGBA8UnormSrgb,
        kMetalPixelFormatRGB10A2Unorm,
        kMetalPixelFormatRGBA16Float,
    };

    if (formatCount != NULL) {
        *formatCount = (uint32_t)(sizeof(formats) / sizeof(formats[0]));
    }

    return formats;
}

static int metalxr_is_supported_swapchain_format(int64_t format)
{
    uint32_t formatCount = 0;
    const int64_t* formats = metalxr_supported_swapchain_formats(&formatCount);
    for (uint32_t i = 0; i < formatCount; ++i) {
        if (formats[i] == format) {
            return 1;
        }
    }

    return 0;
}

static uint64_t metalxr_metal_texture_usage(XrSwapchainUsageFlags usageFlags)
{
    uint64_t metalUsage = 0;

    if ((usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 ||
        (usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        metalUsage |= kMetalTextureUsageRenderTarget;
    }

    if ((usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0 ||
        (usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) != 0) {
        metalUsage |= kMetalTextureUsageShaderRead;
    }

    if ((usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0 ||
        (usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0) {
        metalUsage |= kMetalTextureUsageShaderWrite;
    }

    if ((usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) != 0) {
        metalUsage |= kMetalTextureUsagePixelFormatView;
    }

    return metalUsage != 0 ? metalUsage : kMetalTextureUsageRenderTarget;
}

static MetalXrObjcBridge* metalxr_objc_bridge(void)
{
    if (g_objcBridge.loaded) {
        return g_objcBridge.available ? &g_objcBridge : NULL;
    }

    g_objcBridge.loaded = XR_TRUE;
    g_objcBridge.library = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (g_objcBridge.library == NULL) {
        metalxr_log("libobjc dlopen failed: %s", dlerror());
        return NULL;
    }

    g_objcBridge.objc_getClass = (PFN_objc_getClass)dlsym(g_objcBridge.library, "objc_getClass");
    g_objcBridge.sel_registerName = (PFN_sel_registerName)dlsym(g_objcBridge.library, "sel_registerName");
    g_objcBridge.objc_msgSend = dlsym(g_objcBridge.library, "objc_msgSend");
    if (g_objcBridge.objc_getClass == NULL ||
        g_objcBridge.sel_registerName == NULL ||
        g_objcBridge.objc_msgSend == NULL) {
        metalxr_log("libobjc symbol lookup failed");
        return NULL;
    }

    g_objcBridge.available = XR_TRUE;
    return &g_objcBridge;
}

static MetalXrSel metalxr_sel(MetalXrObjcBridge* bridge, const char* name)
{
    return bridge->sel_registerName(name);
}

static MetalXrObjcId metalxr_objc_send_id(MetalXrObjcId receiver, MetalXrSel selector)
{
    MetalXrObjcBridge* bridge = metalxr_objc_bridge();
    if (bridge == NULL || receiver == NULL || selector == NULL) {
        return NULL;
    }

    PFN_objc_msgSend_id send = (PFN_objc_msgSend_id)bridge->objc_msgSend;
    return send(receiver, selector);
}

static MetalXrObjcId metalxr_objc_send_id_id(
    MetalXrObjcId receiver,
    MetalXrSel selector,
    MetalXrObjcId value)
{
    MetalXrObjcBridge* bridge = metalxr_objc_bridge();
    if (bridge == NULL || receiver == NULL || selector == NULL) {
        return NULL;
    }

    PFN_objc_msgSend_id_id send = (PFN_objc_msgSend_id_id)bridge->objc_msgSend;
    return send(receiver, selector, value);
}

static void metalxr_objc_send_void(MetalXrObjcId receiver, MetalXrSel selector)
{
    MetalXrObjcBridge* bridge = metalxr_objc_bridge();
    if (bridge == NULL || receiver == NULL || selector == NULL) {
        return;
    }

    PFN_objc_msgSend_void send = (PFN_objc_msgSend_void)bridge->objc_msgSend;
    send(receiver, selector);
}

static void metalxr_objc_send_void_uint(MetalXrObjcId receiver, MetalXrSel selector, uint64_t value)
{
    MetalXrObjcBridge* bridge = metalxr_objc_bridge();
    if (bridge == NULL || receiver == NULL || selector == NULL) {
        return;
    }

    PFN_objc_msgSend_void_uint send = (PFN_objc_msgSend_void_uint)bridge->objc_msgSend;
    send(receiver, selector, value);
}

static void* metalxr_retain_objc(void* object)
{
    MetalXrObjcBridge* bridge = metalxr_objc_bridge();
    if (bridge == NULL || object == NULL) {
        return object;
    }

    return metalxr_objc_send_id((MetalXrObjcId)object, metalxr_sel(bridge, "retain"));
}

static void metalxr_release_objc(void* object)
{
    MetalXrObjcBridge* bridge = metalxr_objc_bridge();
    if (bridge == NULL || object == NULL) {
        return;
    }

    metalxr_objc_send_void((MetalXrObjcId)object, metalxr_sel(bridge, "release"));
}

static void* metalxr_get_command_queue_device(void* commandQueue)
{
    MetalXrObjcBridge* bridge = metalxr_objc_bridge();
    if (bridge == NULL || commandQueue == NULL) {
        return NULL;
    }

    return metalxr_objc_send_id((MetalXrObjcId)commandQueue, metalxr_sel(bridge, "device"));
}

static void* metalxr_create_metal_texture(
    void* metalDevice,
    int64_t format,
    uint32_t width,
    uint32_t height,
    uint32_t arraySize,
    uint32_t mipCount,
    XrSwapchainUsageFlags usageFlags)
{
    MetalXrObjcBridge* bridge = metalxr_objc_bridge();
    if (bridge == NULL || metalDevice == NULL) {
        return NULL;
    }

    MetalXrObjcClass descriptorClass = bridge->objc_getClass("MTLTextureDescriptor");
    if (descriptorClass == NULL) {
        metalxr_log("MTLTextureDescriptor class unavailable");
        return NULL;
    }

    MetalXrObjcId descriptor =
        metalxr_objc_send_id((MetalXrObjcId)descriptorClass, metalxr_sel(bridge, "alloc"));
    descriptor = metalxr_objc_send_id(descriptor, metalxr_sel(bridge, "init"));
    if (descriptor == NULL) {
        return NULL;
    }

    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setTextureType:"),
                                arraySize > 1 ? kMetalTextureType2DArray : kMetalTextureType2D);
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setPixelFormat:"), (uint64_t)format);
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setWidth:"), width);
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setHeight:"), height);
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setDepth:"), 1);
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setMipmapLevelCount:"), mipCount);
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setSampleCount:"), 1);
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setArrayLength:"), arraySize);
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setUsage:"),
                                metalxr_metal_texture_usage(usageFlags));
    metalxr_objc_send_void_uint(descriptor, metalxr_sel(bridge, "setStorageMode:"),
                                kMetalStorageModePrivate);

    MetalXrObjcId texture = metalxr_objc_send_id_id(
        (MetalXrObjcId)metalDevice,
        metalxr_sel(bridge, "newTextureWithDescriptor:"),
        descriptor);
    metalxr_release_objc(descriptor);

    return texture;
}

static void* metalxr_get_metal_device(void)
{
    if (g_metalDevice != NULL) {
        return g_metalDevice;
    }

    void* metalLibrary = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY | RTLD_LOCAL);
    if (metalLibrary == NULL) {
        metalxr_log("Metal.framework dlopen failed: %s", dlerror());
        return NULL;
    }

    typedef void* (*PFN_MTLCreateSystemDefaultDevice)(void);
    PFN_MTLCreateSystemDefaultDevice createDevice =
        (PFN_MTLCreateSystemDefaultDevice)dlsym(metalLibrary, "MTLCreateSystemDefaultDevice");
    if (createDevice == NULL) {
        metalxr_log("MTLCreateSystemDefaultDevice lookup failed: %s", dlerror());
        return NULL;
    }

    g_metalDevice = createDevice();
    metalxr_log("Metal device %s", g_metalDevice != NULL ? "created" : "unavailable");
    return g_metalDevice;
}

static XrResult XRAPI_CALL metalxr_xrEnumerateApiLayerProperties(
    uint32_t propertyCapacityInput,
    uint32_t* propertyCountOutput,
    XrApiLayerProperties* properties)
{
    metalxr_log("xrEnumerateApiLayerProperties");

    if (propertyCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *propertyCountOutput = 0;

    if (propertyCapacityInput > 0 && properties == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrEnumerateInstanceExtensionProperties(
    const char* layerName,
    uint32_t propertyCapacityInput,
    uint32_t* propertyCountOutput,
    XrExtensionProperties* properties)
{
    metalxr_log("xrEnumerateInstanceExtensionProperties layer=%s",
                layerName != NULL ? layerName : "<runtime>");

    if (propertyCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (layerName != NULL && layerName[0] != '\0') {
        *propertyCountOutput = 0;
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    }

    const uint32_t extensionCount = (uint32_t)(sizeof(kSupportedExtensions) / sizeof(kSupportedExtensions[0]));
    *propertyCountOutput = extensionCount;

    if (propertyCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (properties == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (propertyCapacityInput < extensionCount) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    for (uint32_t i = 0; i < extensionCount; ++i) {
        properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        properties[i].extensionVersion = kSupportedExtensions[i].version;
        metalxr_copy_string(properties[i].extensionName,
                            sizeof(properties[i].extensionName),
                            kSupportedExtensions[i].name);
    }

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrCreateInstance(
    const XrInstanceCreateInfo* createInfo,
    XrInstance* instance)
{
    metalxr_log("xrCreateInstance");

    if (instance == NULL || createInfo == NULL || createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *instance = XR_NULL_HANDLE;

    if (XR_VERSION_MAJOR(createInfo->applicationInfo.apiVersion) > XR_VERSION_MAJOR(kSupportedApiVersion)) {
        metalxr_log("xrCreateInstance failed: requested API version %u.%u",
                    XR_VERSION_MAJOR(createInfo->applicationInfo.apiVersion),
                    XR_VERSION_MINOR(createInfo->applicationInfo.apiVersion));
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }

    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        const char* extensionName = createInfo->enabledExtensionNames != NULL ?
            createInfo->enabledExtensionNames[i] : NULL;
        if (!metalxr_is_supported_extension(extensionName)) {
            metalxr_log("xrCreateInstance failed: unsupported extension %s",
                        extensionName != NULL ? extensionName : "<null>");
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    XrInstance created = (XrInstance)calloc(1, sizeof(*created));
    if (created == NULL) {
        return XR_ERROR_OUT_OF_MEMORY;
    }

    pthread_mutex_lock(&g_mutex);
    created->id = g_nextInstanceId++;
    pthread_mutex_unlock(&g_mutex);

    created->magic = kInstanceMagic;
    *instance = created;

    metalxr_log("instance %" PRIu64 " created for app='%s' engine='%s' extensions=%u",
                created->id,
                createInfo->applicationInfo.applicationName,
                createInfo->applicationInfo.engineName,
                createInfo->enabledExtensionCount);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrDestroyInstance(XrInstance instance)
{
    metalxr_log("xrDestroyInstance");

    if (!metalxr_is_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (instance->session != NULL && metalxr_is_session(instance->session)) {
        metalxr_log("destroying live session during instance teardown");
        metalxr_release_session(instance->session);
    }

    instance->magic = 0;
    free(instance);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrGetInstanceProperties(
    XrInstance instance,
    XrInstanceProperties* instanceProperties)
{
    metalxr_log("xrGetInstanceProperties");

    if (!metalxr_is_instance(instance) || instanceProperties == NULL) {
        return XR_ERROR_HANDLE_INVALID;
    }

    instanceProperties->type = XR_TYPE_INSTANCE_PROPERTIES;
    instanceProperties->runtimeVersion = kRuntimeVersion;
    metalxr_copy_string(instanceProperties->runtimeName,
                        sizeof(instanceProperties->runtimeName),
                        "MetalXR Runtime");

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData)
{
    if (!metalxr_is_instance(instance) || eventData == NULL) {
        return XR_ERROR_HANDLE_INVALID;
    }

    pthread_mutex_lock(&g_mutex);

    if (g_eventQueue.count == 0) {
        pthread_mutex_unlock(&g_mutex);
        memset(eventData, 0, sizeof(*eventData));
        eventData->type = XR_TYPE_EVENT_DATA_BUFFER;
        return XR_EVENT_UNAVAILABLE;
    }

    const XrEventDataSessionStateChanged event = g_eventQueue.events[g_eventQueue.head];
    g_eventQueue.head = (g_eventQueue.head + 1) % 64;
    --g_eventQueue.count;

    pthread_mutex_unlock(&g_mutex);

    memset(eventData, 0, sizeof(*eventData));
    memcpy(eventData, &event, sizeof(event));

    metalxr_log("xrPollEvent -> session state %s", metalxr_session_state_name(event.state));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrResultToString(
    XrInstance instance,
    XrResult value,
    char buffer[XR_MAX_RESULT_STRING_SIZE])
{
    (void)instance;

    if (buffer == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    metalxr_copy_string(buffer, XR_MAX_RESULT_STRING_SIZE, metalxr_result_name(value));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrStructureTypeToString(
    XrInstance instance,
    XrStructureType value,
    char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
    (void)instance;

    if (buffer == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    metalxr_copy_string(buffer, XR_MAX_STRUCTURE_NAME_SIZE, metalxr_structure_type_name(value));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrGetSystem(
    XrInstance instance,
    const XrSystemGetInfo* getInfo,
    XrSystemId* systemId)
{
    metalxr_log("xrGetSystem");

    if (!metalxr_is_instance(instance) || getInfo == NULL || systemId == NULL ||
        getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        *systemId = XR_NULL_SYSTEM_ID;
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }

    *systemId = kDummySystemId;
    metalxr_log("xrGetSystem -> dummy stereo HMD system=%" PRIu64, (uint64_t)*systemId);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrGetSystemProperties(
    XrInstance instance,
    XrSystemId systemId,
    XrSystemProperties* properties)
{
    metalxr_log("xrGetSystemProperties");

    if (!metalxr_is_instance(instance) || properties == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (systemId != kDummySystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }

    properties->type = XR_TYPE_SYSTEM_PROPERTIES;
    properties->systemId = kDummySystemId;
    properties->vendorId = 0x4d5852;
    metalxr_copy_string(properties->systemName, sizeof(properties->systemName), "MetalXR Dummy Stereo HMD");
    properties->graphicsProperties.maxSwapchainImageHeight = kRecommendedEyeHeight;
    properties->graphicsProperties.maxSwapchainImageWidth = kRecommendedEyeWidth;
    properties->graphicsProperties.maxLayerCount = 16;
    properties->trackingProperties.orientationTracking = XR_TRUE;
    properties->trackingProperties.positionTracking = XR_TRUE;

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrEnumerateViewConfigurations(
    XrInstance instance,
    XrSystemId systemId,
    uint32_t viewConfigurationTypeCapacityInput,
    uint32_t* viewConfigurationTypeCountOutput,
    XrViewConfigurationType* viewConfigurationTypes)
{
    metalxr_log("xrEnumerateViewConfigurations");

    if (!metalxr_is_instance(instance) || viewConfigurationTypeCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (systemId != kDummySystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }

    *viewConfigurationTypeCountOutput = 1;

    if (viewConfigurationTypeCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (viewConfigurationTypes == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    viewConfigurationTypes[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrGetViewConfigurationProperties(
    XrInstance instance,
    XrSystemId systemId,
    XrViewConfigurationType viewConfigurationType,
    XrViewConfigurationProperties* configurationProperties)
{
    metalxr_log("xrGetViewConfigurationProperties");

    if (!metalxr_is_instance(instance) || configurationProperties == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (systemId != kDummySystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }

    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }

    configurationProperties->type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
    configurationProperties->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    configurationProperties->fovMutable = XR_FALSE;

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrEnumerateViewConfigurationViews(
    XrInstance instance,
    XrSystemId systemId,
    XrViewConfigurationType viewConfigurationType,
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrViewConfigurationView* views)
{
    metalxr_log("xrEnumerateViewConfigurationViews");

    if (!metalxr_is_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (systemId != kDummySystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }

    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }

    return metalxr_fill_view_configuration_views(viewCapacityInput, viewCountOutput, views);
}

static XrResult XRAPI_CALL metalxr_xrEnumerateEnvironmentBlendModes(
    XrInstance instance,
    XrSystemId systemId,
    XrViewConfigurationType viewConfigurationType,
    uint32_t environmentBlendModeCapacityInput,
    uint32_t* environmentBlendModeCountOutput,
    XrEnvironmentBlendMode* environmentBlendModes)
{
    metalxr_log("xrEnumerateEnvironmentBlendModes");

    if (!metalxr_is_instance(instance) || environmentBlendModeCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (systemId != kDummySystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }

    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }

    *environmentBlendModeCountOutput = 1;

    if (environmentBlendModeCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (environmentBlendModes == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    environmentBlendModes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrGetMetalGraphicsRequirementsKHR(
    XrInstance instance,
    XrSystemId systemId,
    XrGraphicsRequirementsMetalKHR* graphicsRequirements)
{
    metalxr_log("xrGetMetalGraphicsRequirementsKHR");

    if (!metalxr_is_instance(instance) || graphicsRequirements == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (systemId != kDummySystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }

    graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
    graphicsRequirements->metalDevice = metalxr_get_metal_device();

    if (graphicsRequirements->metalDevice == NULL) {
        return XR_ERROR_RUNTIME_FAILURE;
    }

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrCreateSession(
    XrInstance instance,
    const XrSessionCreateInfo* createInfo,
    XrSession* session)
{
    metalxr_log("xrCreateSession");

    if (!metalxr_is_instance(instance) || createInfo == NULL || session == NULL ||
        createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *session = XR_NULL_HANDLE;

    if (createInfo->systemId != kDummySystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }

    if (instance->session != NULL && metalxr_is_session(instance->session)) {
        return XR_ERROR_LIMIT_REACHED;
    }

    const XrGraphicsBindingMetalKHR* metalBinding =
        (const XrGraphicsBindingMetalKHR*)createInfo->next;
    void* commandQueue = NULL;
    void* metalDevice = NULL;
    if (metalBinding != NULL && metalBinding->type == XR_TYPE_GRAPHICS_BINDING_METAL_KHR) {
        metalxr_log("xrCreateSession received Metal commandQueue=%p", metalBinding->commandQueue);
        commandQueue = metalBinding->commandQueue;
        metalDevice = metalxr_get_command_queue_device(commandQueue);
    } else if (createInfo->next != NULL) {
        const XrStructureType* nextType = (const XrStructureType*)createInfo->next;
        metalxr_log("xrCreateSession ignoring unsupported next struct %s",
                    metalxr_structure_type_name(*nextType));
    }

    if (metalDevice == NULL) {
        metalDevice = metalxr_get_metal_device();
    }

    if (metalDevice == NULL) {
        metalxr_log("xrCreateSession failed: no Metal device available");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    XrSession created = (XrSession)calloc(1, sizeof(*created));
    if (created == NULL) {
        return XR_ERROR_OUT_OF_MEMORY;
    }

    pthread_mutex_lock(&g_mutex);
    created->id = g_nextSessionId++;
    pthread_mutex_unlock(&g_mutex);

    created->magic = kSessionMagic;
    created->instance = instance;
    created->systemId = kDummySystemId;
    created->state = XR_SESSION_STATE_IDLE;
    created->metalCommandQueue = commandQueue;
    created->metalDevice = metalxr_retain_objc(metalDevice);
    created->nextFrameTime = metalxr_now_ns() + kFramePeriodNs;
    instance->session = created;
    *session = created;

    metalxr_log("session %" PRIu64 " created", created->id);
    metalxr_queue_session_state(created, XR_SESSION_STATE_READY);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrDestroySession(XrSession session)
{
    metalxr_log("xrDestroySession");

    if (!metalxr_is_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    metalxr_release_session(session);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrBeginSession(
    XrSession session,
    const XrSessionBeginInfo* beginInfo)
{
    metalxr_log("xrBeginSession");

    if (!metalxr_is_session(session) || beginInfo == NULL ||
        beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (beginInfo->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }

    if (session->running) {
        return XR_ERROR_SESSION_RUNNING;
    }

    if (session->state != XR_SESSION_STATE_READY &&
        session->state != XR_SESSION_STATE_SYNCHRONIZED &&
        session->state != XR_SESSION_STATE_VISIBLE &&
        session->state != XR_SESSION_STATE_FOCUSED) {
        return XR_ERROR_SESSION_NOT_READY;
    }

    session->running = XR_TRUE;
    session->nextFrameTime = metalxr_now_ns() + kFramePeriodNs;
    metalxr_queue_session_state(session, XR_SESSION_STATE_SYNCHRONIZED);
    metalxr_queue_session_state(session, XR_SESSION_STATE_VISIBLE);
    metalxr_queue_session_state(session, XR_SESSION_STATE_FOCUSED);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrEndSession(XrSession session)
{
    metalxr_log("xrEndSession");

    if (!metalxr_is_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (!session->running && session->state != XR_SESSION_STATE_STOPPING) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    session->running = XR_FALSE;
    session->frameBegun = XR_FALSE;
    metalxr_queue_session_state(session, XR_SESSION_STATE_IDLE);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrRequestExitSession(XrSession session)
{
    metalxr_log("xrRequestExitSession");

    if (!metalxr_is_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    metalxr_queue_session_state(session, XR_SESSION_STATE_STOPPING);
    metalxr_queue_session_state(session, XR_SESSION_STATE_EXITING);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrEnumerateReferenceSpaces(
    XrSession session,
    uint32_t spaceCapacityInput,
    uint32_t* spaceCountOutput,
    XrReferenceSpaceType* spaces)
{
    metalxr_log("xrEnumerateReferenceSpaces");

    if (!metalxr_is_session(session) || spaceCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *spaceCountOutput = 3;

    if (spaceCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (spaces == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (spaceCapacityInput < 3) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaces[2] = XR_REFERENCE_SPACE_TYPE_STAGE;

    return XR_SUCCESS;
}

static int metalxr_reference_space_supported(XrReferenceSpaceType type)
{
    return type == XR_REFERENCE_SPACE_TYPE_VIEW ||
           type == XR_REFERENCE_SPACE_TYPE_LOCAL ||
           type == XR_REFERENCE_SPACE_TYPE_STAGE;
}

static XrResult XRAPI_CALL metalxr_xrCreateReferenceSpace(
    XrSession session,
    const XrReferenceSpaceCreateInfo* createInfo,
    XrSpace* space)
{
    metalxr_log("xrCreateReferenceSpace");

    if (!metalxr_is_session(session) || createInfo == NULL || space == NULL ||
        createInfo->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *space = XR_NULL_HANDLE;

    if (!metalxr_reference_space_supported(createInfo->referenceSpaceType)) {
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }

    if (session->spaceCount >= 16) {
        return XR_ERROR_LIMIT_REACHED;
    }

    XrSpace created = (XrSpace)calloc(1, sizeof(*created));
    if (created == NULL) {
        return XR_ERROR_OUT_OF_MEMORY;
    }

    pthread_mutex_lock(&g_mutex);
    created->id = g_nextSpaceId++;
    pthread_mutex_unlock(&g_mutex);

    created->magic = kSpaceMagic;
    created->session = session;
    created->type = createInfo->referenceSpaceType;
    created->poseInReferenceSpace = createInfo->poseInReferenceSpace;
    session->spaces[session->spaceCount++] = created;
    *space = created;

    metalxr_log("space %" PRIu64 " created type=%d", created->id, created->type);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrGetReferenceSpaceBoundsRect(
    XrSession session,
    XrReferenceSpaceType referenceSpaceType,
    XrExtent2Df* bounds)
{
    metalxr_log("xrGetReferenceSpaceBoundsRect");

    if (!metalxr_is_session(session) || bounds == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (!metalxr_reference_space_supported(referenceSpaceType)) {
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }

    bounds->width = referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? 3.0f : 0.0f;
    bounds->height = referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? 3.0f : 0.0f;

    return referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? XR_SUCCESS : XR_SPACE_BOUNDS_UNAVAILABLE;
}

static XrResult XRAPI_CALL metalxr_xrLocateSpace(
    XrSpace space,
    XrSpace baseSpace,
    XrTime time,
    XrSpaceLocation* location)
{
    (void)time;

    if (!metalxr_is_space(space) || !metalxr_is_space(baseSpace) || location == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    location->type = XR_TYPE_SPACE_LOCATION;
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                              XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                              XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    location->pose = metalxr_identity_pose();

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrDestroySpace(XrSpace space)
{
    metalxr_log("xrDestroySpace");

    if (!metalxr_is_space(space)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    XrSession session = space->session;
    for (uint32_t i = 0; i < session->spaceCount; ++i) {
        if (session->spaces[i] == space) {
            for (uint32_t j = i; j + 1 < session->spaceCount; ++j) {
                session->spaces[j] = session->spaces[j + 1];
            }
            --session->spaceCount;
            break;
        }
    }

    space->magic = 0;
    free(space);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrEnumerateSwapchainFormats(
    XrSession session,
    uint32_t formatCapacityInput,
    uint32_t* formatCountOutput,
    int64_t* formats)
{
    metalxr_log("xrEnumerateSwapchainFormats");

    if (!metalxr_is_session(session) || formatCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    uint32_t formatCount = 0;
    const int64_t* supportedFormats = metalxr_supported_swapchain_formats(&formatCount);
    *formatCountOutput = formatCount;

    if (formatCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (formats == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (formatCapacityInput < formatCount) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    for (uint32_t i = 0; i < formatCount; ++i) {
        formats[i] = supportedFormats[i];
    }

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrCreateSwapchain(
    XrSession session,
    const XrSwapchainCreateInfo* createInfo,
    XrSwapchain* swapchain)
{
    metalxr_log("xrCreateSwapchain");

    if (!metalxr_is_session(session) || createInfo == NULL || swapchain == NULL ||
        createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *swapchain = XR_NULL_HANDLE;

    if (session->swapchainCount >= kMaxSessionSwapchains) {
        return XR_ERROR_LIMIT_REACHED;
    }

    if (createInfo->width == 0 || createInfo->height == 0 ||
        createInfo->width > kRecommendedEyeWidth || createInfo->height > kRecommendedEyeHeight) {
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    }

    if (createInfo->faceCount != 1 || createInfo->arraySize == 0 ||
        createInfo->mipCount == 0 || createInfo->sampleCount != 1) {
        metalxr_log("xrCreateSwapchain unsupported dimensions: face=%u array=%u mip=%u samples=%u",
                    createInfo->faceCount,
                    createInfo->arraySize,
                    createInfo->mipCount,
                    createInfo->sampleCount);
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    if (!metalxr_is_supported_swapchain_format(createInfo->format)) {
        metalxr_log("xrCreateSwapchain unsupported Metal format=%" PRId64, createInfo->format);
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }

    XrSwapchain created = (XrSwapchain)calloc(1, sizeof(*created));
    if (created == NULL) {
        return XR_ERROR_OUT_OF_MEMORY;
    }

    pthread_mutex_lock(&g_mutex);
    created->id = g_nextSwapchainId++;
    pthread_mutex_unlock(&g_mutex);

    created->magic = kSwapchainMagic;
    created->session = session;
    created->usageFlags = createInfo->usageFlags;
    created->format = createInfo->format;
    created->width = createInfo->width;
    created->height = createInfo->height;
    created->faceCount = createInfo->faceCount;
    created->arraySize = createInfo->arraySize;
    created->mipCount = createInfo->mipCount;
    created->sampleCount = createInfo->sampleCount;
    created->imageCount = kSwapchainImageCount;

    for (uint32_t i = 0; i < created->imageCount; ++i) {
        created->textures[i] = metalxr_create_metal_texture(
            session->metalDevice,
            createInfo->format,
            createInfo->width,
            createInfo->height,
            createInfo->arraySize,
            createInfo->mipCount,
            createInfo->usageFlags);
        if (created->textures[i] == NULL) {
            metalxr_log("xrCreateSwapchain failed: Metal texture allocation failed");
            metalxr_release_swapchain(created);
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }

    session->swapchains[session->swapchainCount++] = created;
    *swapchain = created;

    metalxr_log("swapchain %" PRIu64 " created format=%" PRId64
                " %ux%u array=%u images=%u usage=0x%llx",
                created->id,
                created->format,
                created->width,
                created->height,
                created->arraySize,
                created->imageCount,
                (unsigned long long)created->usageFlags);

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrDestroySwapchain(XrSwapchain swapchain)
{
    metalxr_log("xrDestroySwapchain");

    if (!metalxr_is_swapchain(swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    metalxr_log("swapchain %" PRIu64 " destroyed acquired=%" PRIu64 " released=%" PRIu64,
                swapchain->id,
                swapchain->acquireCount,
                swapchain->releaseCount);
    metalxr_release_swapchain(swapchain);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrEnumerateSwapchainImages(
    XrSwapchain swapchain,
    uint32_t imageCapacityInput,
    uint32_t* imageCountOutput,
    XrSwapchainImageBaseHeader* images)
{
    if (!metalxr_is_swapchain(swapchain) || imageCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *imageCountOutput = swapchain->imageCount;

    if (imageCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (images == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (imageCapacityInput < swapchain->imageCount) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    XrSwapchainImageMetalKHR* metalImages = (XrSwapchainImageMetalKHR*)images;
    for (uint32_t i = 0; i < swapchain->imageCount; ++i) {
        metalImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
        metalImages[i].texture = swapchain->textures[i];
    }

    metalxr_log("xrEnumerateSwapchainImages swapchain=%" PRIu64 " images=%u",
                swapchain->id,
                swapchain->imageCount);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrAcquireSwapchainImage(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquireInfo,
    uint32_t* index)
{
    (void)acquireInfo;

    if (!metalxr_is_swapchain(swapchain) || index == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (swapchain->imageAcquired) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    swapchain->acquiredImageIndex = (uint32_t)(swapchain->acquireCount % swapchain->imageCount);
    swapchain->imageAcquired = XR_TRUE;
    swapchain->imageWaited = XR_FALSE;
    ++swapchain->acquireCount;
    *index = swapchain->acquiredImageIndex;

    if (swapchain->acquireCount <= 5 || (swapchain->acquireCount % 300) == 0) {
        metalxr_log("xrAcquireSwapchainImage swapchain=%" PRIu64 " image=%u",
                    swapchain->id,
                    swapchain->acquiredImageIndex);
    }

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrWaitSwapchainImage(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* waitInfo)
{
    (void)waitInfo;

    if (!metalxr_is_swapchain(swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (!swapchain->imageAcquired) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    swapchain->imageWaited = XR_TRUE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrReleaseSwapchainImage(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* releaseInfo)
{
    (void)releaseInfo;

    if (!metalxr_is_swapchain(swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (!swapchain->imageAcquired) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    ++swapchain->releaseCount;

    if (swapchain->releaseCount <= 5 || (swapchain->releaseCount % 300) == 0) {
        metalxr_log("xrReleaseSwapchainImage swapchain=%" PRIu64 " image=%u",
                    swapchain->id,
                    swapchain->acquiredImageIndex);
    }

    swapchain->imageAcquired = XR_FALSE;
    swapchain->imageWaited = XR_FALSE;

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrWaitFrame(
    XrSession session,
    const XrFrameWaitInfo* frameWaitInfo,
    XrFrameState* frameState)
{
    (void)frameWaitInfo;

    if (!metalxr_is_session(session) || frameState == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (!session->running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    const XrTime now = metalxr_now_ns();
    if (session->nextFrameTime <= now) {
        session->nextFrameTime = now + kFramePeriodNs;
    }

    frameState->type = XR_TYPE_FRAME_STATE;
    frameState->predictedDisplayTime = session->nextFrameTime;
    frameState->predictedDisplayPeriod = kFramePeriodNs;
    frameState->shouldRender = XR_TRUE;

    session->nextFrameTime += kFramePeriodNs;
    ++session->frameIndex;

    if (session->frameIndex <= 5 || (session->frameIndex % 300) == 0) {
        metalxr_log("xrWaitFrame frame=%" PRIu64 " predicted=%" PRId64,
                    session->frameIndex,
                    frameState->predictedDisplayTime);
    }

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrBeginFrame(
    XrSession session,
    const XrFrameBeginInfo* frameBeginInfo)
{
    (void)frameBeginInfo;

    if (!metalxr_is_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (!session->running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    session->frameBegun = XR_TRUE;
    return XR_SUCCESS;
}

static void metalxr_write_frame_metadata(
    XrSession session,
    const XrFrameEndInfo* frameEndInfo,
    const XrCompositionLayerProjection* projectionLayer)
{
    const char* dumpDirectory = getenv("METALXR_FRAME_DUMP_DIR");
    if (dumpDirectory == NULL || dumpDirectory[0] == '\0') {
        return;
    }

    char path[1024];
    snprintf(path,
             sizeof(path),
             "%s/frame_%06" PRIu64 ".txt",
             dumpDirectory,
             session->frameIndex);

    FILE* output = fopen(path, "w");
    if (output == NULL) {
        metalxr_log("frame metadata dump failed: %s", path);
        return;
    }

    fprintf(output, "frame=%" PRIu64 "\n", session->frameIndex);
    fprintf(output, "displayTime=%" PRId64 "\n", frameEndInfo->displayTime);
    fprintf(output, "layerCount=%u\n", frameEndInfo->layerCount);
    fprintf(output, "projectionViewCount=%u\n",
            projectionLayer != NULL ? projectionLayer->viewCount : 0);

    if (projectionLayer != NULL && projectionLayer->views != NULL) {
        for (uint32_t viewIndex = 0; viewIndex < projectionLayer->viewCount; ++viewIndex) {
            const XrCompositionLayerProjectionView* view = &projectionLayer->views[viewIndex];
            XrSwapchain swapchain = view->subImage.swapchain;
            if (!metalxr_is_swapchain(swapchain)) {
                fprintf(output, "view[%u].swapchain=invalid\n", viewIndex);
                continue;
            }

            const uint32_t imageIndex = swapchain->acquiredImageIndex;
            void* texture = imageIndex < swapchain->imageCount ? swapchain->textures[imageIndex] : NULL;
            fprintf(output, "view[%u].swapchain=%" PRIu64 "\n", viewIndex, swapchain->id);
            fprintf(output, "view[%u].imageIndex=%u\n", viewIndex, imageIndex);
            fprintf(output, "view[%u].texture=%p\n", viewIndex, texture);
            fprintf(output, "view[%u].format=%" PRId64 "\n", viewIndex, swapchain->format);
            fprintf(output, "view[%u].size=%ux%u\n", viewIndex, swapchain->width, swapchain->height);
            fprintf(output, "view[%u].arrayIndex=%u\n", viewIndex, view->subImage.imageArrayIndex);
            fprintf(output,
                    "view[%u].rect=%d,%d %dx%d\n",
                    viewIndex,
                    view->subImage.imageRect.offset.x,
                    view->subImage.imageRect.offset.y,
                    view->subImage.imageRect.extent.width,
                    view->subImage.imageRect.extent.height);
        }
    }

    fclose(output);
    metalxr_log("frame metadata dumped: %s", path);
}

static void metalxr_capture_frame_metadata(XrSession session, const XrFrameEndInfo* frameEndInfo)
{
    const XrCompositionLayerProjection* firstProjectionLayer = NULL;
    const int shouldSampleFrame = session->frameIndex <= 5 || (session->frameIndex % 300) == 0;

    if (frameEndInfo->layerCount == 0 || frameEndInfo->layers == NULL) {
        return;
    }

    for (uint32_t layerIndex = 0; layerIndex < frameEndInfo->layerCount; ++layerIndex) {
        const XrCompositionLayerBaseHeader* layer = frameEndInfo->layers[layerIndex];
        if (layer == NULL) {
            continue;
        }

        if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            if (shouldSampleFrame) {
                metalxr_log("xrEndFrame layer[%u] type=%s ignored",
                            layerIndex,
                            metalxr_structure_type_name(layer->type));
            }
            continue;
        }

        const XrCompositionLayerProjection* projectionLayer =
            (const XrCompositionLayerProjection*)layer;
        if (firstProjectionLayer == NULL) {
            firstProjectionLayer = projectionLayer;
        }

        if (shouldSampleFrame) {
            metalxr_log("xrEndFrame projection layer[%u] views=%u space=%p",
                        layerIndex,
                        projectionLayer->viewCount,
                        (void*)projectionLayer->space);
        }

        if (projectionLayer->views == NULL) {
            continue;
        }

        for (uint32_t viewIndex = 0; viewIndex < projectionLayer->viewCount; ++viewIndex) {
            const XrCompositionLayerProjectionView* view = &projectionLayer->views[viewIndex];
            XrSwapchain swapchain = view->subImage.swapchain;
            if (!metalxr_is_swapchain(swapchain)) {
                if (shouldSampleFrame) {
                    metalxr_log("projection view[%u] has invalid swapchain", viewIndex);
                }
                continue;
            }

            const uint32_t imageIndex = swapchain->acquiredImageIndex;
            void* texture = imageIndex < swapchain->imageCount ? swapchain->textures[imageIndex] : NULL;
            if (shouldSampleFrame) {
                metalxr_log("projection view[%u] swapchain=%" PRIu64 " image=%u texture=%p rect=%dx%d",
                            viewIndex,
                            swapchain->id,
                            imageIndex,
                            texture,
                            view->subImage.imageRect.extent.width,
                            view->subImage.imageRect.extent.height);
            }
        }
    }

    if (shouldSampleFrame) {
        metalxr_write_frame_metadata(session, frameEndInfo, firstProjectionLayer);
    }
}

static XrResult XRAPI_CALL metalxr_xrEndFrame(
    XrSession session,
    const XrFrameEndInfo* frameEndInfo)
{
    if (!metalxr_is_session(session) || frameEndInfo == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (!session->running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    if (frameEndInfo->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
        return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    }

    session->frameBegun = XR_FALSE;
    metalxr_capture_frame_metadata(session, frameEndInfo);

    if (session->frameIndex <= 5 || (session->frameIndex % 300) == 0) {
        metalxr_log("xrEndFrame frame=%" PRIu64 " layers=%u display=%" PRId64,
                    session->frameIndex,
                    frameEndInfo->layerCount,
                    frameEndInfo->displayTime);
    }

    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrLocateViews(
    XrSession session,
    const XrViewLocateInfo* viewLocateInfo,
    XrViewState* viewState,
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrView* views)
{
    if (!metalxr_is_session(session) || viewLocateInfo == NULL || viewState == NULL ||
        viewCountOutput == NULL || viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (!session->running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    if (viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }

    if (!metalxr_is_space(viewLocateInfo->space)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    *viewCountOutput = kViewCount;
    viewState->type = XR_TYPE_VIEW_STATE;
    viewState->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                                XR_VIEW_STATE_POSITION_VALID_BIT |
                                XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                                XR_VIEW_STATE_POSITION_TRACKED_BIT;

    if (viewCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (views == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (viewCapacityInput < kViewCount) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    for (uint32_t i = 0; i < kViewCount; ++i) {
        views[i].type = XR_TYPE_VIEW;
        views[i].pose = metalxr_identity_pose();
        views[i].pose.position.x = i == 0 ? -0.032f : 0.032f;
        views[i].fov = metalxr_default_fov();
    }

    return XR_SUCCESS;
}

typedef struct MetalXrProc {
    const char* name;
    PFN_xrVoidFunction function;
} MetalXrProc;

static const MetalXrProc kProcTable[] = {
    { "xrGetInstanceProcAddr", (PFN_xrVoidFunction)xrGetInstanceProcAddr },
    { "xrEnumerateApiLayerProperties", (PFN_xrVoidFunction)metalxr_xrEnumerateApiLayerProperties },
    { "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction)metalxr_xrEnumerateInstanceExtensionProperties },
    { "xrCreateInstance", (PFN_xrVoidFunction)metalxr_xrCreateInstance },
    { "xrDestroyInstance", (PFN_xrVoidFunction)metalxr_xrDestroyInstance },
    { "xrGetInstanceProperties", (PFN_xrVoidFunction)metalxr_xrGetInstanceProperties },
    { "xrPollEvent", (PFN_xrVoidFunction)metalxr_xrPollEvent },
    { "xrResultToString", (PFN_xrVoidFunction)metalxr_xrResultToString },
    { "xrStructureTypeToString", (PFN_xrVoidFunction)metalxr_xrStructureTypeToString },
    { "xrGetSystem", (PFN_xrVoidFunction)metalxr_xrGetSystem },
    { "xrGetSystemProperties", (PFN_xrVoidFunction)metalxr_xrGetSystemProperties },
    { "xrEnumerateViewConfigurations", (PFN_xrVoidFunction)metalxr_xrEnumerateViewConfigurations },
    { "xrGetViewConfigurationProperties", (PFN_xrVoidFunction)metalxr_xrGetViewConfigurationProperties },
    { "xrEnumerateViewConfigurationViews", (PFN_xrVoidFunction)metalxr_xrEnumerateViewConfigurationViews },
    { "xrEnumerateEnvironmentBlendModes", (PFN_xrVoidFunction)metalxr_xrEnumerateEnvironmentBlendModes },
    { "xrGetMetalGraphicsRequirementsKHR", (PFN_xrVoidFunction)metalxr_xrGetMetalGraphicsRequirementsKHR },
    { "xrGetMetalGraphicsRequirementsKHRX2", (PFN_xrVoidFunction)metalxr_xrGetMetalGraphicsRequirementsKHR },
    { "xrCreateSession", (PFN_xrVoidFunction)metalxr_xrCreateSession },
    { "xrDestroySession", (PFN_xrVoidFunction)metalxr_xrDestroySession },
    { "xrBeginSession", (PFN_xrVoidFunction)metalxr_xrBeginSession },
    { "xrEndSession", (PFN_xrVoidFunction)metalxr_xrEndSession },
    { "xrRequestExitSession", (PFN_xrVoidFunction)metalxr_xrRequestExitSession },
    { "xrEnumerateReferenceSpaces", (PFN_xrVoidFunction)metalxr_xrEnumerateReferenceSpaces },
    { "xrCreateReferenceSpace", (PFN_xrVoidFunction)metalxr_xrCreateReferenceSpace },
    { "xrGetReferenceSpaceBoundsRect", (PFN_xrVoidFunction)metalxr_xrGetReferenceSpaceBoundsRect },
    { "xrLocateSpace", (PFN_xrVoidFunction)metalxr_xrLocateSpace },
    { "xrDestroySpace", (PFN_xrVoidFunction)metalxr_xrDestroySpace },
    { "xrEnumerateSwapchainFormats", (PFN_xrVoidFunction)metalxr_xrEnumerateSwapchainFormats },
    { "xrCreateSwapchain", (PFN_xrVoidFunction)metalxr_xrCreateSwapchain },
    { "xrDestroySwapchain", (PFN_xrVoidFunction)metalxr_xrDestroySwapchain },
    { "xrEnumerateSwapchainImages", (PFN_xrVoidFunction)metalxr_xrEnumerateSwapchainImages },
    { "xrAcquireSwapchainImage", (PFN_xrVoidFunction)metalxr_xrAcquireSwapchainImage },
    { "xrWaitSwapchainImage", (PFN_xrVoidFunction)metalxr_xrWaitSwapchainImage },
    { "xrReleaseSwapchainImage", (PFN_xrVoidFunction)metalxr_xrReleaseSwapchainImage },
    { "xrWaitFrame", (PFN_xrVoidFunction)metalxr_xrWaitFrame },
    { "xrBeginFrame", (PFN_xrVoidFunction)metalxr_xrBeginFrame },
    { "xrEndFrame", (PFN_xrVoidFunction)metalxr_xrEndFrame },
    { "xrLocateViews", (PFN_xrVoidFunction)metalxr_xrLocateViews },
};

METALXR_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function)
{
    (void)instance;

    if (function == NULL || name == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *function = NULL;

    for (size_t i = 0; i < sizeof(kProcTable) / sizeof(kProcTable[0]); ++i) {
        if (strcmp(name, kProcTable[i].name) == 0) {
            *function = kProcTable[i].function;
            return XR_SUCCESS;
        }
    }

    metalxr_log("xrGetInstanceProcAddr unsupported function: %s", name);
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

METALXR_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* runtimeRequest)
{
    metalxr_log("xrNegotiateLoaderRuntimeInterface");

    if (loaderInfo == NULL || runtimeRequest == NULL) {
        metalxr_log("negotiation failed: null negotiation struct");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        metalxr_log("negotiation failed: invalid loader info");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest)) {
        metalxr_log("negotiation failed: invalid runtime request");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION) {
        metalxr_log("negotiation failed: unsupported loader runtime interface version range");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (!api_versions_overlap(loaderInfo->minApiVersion, loaderInfo->maxApiVersion, kSupportedApiVersion)) {
        metalxr_log("negotiation failed: unsupported OpenXR API version range");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = kSupportedApiVersion;
    runtimeRequest->getInstanceProcAddr = xrGetInstanceProcAddr;

    metalxr_log("negotiation succeeded: interface=%u api=1.0.0",
                runtimeRequest->runtimeInterfaceVersion);

    return XR_SUCCESS;
}
