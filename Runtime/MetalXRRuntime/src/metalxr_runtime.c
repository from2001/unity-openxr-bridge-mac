#include "MetalXRRuntime/openxr_minimal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const XrVersion kSupportedApiVersion = XR_MAKE_VERSION(1, 0, 0);

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

static XrResult XRAPI_CALL metalxr_xrEnumerateApiLayerProperties(
    uint32_t propertyCapacityInput,
    uint32_t* propertyCountOutput,
    void* properties)
{
    (void)propertyCapacityInput;
    (void)properties;

    metalxr_log("xrEnumerateApiLayerProperties");

    if (propertyCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *propertyCountOutput = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrEnumerateInstanceExtensionProperties(
    const char* layerName,
    uint32_t propertyCapacityInput,
    uint32_t* propertyCountOutput,
    void* properties)
{
    (void)layerName;
    (void)propertyCapacityInput;
    (void)properties;

    metalxr_log("xrEnumerateInstanceExtensionProperties");

    if (propertyCountOutput == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *propertyCountOutput = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL metalxr_xrCreateInstance(const void* createInfo, XrInstance* instance)
{
    (void)createInfo;

    metalxr_log("xrCreateInstance returning XR_ERROR_RUNTIME_UNAVAILABLE; lifecycle is tracked by issue #3");

    if (instance != NULL) {
        *instance = NULL;
    }

    return XR_ERROR_RUNTIME_UNAVAILABLE;
}

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

    if (strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = (PFN_xrVoidFunction)xrGetInstanceProcAddr;
        return XR_SUCCESS;
    }

    if (strcmp(name, "xrEnumerateApiLayerProperties") == 0) {
        *function = (PFN_xrVoidFunction)metalxr_xrEnumerateApiLayerProperties;
        return XR_SUCCESS;
    }

    if (strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0) {
        *function = (PFN_xrVoidFunction)metalxr_xrEnumerateInstanceExtensionProperties;
        return XR_SUCCESS;
    }

    if (strcmp(name, "xrCreateInstance") == 0) {
        *function = (PFN_xrVoidFunction)metalxr_xrCreateInstance;
        return XR_SUCCESS;
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
