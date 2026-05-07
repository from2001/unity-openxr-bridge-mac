#ifndef METALXR_OPENXR_MINIMAL_H
#define METALXR_OPENXR_MINIMAL_H

// Minimal OpenXR loader/runtime negotiation and core lifecycle declarations
// used by the MetalXR runtime. Keep this header small until the runtime needs
// the full Khronos generated OpenXR headers.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define XRAPI_ATTR
#define XRAPI_CALL __stdcall
#define XRAPI_PTR XRAPI_CALL
#else
#define XRAPI_ATTR
#define XRAPI_CALL
#define XRAPI_PTR
#endif

#if defined(__GNUC__) || defined(__clang__)
#define METALXR_EXPORT __attribute__((visibility("default")))
#else
#define METALXR_EXPORT
#endif

typedef uint64_t XrVersion;
typedef uint64_t XrFlags64;
typedef uint32_t XrBool32;
typedef uint64_t XrSystemId;
typedef uint64_t XrPath;
typedef int64_t XrTime;
typedef int64_t XrDuration;
typedef struct XrInstance_T* XrInstance;
typedef struct XrSession_T* XrSession;
typedef struct XrSpace_T* XrSpace;
typedef struct XrSwapchain_T* XrSwapchain;
typedef struct XrActionSet_T* XrActionSet;
typedef struct XrAction_T* XrAction;
typedef int32_t XrResult;
typedef void (XRAPI_PTR *PFN_xrVoidFunction)(void);
typedef XrResult (XRAPI_PTR *PFN_xrGetInstanceProcAddr)(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function);

#define XR_MAKE_VERSION(major, minor, patch) \
    ((((uint64_t)(major)) << 48) | (((uint64_t)(minor)) << 32) | ((uint64_t)(patch)))

#define XR_VERSION_MAJOR(version) ((uint16_t)(((uint64_t)(version)) >> 48))
#define XR_VERSION_MINOR(version) ((uint16_t)(((uint64_t)(version)) >> 32))

#define XR_CURRENT_LOADER_RUNTIME_VERSION 1
#define XR_LOADER_INFO_STRUCT_VERSION 1
#define XR_RUNTIME_INFO_STRUCT_VERSION 1
#define XR_CURRENT_API_VERSION XR_MAKE_VERSION(1, 0, 0)

#define XR_NULL_HANDLE NULL
#define XR_NULL_SYSTEM_ID 0
#define XR_NULL_PATH 0
#define XR_TRUE 1
#define XR_FALSE 0

#define XR_MAX_APPLICATION_NAME_SIZE 128
#define XR_MAX_ENGINE_NAME_SIZE 128
#define XR_MAX_RUNTIME_NAME_SIZE 128
#define XR_MAX_SYSTEM_NAME_SIZE 256
#define XR_MAX_EXTENSION_NAME_SIZE 128
#define XR_MAX_API_LAYER_NAME_SIZE 256
#define XR_MAX_API_LAYER_DESCRIPTION_SIZE 256
#define XR_MAX_STRUCTURE_NAME_SIZE 64
#define XR_MAX_RESULT_STRING_SIZE 64
#define XR_MAX_ACTION_SET_NAME_SIZE 64
#define XR_MAX_ACTION_NAME_SIZE 64
#define XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE 128
#define XR_MAX_LOCALIZED_ACTION_NAME_SIZE 128
#define XR_MAX_PATH_LENGTH 256

enum {
    XR_SUCCESS = 0,
    XR_TIMEOUT_EXPIRED = 1,
    XR_EVENT_UNAVAILABLE = 4,
    XR_SPACE_BOUNDS_UNAVAILABLE = 7,
    XR_ERROR_VALIDATION_FAILURE = -1,
    XR_ERROR_RUNTIME_FAILURE = -2,
    XR_ERROR_OUT_OF_MEMORY = -3,
    XR_ERROR_API_VERSION_UNSUPPORTED = -4,
    XR_ERROR_INITIALIZATION_FAILED = -6,
    XR_ERROR_FUNCTION_UNSUPPORTED = -7,
    XR_ERROR_FEATURE_UNSUPPORTED = -8,
    XR_ERROR_EXTENSION_NOT_PRESENT = -9,
    XR_ERROR_LIMIT_REACHED = -10,
    XR_ERROR_SIZE_INSUFFICIENT = -11,
    XR_ERROR_HANDLE_INVALID = -12,
    XR_ERROR_INSTANCE_LOST = -13,
    XR_ERROR_SESSION_RUNNING = -14,
    XR_ERROR_SESSION_NOT_RUNNING = -16,
    XR_ERROR_SESSION_LOST = -17,
    XR_ERROR_SYSTEM_INVALID = -18,
    XR_ERROR_SWAPCHAIN_RECT_INVALID = -25,
    XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED = -26,
    XR_ERROR_SESSION_NOT_READY = -28,
    XR_ERROR_SESSION_NOT_STOPPING = -29,
    XR_ERROR_REFERENCE_SPACE_UNSUPPORTED = -31,
    XR_ERROR_FORM_FACTOR_UNSUPPORTED = -34,
    XR_ERROR_FORM_FACTOR_UNAVAILABLE = -35,
    XR_ERROR_API_LAYER_NOT_PRESENT = -36,
    XR_ERROR_CALL_ORDER_INVALID = -37,
    XR_ERROR_GRAPHICS_DEVICE_INVALID = -38,
    XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED = -41,
    XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED = -42,
    XR_ERROR_RUNTIME_UNAVAILABLE = -51
};

typedef enum XrStructureType {
    XR_TYPE_UNKNOWN = 0,
    XR_TYPE_API_LAYER_PROPERTIES = 1,
    XR_TYPE_EXTENSION_PROPERTIES = 2,
    XR_TYPE_INSTANCE_CREATE_INFO = 3,
    XR_TYPE_SYSTEM_GET_INFO = 4,
    XR_TYPE_SYSTEM_PROPERTIES = 5,
    XR_TYPE_VIEW_LOCATE_INFO = 6,
    XR_TYPE_VIEW = 7,
    XR_TYPE_SESSION_CREATE_INFO = 8,
    XR_TYPE_SWAPCHAIN_CREATE_INFO = 9,
    XR_TYPE_SESSION_BEGIN_INFO = 10,
    XR_TYPE_VIEW_STATE = 11,
    XR_TYPE_FRAME_END_INFO = 12,
    XR_TYPE_EVENT_DATA_BUFFER = 16,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED = 18,
    XR_TYPE_INSTANCE_PROPERTIES = 32,
    XR_TYPE_FRAME_WAIT_INFO = 33,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION = 35,
    XR_TYPE_REFERENCE_SPACE_CREATE_INFO = 37,
    XR_TYPE_VIEW_CONFIGURATION_VIEW = 41,
    XR_TYPE_SPACE_LOCATION = 42,
    XR_TYPE_FRAME_STATE = 44,
    XR_TYPE_VIEW_CONFIGURATION_PROPERTIES = 45,
    XR_TYPE_FRAME_BEGIN_INFO = 46,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW = 48,
    XR_TYPE_INTERACTION_PROFILE_STATE = 53,
    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO = 55,
    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO = 56,
    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO = 57,
    XR_TYPE_HAPTIC_VIBRATION = 13,
    XR_TYPE_ACTION_STATE_BOOLEAN = 23,
    XR_TYPE_ACTION_STATE_FLOAT = 24,
    XR_TYPE_ACTION_STATE_VECTOR2F = 25,
    XR_TYPE_ACTION_STATE_POSE = 27,
    XR_TYPE_ACTION_SET_CREATE_INFO = 28,
    XR_TYPE_ACTION_CREATE_INFO = 29,
    XR_TYPE_ACTION_SPACE_CREATE_INFO = 38,
    XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING = 51,
    XR_TYPE_ACTION_STATE_GET_INFO = 58,
    XR_TYPE_HAPTIC_ACTION_INFO = 59,
    XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO = 60,
    XR_TYPE_ACTIONS_SYNC_INFO = 61,
    XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO = 62,
    XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO = 63,
    XR_TYPE_GRAPHICS_BINDING_METAL_KHR = 1000029000,
    XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR = 1000029001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR = 1000029002,
    XR_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrStructureType;

typedef enum XrFormFactor {
    XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY = 1,
    XR_FORM_FACTOR_HANDHELD_DISPLAY = 2,
    XR_FORM_FACTOR_MAX_ENUM = 0x7FFFFFFF
} XrFormFactor;

typedef enum XrViewConfigurationType {
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO = 1,
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO = 2,
    XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrViewConfigurationType;

typedef enum XrEnvironmentBlendMode {
    XR_ENVIRONMENT_BLEND_MODE_OPAQUE = 1,
    XR_ENVIRONMENT_BLEND_MODE_ADDITIVE = 2,
    XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND = 3,
    XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM = 0x7FFFFFFF
} XrEnvironmentBlendMode;

typedef enum XrReferenceSpaceType {
    XR_REFERENCE_SPACE_TYPE_VIEW = 1,
    XR_REFERENCE_SPACE_TYPE_LOCAL = 2,
    XR_REFERENCE_SPACE_TYPE_STAGE = 3,
    XR_REFERENCE_SPACE_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrReferenceSpaceType;

typedef enum XrSessionState {
    XR_SESSION_STATE_UNKNOWN = 0,
    XR_SESSION_STATE_IDLE = 1,
    XR_SESSION_STATE_READY = 2,
    XR_SESSION_STATE_SYNCHRONIZED = 3,
    XR_SESSION_STATE_VISIBLE = 4,
    XR_SESSION_STATE_FOCUSED = 5,
    XR_SESSION_STATE_STOPPING = 6,
    XR_SESSION_STATE_LOSS_PENDING = 7,
    XR_SESSION_STATE_EXITING = 8,
    XR_SESSION_STATE_MAX_ENUM = 0x7FFFFFFF
} XrSessionState;

typedef enum XrActionType {
    XR_ACTION_TYPE_BOOLEAN_INPUT = 1,
    XR_ACTION_TYPE_FLOAT_INPUT = 2,
    XR_ACTION_TYPE_VECTOR2F_INPUT = 3,
    XR_ACTION_TYPE_POSE_INPUT = 4,
    XR_ACTION_TYPE_VIBRATION_OUTPUT = 100,
    XR_ACTION_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrActionType;

typedef XrFlags64 XrInstanceCreateFlags;
typedef XrFlags64 XrSessionCreateFlags;
typedef XrFlags64 XrSpaceLocationFlags;
typedef XrFlags64 XrViewStateFlags;
typedef XrFlags64 XrCompositionLayerFlags;
typedef XrFlags64 XrSwapchainCreateFlags;
typedef XrFlags64 XrSwapchainUsageFlags;
typedef XrFlags64 XrActionSetCreateFlags;
typedef XrFlags64 XrActionCreateFlags;
typedef XrFlags64 XrInputSourceLocalizedNameFlags;

static const XrSpaceLocationFlags XR_SPACE_LOCATION_ORIENTATION_VALID_BIT = 0x00000001;
static const XrSpaceLocationFlags XR_SPACE_LOCATION_POSITION_VALID_BIT = 0x00000002;
static const XrSpaceLocationFlags XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT = 0x00000004;
static const XrSpaceLocationFlags XR_SPACE_LOCATION_POSITION_TRACKED_BIT = 0x00000008;

static const XrViewStateFlags XR_VIEW_STATE_ORIENTATION_VALID_BIT = 0x00000001;
static const XrViewStateFlags XR_VIEW_STATE_POSITION_VALID_BIT = 0x00000002;
static const XrViewStateFlags XR_VIEW_STATE_ORIENTATION_TRACKED_BIT = 0x00000004;
static const XrViewStateFlags XR_VIEW_STATE_POSITION_TRACKED_BIT = 0x00000008;

static const XrSwapchainCreateFlags XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT = 0x00000001;
static const XrSwapchainCreateFlags XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT = 0x00000002;

static const XrSwapchainUsageFlags XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT = 0x00000001;
static const XrSwapchainUsageFlags XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x00000002;
static const XrSwapchainUsageFlags XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT = 0x00000004;
static const XrSwapchainUsageFlags XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT = 0x00000008;
static const XrSwapchainUsageFlags XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT = 0x00000010;
static const XrSwapchainUsageFlags XR_SWAPCHAIN_USAGE_SAMPLED_BIT = 0x00000020;
static const XrSwapchainUsageFlags XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT = 0x00000040;

static const XrInputSourceLocalizedNameFlags XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT = 0x00000001;
static const XrInputSourceLocalizedNameFlags XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT = 0x00000002;
static const XrInputSourceLocalizedNameFlags XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT = 0x00000004;

typedef struct XrApiLayerProperties {
    XrStructureType type;
    void* next;
    char layerName[XR_MAX_API_LAYER_NAME_SIZE];
    XrVersion specVersion;
    uint32_t layerVersion;
    char description[XR_MAX_API_LAYER_DESCRIPTION_SIZE];
} XrApiLayerProperties;

typedef struct XrExtensionProperties {
    XrStructureType type;
    void* next;
    char extensionName[XR_MAX_EXTENSION_NAME_SIZE];
    uint32_t extensionVersion;
} XrExtensionProperties;

typedef struct XrApplicationInfo {
    char applicationName[XR_MAX_APPLICATION_NAME_SIZE];
    uint32_t applicationVersion;
    char engineName[XR_MAX_ENGINE_NAME_SIZE];
    uint32_t engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;

typedef struct XrInstanceCreateInfo {
    XrStructureType type;
    const void* next;
    XrInstanceCreateFlags createFlags;
    XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount;
    const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* enabledExtensionNames;
} XrInstanceCreateInfo;

typedef struct XrInstanceProperties {
    XrStructureType type;
    void* next;
    XrVersion runtimeVersion;
    char runtimeName[XR_MAX_RUNTIME_NAME_SIZE];
} XrInstanceProperties;

typedef struct XrEventDataBuffer {
    XrStructureType type;
    const void* next;
    uint8_t varying[4000];
} XrEventDataBuffer;

typedef struct XrSystemGetInfo {
    XrStructureType type;
    const void* next;
    XrFormFactor formFactor;
} XrSystemGetInfo;

typedef struct XrSystemGraphicsProperties {
    uint32_t maxSwapchainImageHeight;
    uint32_t maxSwapchainImageWidth;
    uint32_t maxLayerCount;
} XrSystemGraphicsProperties;

typedef struct XrSystemTrackingProperties {
    XrBool32 orientationTracking;
    XrBool32 positionTracking;
} XrSystemTrackingProperties;

typedef struct XrSystemProperties {
    XrStructureType type;
    void* next;
    XrSystemId systemId;
    uint32_t vendorId;
    char systemName[XR_MAX_SYSTEM_NAME_SIZE];
    XrSystemGraphicsProperties graphicsProperties;
    XrSystemTrackingProperties trackingProperties;
} XrSystemProperties;

typedef struct XrSessionCreateInfo {
    XrStructureType type;
    const void* next;
    XrSessionCreateFlags createFlags;
    XrSystemId systemId;
} XrSessionCreateInfo;

typedef struct XrGraphicsBindingMetalKHR {
    XrStructureType type;
    const void* next;
    void* commandQueue;
} XrGraphicsBindingMetalKHR;

typedef struct XrGraphicsRequirementsMetalKHR {
    XrStructureType type;
    void* next;
    void* metalDevice;
} XrGraphicsRequirementsMetalKHR;

typedef struct XrSwapchainCreateInfo {
    XrStructureType type;
    const void* next;
    XrSwapchainCreateFlags createFlags;
    XrSwapchainUsageFlags usageFlags;
    int64_t format;
    uint32_t sampleCount;
    uint32_t width;
    uint32_t height;
    uint32_t faceCount;
    uint32_t arraySize;
    uint32_t mipCount;
} XrSwapchainCreateInfo;

typedef struct XrSwapchainImageBaseHeader {
    XrStructureType type;
    void* next;
} XrSwapchainImageBaseHeader;

typedef struct XrSwapchainImageMetalKHR {
    XrStructureType type;
    void* next;
    void* texture;
} XrSwapchainImageMetalKHR;

typedef struct XrSwapchainImageAcquireInfo {
    XrStructureType type;
    const void* next;
} XrSwapchainImageAcquireInfo;

typedef struct XrSwapchainImageWaitInfo {
    XrStructureType type;
    const void* next;
    XrDuration timeout;
} XrSwapchainImageWaitInfo;

typedef struct XrSwapchainImageReleaseInfo {
    XrStructureType type;
    const void* next;
} XrSwapchainImageReleaseInfo;

typedef struct XrVector3f {
    float x;
    float y;
    float z;
} XrVector3f;

typedef struct XrVector2f {
    float x;
    float y;
} XrVector2f;

typedef struct XrQuaternionf {
    float x;
    float y;
    float z;
    float w;
} XrQuaternionf;

typedef struct XrPosef {
    XrQuaternionf orientation;
    XrVector3f position;
} XrPosef;

typedef struct XrReferenceSpaceCreateInfo {
    XrStructureType type;
    const void* next;
    XrReferenceSpaceType referenceSpaceType;
    XrPosef poseInReferenceSpace;
} XrReferenceSpaceCreateInfo;

typedef struct XrActionSetCreateInfo {
    XrStructureType type;
    const void* next;
    XrActionSetCreateFlags createFlags;
    char actionSetName[XR_MAX_ACTION_SET_NAME_SIZE];
    char localizedActionSetName[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE];
    uint32_t priority;
} XrActionSetCreateInfo;

typedef struct XrActionCreateInfo {
    XrStructureType type;
    const void* next;
    char actionName[XR_MAX_ACTION_NAME_SIZE];
    XrActionType actionType;
    uint32_t countSubactionPaths;
    const XrPath* subactionPaths;
    char localizedActionName[XR_MAX_LOCALIZED_ACTION_NAME_SIZE];
} XrActionCreateInfo;

typedef struct XrActionSuggestedBinding {
    XrAction action;
    XrPath binding;
} XrActionSuggestedBinding;

typedef struct XrInteractionProfileSuggestedBinding {
    XrStructureType type;
    const void* next;
    XrPath interactionProfile;
    uint32_t countSuggestedBindings;
    const XrActionSuggestedBinding* suggestedBindings;
} XrInteractionProfileSuggestedBinding;

typedef struct XrSessionActionSetsAttachInfo {
    XrStructureType type;
    const void* next;
    uint32_t countActionSets;
    const XrActionSet* actionSets;
} XrSessionActionSetsAttachInfo;

typedef struct XrInteractionProfileState {
    XrStructureType type;
    void* next;
    XrPath interactionProfile;
} XrInteractionProfileState;

typedef struct XrActiveActionSet {
    XrActionSet actionSet;
    XrPath subactionPath;
} XrActiveActionSet;

typedef struct XrActionsSyncInfo {
    XrStructureType type;
    const void* next;
    uint32_t countActiveActionSets;
    const XrActiveActionSet* activeActionSets;
} XrActionsSyncInfo;

typedef struct XrBoundSourcesForActionEnumerateInfo {
    XrStructureType type;
    const void* next;
    XrAction action;
} XrBoundSourcesForActionEnumerateInfo;

typedef struct XrInputSourceLocalizedNameGetInfo {
    XrStructureType type;
    const void* next;
    XrPath sourcePath;
    XrInputSourceLocalizedNameFlags whichComponents;
} XrInputSourceLocalizedNameGetInfo;

typedef struct XrActionStateGetInfo {
    XrStructureType type;
    const void* next;
    XrAction action;
    XrPath subactionPath;
} XrActionStateGetInfo;

typedef struct XrActionStateBoolean {
    XrStructureType type;
    void* next;
    XrBool32 currentState;
    XrBool32 changedSinceLastSync;
    XrTime lastChangeTime;
    XrBool32 isActive;
} XrActionStateBoolean;

typedef struct XrActionStateFloat {
    XrStructureType type;
    void* next;
    float currentState;
    XrBool32 changedSinceLastSync;
    XrTime lastChangeTime;
    XrBool32 isActive;
} XrActionStateFloat;

typedef struct XrActionStateVector2f {
    XrStructureType type;
    void* next;
    XrVector2f currentState;
    XrBool32 changedSinceLastSync;
    XrTime lastChangeTime;
    XrBool32 isActive;
} XrActionStateVector2f;

typedef struct XrActionStatePose {
    XrStructureType type;
    void* next;
    XrBool32 isActive;
} XrActionStatePose;

typedef struct XrActionSpaceCreateInfo {
    XrStructureType type;
    const void* next;
    XrAction action;
    XrPath subactionPath;
    XrPosef poseInActionSpace;
} XrActionSpaceCreateInfo;

typedef struct XrHapticActionInfo {
    XrStructureType type;
    const void* next;
    XrAction action;
    XrPath subactionPath;
} XrHapticActionInfo;

typedef struct XrHapticBaseHeader {
    XrStructureType type;
    const void* next;
} XrHapticBaseHeader;

typedef struct XrHapticVibration {
    XrStructureType type;
    const void* next;
    XrDuration duration;
    float frequency;
    float amplitude;
} XrHapticVibration;

typedef struct XrExtent2Df {
    float width;
    float height;
} XrExtent2Df;

typedef struct XrOffset2Di {
    int32_t x;
    int32_t y;
} XrOffset2Di;

typedef struct XrExtent2Di {
    int32_t width;
    int32_t height;
} XrExtent2Di;

typedef struct XrRect2Di {
    XrOffset2Di offset;
    XrExtent2Di extent;
} XrRect2Di;

typedef struct XrSpaceLocation {
    XrStructureType type;
    void* next;
    XrSpaceLocationFlags locationFlags;
    XrPosef pose;
} XrSpaceLocation;

typedef struct XrViewConfigurationProperties {
    XrStructureType type;
    void* next;
    XrViewConfigurationType viewConfigurationType;
    XrBool32 fovMutable;
} XrViewConfigurationProperties;

typedef struct XrViewConfigurationView {
    XrStructureType type;
    void* next;
    uint32_t recommendedImageRectWidth;
    uint32_t maxImageRectWidth;
    uint32_t recommendedImageRectHeight;
    uint32_t maxImageRectHeight;
    uint32_t recommendedSwapchainSampleCount;
    uint32_t maxSwapchainSampleCount;
} XrViewConfigurationView;

typedef struct XrSessionBeginInfo {
    XrStructureType type;
    const void* next;
    XrViewConfigurationType primaryViewConfigurationType;
} XrSessionBeginInfo;

typedef struct XrFrameWaitInfo {
    XrStructureType type;
    const void* next;
} XrFrameWaitInfo;

typedef struct XrFrameState {
    XrStructureType type;
    void* next;
    XrTime predictedDisplayTime;
    XrDuration predictedDisplayPeriod;
    XrBool32 shouldRender;
} XrFrameState;

typedef struct XrFrameBeginInfo {
    XrStructureType type;
    const void* next;
} XrFrameBeginInfo;

typedef struct XrCompositionLayerBaseHeader {
    XrStructureType type;
    const void* next;
    XrCompositionLayerFlags layerFlags;
    XrSpace space;
} XrCompositionLayerBaseHeader;

typedef struct XrFrameEndInfo {
    XrStructureType type;
    const void* next;
    XrTime displayTime;
    XrEnvironmentBlendMode environmentBlendMode;
    uint32_t layerCount;
    const XrCompositionLayerBaseHeader* const* layers;
} XrFrameEndInfo;

typedef struct XrViewLocateInfo {
    XrStructureType type;
    const void* next;
    XrViewConfigurationType viewConfigurationType;
    XrTime displayTime;
    XrSpace space;
} XrViewLocateInfo;

typedef struct XrViewState {
    XrStructureType type;
    void* next;
    XrViewStateFlags viewStateFlags;
} XrViewState;

typedef struct XrFovf {
    float angleLeft;
    float angleRight;
    float angleUp;
    float angleDown;
} XrFovf;

typedef struct XrView {
    XrStructureType type;
    void* next;
    XrPosef pose;
    XrFovf fov;
} XrView;

typedef struct XrSwapchainSubImage {
    XrSwapchain swapchain;
    XrRect2Di imageRect;
    uint32_t imageArrayIndex;
} XrSwapchainSubImage;

typedef struct XrCompositionLayerProjectionView {
    XrStructureType type;
    const void* next;
    XrPosef pose;
    XrFovf fov;
    XrSwapchainSubImage subImage;
} XrCompositionLayerProjectionView;

typedef struct XrCompositionLayerProjection {
    XrStructureType type;
    const void* next;
    XrCompositionLayerFlags layerFlags;
    XrSpace space;
    uint32_t viewCount;
    const XrCompositionLayerProjectionView* views;
} XrCompositionLayerProjection;

typedef struct XrEventDataSessionStateChanged {
    XrStructureType type;
    const void* next;
    XrSession session;
    XrSessionState state;
    XrTime time;
} XrEventDataSessionStateChanged;

typedef enum XrLoaderInterfaceStructs {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO = 4,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO = 5,
    XR_LOADER_INTERFACE_STRUCTS_MAX_ENUM = 0x7FFFFFFF
} XrLoaderInterfaceStructs;

typedef struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateRuntimeRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t runtimeInterfaceVersion;
    XrVersion runtimeApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* runtimeRequest);

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function);

#ifdef __cplusplus
}
#endif

#endif
