#!/usr/bin/env bash
set -euo pipefail

# Probe the runtime by dlopen'ing the dylib, running OpenXR loader
# negotiation, and exercising the dummy HMD lifecycle.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$repo_root/Runtime/MetalXRRuntime"
runtime_dylib="$runtime_dir/build/libmetalxr_runtime.dylib"
probe_source="${TMPDIR:-/tmp}/metalxr_runtime_probe.c"
probe_binary="${TMPDIR:-/tmp}/metalxr_runtime_probe"
probe_log="${TMPDIR:-/tmp}/metalxr_runtime_probe.log"
probe_dump_dir="${TMPDIR:-/tmp}/metalxr_frame_dump"
probe_timing_state="${TMPDIR:-/tmp}/metalxr_runtime_probe_timing_state.txt"

if [[ ! -f "$runtime_dylib" ]]; then
  "$repo_root/Scripts/build-metalxr-runtime.sh"
fi

cat > "$probe_source" <<'PROBE'
#include "MetalXRRuntime/openxr_minimal.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* runtimeRequest);

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /path/to/libmetalxr_runtime.dylib\n", argv[0]);
        return 2;
    }

    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    PFN_xrNegotiateLoaderRuntimeInterface negotiate =
        (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(library, "xrNegotiateLoaderRuntimeInterface");
    if (negotiate == NULL) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }

    XrNegotiateLoaderInfo loaderInfo = {
        XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
        XR_LOADER_INFO_STRUCT_VERSION,
        sizeof(XrNegotiateLoaderInfo),
        XR_CURRENT_LOADER_RUNTIME_VERSION,
        XR_CURRENT_LOADER_RUNTIME_VERSION,
        XR_MAKE_VERSION(1, 0, 0),
        XR_MAKE_VERSION(1, 1, 0)
    };

    XrNegotiateRuntimeRequest runtimeRequest = {
        XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
        XR_RUNTIME_INFO_STRUCT_VERSION,
        sizeof(XrNegotiateRuntimeRequest),
        0,
        0,
        NULL
    };

    XrResult result = negotiate(&loaderInfo, &runtimeRequest);
    printf("negotiate=%d interface=%u api=%u.%u getInstanceProcAddr=%s\n",
           result,
           runtimeRequest.runtimeInterfaceVersion,
           XR_VERSION_MAJOR(runtimeRequest.runtimeApiVersion),
           XR_VERSION_MINOR(runtimeRequest.runtimeApiVersion),
           runtimeRequest.getInstanceProcAddr != NULL ? "yes" : "no");

    if (result != XR_SUCCESS || runtimeRequest.getInstanceProcAddr == NULL) {
        return 1;
    }

    typedef XrResult (*PFN_xrEnumerateInstanceExtensionProperties)(
        const char* layerName,
        uint32_t propertyCapacityInput,
        uint32_t* propertyCountOutput,
        XrExtensionProperties* properties);
    typedef XrResult (*PFN_xrCreateInstance)(const XrInstanceCreateInfo* createInfo, XrInstance* instance);
    typedef XrResult (*PFN_xrDestroyInstance)(XrInstance instance);
    typedef XrResult (*PFN_xrGetInstanceProperties)(XrInstance instance, XrInstanceProperties* instanceProperties);
    typedef XrResult (*PFN_xrPollEvent)(XrInstance instance, XrEventDataBuffer* eventData);
    typedef XrResult (*PFN_xrGetSystem)(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId);
    typedef XrResult (*PFN_xrGetSystemProperties)(XrInstance instance, XrSystemId systemId, XrSystemProperties* properties);
    typedef XrResult (*PFN_xrEnumerateViewConfigurations)(
        XrInstance instance,
        XrSystemId systemId,
        uint32_t viewConfigurationTypeCapacityInput,
        uint32_t* viewConfigurationTypeCountOutput,
        XrViewConfigurationType* viewConfigurationTypes);
    typedef XrResult (*PFN_xrGetViewConfigurationProperties)(
        XrInstance instance,
        XrSystemId systemId,
        XrViewConfigurationType viewConfigurationType,
        XrViewConfigurationProperties* configurationProperties);
    typedef XrResult (*PFN_xrEnumerateViewConfigurationViews)(
        XrInstance instance,
        XrSystemId systemId,
        XrViewConfigurationType viewConfigurationType,
        uint32_t viewCapacityInput,
        uint32_t* viewCountOutput,
        XrViewConfigurationView* views);
    typedef XrResult (*PFN_xrEnumerateEnvironmentBlendModes)(
        XrInstance instance,
        XrSystemId systemId,
        XrViewConfigurationType viewConfigurationType,
        uint32_t environmentBlendModeCapacityInput,
        uint32_t* environmentBlendModeCountOutput,
        XrEnvironmentBlendMode* environmentBlendModes);
    typedef XrResult (*PFN_xrGetMetalGraphicsRequirementsKHR)(
        XrInstance instance,
        XrSystemId systemId,
        XrGraphicsRequirementsMetalKHR* graphicsRequirements);
    typedef XrResult (*PFN_xrCreateSession)(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session);
    typedef XrResult (*PFN_xrDestroySession)(XrSession session);
    typedef XrResult (*PFN_xrBeginSession)(XrSession session, const XrSessionBeginInfo* beginInfo);
    typedef XrResult (*PFN_xrEndSession)(XrSession session);
    typedef XrResult (*PFN_xrRequestExitSession)(XrSession session);
    typedef XrResult (*PFN_xrEnumerateReferenceSpaces)(
        XrSession session,
        uint32_t spaceCapacityInput,
        uint32_t* spaceCountOutput,
        XrReferenceSpaceType* spaces);
    typedef XrResult (*PFN_xrCreateReferenceSpace)(
        XrSession session,
        const XrReferenceSpaceCreateInfo* createInfo,
        XrSpace* space);
    typedef XrResult (*PFN_xrDestroySpace)(XrSpace space);
    typedef XrResult (*PFN_xrEnumerateSwapchainFormats)(
        XrSession session,
        uint32_t formatCapacityInput,
        uint32_t* formatCountOutput,
        int64_t* formats);
    typedef XrResult (*PFN_xrCreateSwapchain)(
        XrSession session,
        const XrSwapchainCreateInfo* createInfo,
        XrSwapchain* swapchain);
    typedef XrResult (*PFN_xrDestroySwapchain)(XrSwapchain swapchain);
    typedef XrResult (*PFN_xrEnumerateSwapchainImages)(
        XrSwapchain swapchain,
        uint32_t imageCapacityInput,
        uint32_t* imageCountOutput,
        XrSwapchainImageBaseHeader* images);
    typedef XrResult (*PFN_xrAcquireSwapchainImage)(
        XrSwapchain swapchain,
        const XrSwapchainImageAcquireInfo* acquireInfo,
        uint32_t* index);
    typedef XrResult (*PFN_xrWaitSwapchainImage)(
        XrSwapchain swapchain,
        const XrSwapchainImageWaitInfo* waitInfo);
    typedef XrResult (*PFN_xrReleaseSwapchainImage)(
        XrSwapchain swapchain,
        const XrSwapchainImageReleaseInfo* releaseInfo);
    typedef XrResult (*PFN_xrWaitFrame)(XrSession session, const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState);
    typedef XrResult (*PFN_xrBeginFrame)(XrSession session, const XrFrameBeginInfo* frameBeginInfo);
    typedef XrResult (*PFN_xrEndFrame)(XrSession session, const XrFrameEndInfo* frameEndInfo);
    typedef XrResult (*PFN_xrLocateViews)(
        XrSession session,
        const XrViewLocateInfo* viewLocateInfo,
        XrViewState* viewState,
        uint32_t viewCapacityInput,
        uint32_t* viewCountOutput,
        XrView* views);

#define LOAD_XR_PROC(handle, type, variable, proc_name) \
    type variable = NULL; \
    result = runtimeRequest.getInstanceProcAddr((handle), (proc_name), (PFN_xrVoidFunction*)&variable); \
    printf("getProcAddr(%s)=%d function=%s\n", (proc_name), result, variable != NULL ? "yes" : "no"); \
    if (result != XR_SUCCESS || variable == NULL) { \
        return 1; \
    }

    LOAD_XR_PROC(NULL, PFN_xrEnumerateInstanceExtensionProperties, xrEnumerateInstanceExtensionProperties, "xrEnumerateInstanceExtensionProperties")
    LOAD_XR_PROC(NULL, PFN_xrCreateInstance, xrCreateInstance, "xrCreateInstance")

    uint32_t extensionCount = 0;
    result = xrEnumerateInstanceExtensionProperties(NULL, 0, &extensionCount, NULL);
    printf("enumerateExtensions=%d count=%u\n", result, extensionCount);
    if (result != XR_SUCCESS || extensionCount == 0) {
        return 1;
    }

    XrExtensionProperties extensions[8] = { 0 };
    result = xrEnumerateInstanceExtensionProperties(NULL, 8, &extensionCount, extensions);
    printf("enumerateExtensionsFill=%d first=%s\n", result, extensions[0].extensionName);
    if (result != XR_SUCCESS) {
        return 1;
    }

    const char* enabledExtensions[] = {
        "XR_KHRX2_metal_enable"
    };
    XrInstanceCreateInfo instanceCreateInfo = {
        XR_TYPE_INSTANCE_CREATE_INFO,
        NULL,
        0,
        {
            "MetalXR Probe",
            1,
            "MetalXR",
            1,
            XR_CURRENT_API_VERSION
        },
        0,
        NULL,
        1,
        enabledExtensions
    };

    XrInstance instance = NULL;
    result = xrCreateInstance(&instanceCreateInfo, &instance);
    printf("createInstance=%d instance=%s\n", result, instance != NULL ? "yes" : "no");
    if (result != XR_SUCCESS || instance == NULL) {
        return 1;
    }

    LOAD_XR_PROC(instance, PFN_xrDestroyInstance, xrDestroyInstance, "xrDestroyInstance")
    LOAD_XR_PROC(instance, PFN_xrGetInstanceProperties, xrGetInstanceProperties, "xrGetInstanceProperties")
    LOAD_XR_PROC(instance, PFN_xrPollEvent, xrPollEvent, "xrPollEvent")
    LOAD_XR_PROC(instance, PFN_xrGetSystem, xrGetSystem, "xrGetSystem")
    LOAD_XR_PROC(instance, PFN_xrGetSystemProperties, xrGetSystemProperties, "xrGetSystemProperties")
    LOAD_XR_PROC(instance, PFN_xrEnumerateViewConfigurations, xrEnumerateViewConfigurations, "xrEnumerateViewConfigurations")
    LOAD_XR_PROC(instance, PFN_xrGetViewConfigurationProperties, xrGetViewConfigurationProperties, "xrGetViewConfigurationProperties")
    LOAD_XR_PROC(instance, PFN_xrEnumerateViewConfigurationViews, xrEnumerateViewConfigurationViews, "xrEnumerateViewConfigurationViews")
    LOAD_XR_PROC(instance, PFN_xrEnumerateEnvironmentBlendModes, xrEnumerateEnvironmentBlendModes, "xrEnumerateEnvironmentBlendModes")
    LOAD_XR_PROC(instance, PFN_xrGetMetalGraphicsRequirementsKHR, xrGetMetalGraphicsRequirementsKHR, "xrGetMetalGraphicsRequirementsKHR")
    LOAD_XR_PROC(instance, PFN_xrGetMetalGraphicsRequirementsKHR, xrGetMetalGraphicsRequirementsKHRX2, "xrGetMetalGraphicsRequirementsKHRX2")
    LOAD_XR_PROC(instance, PFN_xrCreateSession, xrCreateSession, "xrCreateSession")
    LOAD_XR_PROC(instance, PFN_xrDestroySession, xrDestroySession, "xrDestroySession")
    LOAD_XR_PROC(instance, PFN_xrBeginSession, xrBeginSession, "xrBeginSession")
    LOAD_XR_PROC(instance, PFN_xrEndSession, xrEndSession, "xrEndSession")
    LOAD_XR_PROC(instance, PFN_xrRequestExitSession, xrRequestExitSession, "xrRequestExitSession")
    LOAD_XR_PROC(instance, PFN_xrEnumerateReferenceSpaces, xrEnumerateReferenceSpaces, "xrEnumerateReferenceSpaces")
    LOAD_XR_PROC(instance, PFN_xrCreateReferenceSpace, xrCreateReferenceSpace, "xrCreateReferenceSpace")
    LOAD_XR_PROC(instance, PFN_xrDestroySpace, xrDestroySpace, "xrDestroySpace")
    LOAD_XR_PROC(instance, PFN_xrEnumerateSwapchainFormats, xrEnumerateSwapchainFormats, "xrEnumerateSwapchainFormats")
    LOAD_XR_PROC(instance, PFN_xrCreateSwapchain, xrCreateSwapchain, "xrCreateSwapchain")
    LOAD_XR_PROC(instance, PFN_xrDestroySwapchain, xrDestroySwapchain, "xrDestroySwapchain")
    LOAD_XR_PROC(instance, PFN_xrEnumerateSwapchainImages, xrEnumerateSwapchainImages, "xrEnumerateSwapchainImages")
    LOAD_XR_PROC(instance, PFN_xrAcquireSwapchainImage, xrAcquireSwapchainImage, "xrAcquireSwapchainImage")
    LOAD_XR_PROC(instance, PFN_xrWaitSwapchainImage, xrWaitSwapchainImage, "xrWaitSwapchainImage")
    LOAD_XR_PROC(instance, PFN_xrReleaseSwapchainImage, xrReleaseSwapchainImage, "xrReleaseSwapchainImage")
    LOAD_XR_PROC(instance, PFN_xrWaitFrame, xrWaitFrame, "xrWaitFrame")
    LOAD_XR_PROC(instance, PFN_xrBeginFrame, xrBeginFrame, "xrBeginFrame")
    LOAD_XR_PROC(instance, PFN_xrEndFrame, xrEndFrame, "xrEndFrame")
    LOAD_XR_PROC(instance, PFN_xrLocateViews, xrLocateViews, "xrLocateViews")

#undef LOAD_XR_PROC

    XrInstanceProperties instanceProperties = { XR_TYPE_INSTANCE_PROPERTIES, NULL, 0, { 0 } };
    result = xrGetInstanceProperties(instance, &instanceProperties);
    printf("instanceProperties=%d runtime=%s\n", result, instanceProperties.runtimeName);
    if (result != XR_SUCCESS) {
        return 1;
    }

    XrSystemGetInfo systemGetInfo = {
        XR_TYPE_SYSTEM_GET_INFO,
        NULL,
        XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY
    };
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    result = xrGetSystem(instance, &systemGetInfo, &systemId);
    printf("getSystem=%d system=%llu\n", result, (unsigned long long)systemId);
    if (result != XR_SUCCESS || systemId == XR_NULL_SYSTEM_ID) {
        return 1;
    }

    XrSystemProperties systemProperties = { 0 };
    systemProperties.type = XR_TYPE_SYSTEM_PROPERTIES;
    result = xrGetSystemProperties(instance, systemId, &systemProperties);
    printf("systemProperties=%d name=%s eye=%ux%u\n",
           result,
           systemProperties.systemName,
           systemProperties.graphicsProperties.maxSwapchainImageWidth,
           systemProperties.graphicsProperties.maxSwapchainImageHeight);
    if (result != XR_SUCCESS) {
        return 1;
    }

    uint32_t viewConfigCount = 0;
    XrViewConfigurationType viewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    result = xrEnumerateViewConfigurations(instance, systemId, 1, &viewConfigCount, &viewConfig);
    printf("viewConfigurations=%d count=%u first=%d\n", result, viewConfigCount, viewConfig);
    if (result != XR_SUCCESS || viewConfig != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return 1;
    }

    XrViewConfigurationProperties viewConfigProperties = { 0 };
    viewConfigProperties.type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
    result = xrGetViewConfigurationProperties(instance, systemId, viewConfig, &viewConfigProperties);
    printf("viewConfigurationProperties=%d fovMutable=%u\n", result, viewConfigProperties.fovMutable);
    if (result != XR_SUCCESS) {
        return 1;
    }

    uint32_t viewCount = 0;
    XrViewConfigurationView configViews[2] = { 0 };
    configViews[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    configViews[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    result = xrEnumerateViewConfigurationViews(instance, systemId, viewConfig, 2, &viewCount, configViews);
    printf("viewConfigurationViews=%d count=%u first=%ux%u\n",
           result,
           viewCount,
           configViews[0].recommendedImageRectWidth,
           configViews[0].recommendedImageRectHeight);
    if (result != XR_SUCCESS || viewCount != 2) {
        return 1;
    }

    uint32_t blendModeCount = 0;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    result = xrEnumerateEnvironmentBlendModes(instance, systemId, viewConfig, 1, &blendModeCount, &blendMode);
    printf("blendModes=%d count=%u first=%d\n", result, blendModeCount, blendMode);
    if (result != XR_SUCCESS || blendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
        return 1;
    }

    XrGraphicsRequirementsMetalKHR graphicsRequirements = { 0 };
    graphicsRequirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
    result = xrGetMetalGraphicsRequirementsKHR(instance, systemId, &graphicsRequirements);
    printf("metalGraphicsRequirements=%d metalDevice=%s\n",
           result,
           graphicsRequirements.metalDevice != NULL ? "yes" : "no");
    if (result != XR_SUCCESS || graphicsRequirements.metalDevice == NULL) {
        return 1;
    }

    XrGraphicsBindingMetalKHR metalBinding = {
        XR_TYPE_GRAPHICS_BINDING_METAL_KHR,
        NULL,
        NULL
    };
    XrSessionCreateInfo sessionCreateInfo = {
        XR_TYPE_SESSION_CREATE_INFO,
        &metalBinding,
        0,
        systemId
    };
    XrSession session = NULL;
    result = xrCreateSession(instance, &sessionCreateInfo, &session);
    printf("createSession=%d session=%s\n", result, session != NULL ? "yes" : "no");
    if (result != XR_SUCCESS || session == NULL) {
        return 1;
    }

    XrEventDataBuffer eventData = { 0 };
    eventData.type = XR_TYPE_EVENT_DATA_BUFFER;
    result = xrPollEvent(instance, &eventData);
    printf("pollEventReady=%d type=%d\n", result, eventData.type);
    if (result != XR_SUCCESS || eventData.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        return 1;
    }

    XrSessionBeginInfo beginInfo = {
        XR_TYPE_SESSION_BEGIN_INFO,
        NULL,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
    };
    result = xrBeginSession(session, &beginInfo);
    printf("beginSession=%d\n", result);
    if (result != XR_SUCCESS) {
        return 1;
    }

    for (int i = 0; i < 3; ++i) {
        eventData.type = XR_TYPE_EVENT_DATA_BUFFER;
        result = xrPollEvent(instance, &eventData);
        printf("pollEventRunning[%d]=%d type=%d\n", i, result, eventData.type);
        if (result != XR_SUCCESS || eventData.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            return 1;
        }
    }

    uint32_t referenceSpaceCount = 0;
    XrReferenceSpaceType referenceSpaces[3] = { 0 };
    result = xrEnumerateReferenceSpaces(session, 3, &referenceSpaceCount, referenceSpaces);
    printf("referenceSpaces=%d count=%u\n", result, referenceSpaceCount);
    if (result != XR_SUCCESS || referenceSpaceCount != 3) {
        return 1;
    }

    XrReferenceSpaceCreateInfo referenceSpaceCreateInfo = {
        XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        NULL,
        XR_REFERENCE_SPACE_TYPE_LOCAL,
        {
            { 0.0f, 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, 0.0f }
        }
    };
    XrSpace localSpace = NULL;
    result = xrCreateReferenceSpace(session, &referenceSpaceCreateInfo, &localSpace);
    printf("createReferenceSpace=%d space=%s\n", result, localSpace != NULL ? "yes" : "no");
    if (result != XR_SUCCESS || localSpace == NULL) {
        return 1;
    }

    uint32_t swapchainFormatCount = 0;
    int64_t swapchainFormats[8] = { 0 };
    result = xrEnumerateSwapchainFormats(session, 8, &swapchainFormatCount, swapchainFormats);
    printf("swapchainFormats=%d count=%u first=%lld\n",
           result,
           swapchainFormatCount,
           (long long)swapchainFormats[0]);
    if (result != XR_SUCCESS || swapchainFormatCount == 0) {
        return 1;
    }

    XrSwapchain swapchains[2] = { NULL, NULL };
    XrSwapchainImageMetalKHR swapchainImages[2][3] = { 0 };
    for (uint32_t eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo swapchainCreateInfo = {
            XR_TYPE_SWAPCHAIN_CREATE_INFO,
            NULL,
            0,
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
            swapchainFormats[0],
            1,
            configViews[eye].recommendedImageRectWidth,
            configViews[eye].recommendedImageRectHeight,
            1,
            1,
            1
        };
        result = xrCreateSwapchain(session, &swapchainCreateInfo, &swapchains[eye]);
        printf("createSwapchain[%u]=%d swapchain=%s\n",
               eye,
               result,
               swapchains[eye] != NULL ? "yes" : "no");
        if (result != XR_SUCCESS || swapchains[eye] == NULL) {
            return 1;
        }

        uint32_t swapchainImageCount = 0;
        result = xrEnumerateSwapchainImages(
            swapchains[eye],
            3,
            &swapchainImageCount,
            (XrSwapchainImageBaseHeader*)swapchainImages[eye]);
        printf("swapchainImages[%u]=%d count=%u firstTexture=%s\n",
               eye,
               result,
               swapchainImageCount,
               swapchainImages[eye][0].texture != NULL ? "yes" : "no");
        if (result != XR_SUCCESS || swapchainImageCount != 3 || swapchainImages[eye][0].texture == NULL) {
            return 1;
        }
    }

    for (int frame = 0; frame < 3; ++frame) {
        XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO, NULL };
        XrFrameState frameState = { 0 };
        frameState.type = XR_TYPE_FRAME_STATE;
        result = xrWaitFrame(session, &waitInfo, &frameState);
        printf("waitFrame[%d]=%d predicted=%lld period=%lld render=%u\n",
               frame,
               result,
               (long long)frameState.predictedDisplayTime,
               (long long)frameState.predictedDisplayPeriod,
               frameState.shouldRender);
        if (result != XR_SUCCESS || frameState.shouldRender != XR_TRUE) {
            return 1;
        }

        XrFrameBeginInfo frameBeginInfo = { XR_TYPE_FRAME_BEGIN_INFO, NULL };
        result = xrBeginFrame(session, &frameBeginInfo);
        printf("beginFrame[%d]=%d\n", frame, result);
        if (result != XR_SUCCESS) {
            return 1;
        }

        XrViewLocateInfo viewLocateInfo = {
            XR_TYPE_VIEW_LOCATE_INFO,
            NULL,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            frameState.predictedDisplayTime,
            localSpace
        };
        XrViewState viewState = { 0 };
        viewState.type = XR_TYPE_VIEW_STATE;
        XrView views[2] = { 0 };
        views[0].type = XR_TYPE_VIEW;
        views[1].type = XR_TYPE_VIEW;
        viewCount = 0;
        result = xrLocateViews(session, &viewLocateInfo, &viewState, 2, &viewCount, views);
        printf("locateViews[%d]=%d count=%u leftX=%f rightX=%f\n",
               frame,
               result,
               viewCount,
               views[0].pose.position.x,
               views[1].pose.position.x);
        if (result != XR_SUCCESS || viewCount != 2) {
            return 1;
        }

        uint32_t swapchainImageIndices[2] = { 0, 0 };
        XrCompositionLayerProjectionView projectionViews[2] = { 0 };
        for (uint32_t eye = 0; eye < 2; ++eye) {
            XrSwapchainImageAcquireInfo acquireInfo = {
                XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
                NULL
            };
            result = xrAcquireSwapchainImage(swapchains[eye], &acquireInfo, &swapchainImageIndices[eye]);
            printf("acquireSwapchainImage[%d][%u]=%d index=%u\n",
                   frame,
                   eye,
                   result,
                   swapchainImageIndices[eye]);
            if (result != XR_SUCCESS) {
                return 1;
            }

            XrSwapchainImageWaitInfo waitImageInfo = {
                XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                NULL,
                0
            };
            result = xrWaitSwapchainImage(swapchains[eye], &waitImageInfo);
            printf("waitSwapchainImage[%d][%u]=%d\n", frame, eye, result);
            if (result != XR_SUCCESS) {
                return 1;
            }

            XrSwapchainImageReleaseInfo releaseInfo = {
                XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
                NULL
            };
            result = xrReleaseSwapchainImage(swapchains[eye], &releaseInfo);
            printf("releaseSwapchainImage[%d][%u]=%d\n", frame, eye, result);
            if (result != XR_SUCCESS) {
                return 1;
            }

            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projectionViews[eye].pose = views[eye].pose;
            projectionViews[eye].fov = views[eye].fov;
            projectionViews[eye].subImage.swapchain = swapchains[eye];
            projectionViews[eye].subImage.imageRect.offset.x = 0;
            projectionViews[eye].subImage.imageRect.offset.y = 0;
            projectionViews[eye].subImage.imageRect.extent.width = (int32_t)configViews[eye].recommendedImageRectWidth;
            projectionViews[eye].subImage.imageRect.extent.height = (int32_t)configViews[eye].recommendedImageRectHeight;
            projectionViews[eye].subImage.imageArrayIndex = 0;
        }

        XrCompositionLayerProjection projectionLayer = {
            XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            NULL,
            0,
            localSpace,
            2,
            projectionViews
        };
        const XrCompositionLayerBaseHeader* layers[] = {
            (const XrCompositionLayerBaseHeader*)&projectionLayer
        };
        XrFrameEndInfo frameEndInfo = {
            XR_TYPE_FRAME_END_INFO,
            NULL,
            frameState.predictedDisplayTime,
            XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
            1,
            layers
        };
        result = xrEndFrame(session, &frameEndInfo);
        printf("endFrame[%d]=%d\n", frame, result);
        if (result != XR_SUCCESS) {
            return 1;
        }
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        result = xrDestroySwapchain(swapchains[eye]);
        printf("destroySwapchain[%u]=%d\n", eye, result);
        if (result != XR_SUCCESS) {
            return 1;
        }
    }

    result = xrRequestExitSession(session);
    printf("requestExitSession=%d\n", result);
    if (result != XR_SUCCESS) {
        return 1;
    }

    result = xrEndSession(session);
    printf("endSession=%d\n", result);
    if (result != XR_SUCCESS) {
        return 1;
    }

    result = xrDestroySpace(localSpace);
    printf("destroySpace=%d\n", result);
    if (result != XR_SUCCESS) {
        return 1;
    }

    result = xrDestroySession(session);
    printf("destroySession=%d\n", result);
    if (result != XR_SUCCESS) {
        return 1;
    }

    result = xrDestroyInstance(instance);
    printf("destroyInstance=%d\n", result);
    if (result != XR_SUCCESS) {
        return 1;
    }

    return 0;
}
PROBE

clang -std=c11 -Wall -Wextra -Werror \
  -I "$runtime_dir/include" \
  "$probe_source" \
  -o "$probe_binary"

rm -f "$probe_log"
rm -f "$probe_timing_state"
rm -rf "$probe_dump_dir"
mkdir -p "$probe_dump_dir"
METALXR_RUNTIME_LOG="$probe_log" \
METALXR_FRAME_DUMP_DIR="$probe_dump_dir" \
METALXR_TIMING_STATE_PATH="$probe_timing_state" \
"$probe_binary" "$runtime_dylib"

echo "Runtime log: $probe_log"
sed -n '1,180p' "$probe_log"

metadata_file="$probe_dump_dir/frame_000001.txt"
if [[ ! -f "$metadata_file" ]]; then
  echo "Expected frame metadata was not written: $metadata_file" >&2
  exit 1
fi

echo "Frame metadata: $metadata_file"
sed -n '1,80p' "$metadata_file"
