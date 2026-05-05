#!/usr/bin/env bash
set -euo pipefail

# Probe runtime-side tracking, action-state, action-space, and haptic bridge behavior.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$repo_root/Runtime/MetalXRRuntime"
runtime_dylib="$runtime_dir/build/libmetalxr_runtime.dylib"
probe_source="${TMPDIR:-/tmp}/metalxr_input_bridge_probe.c"
probe_binary="${TMPDIR:-/tmp}/metalxr_input_bridge_probe"
tracking_state="${TMPDIR:-/tmp}/metalxr_tracking_state_probe.txt"
haptic_command="${TMPDIR:-/tmp}/metalxr_haptic_command_probe.txt"

if [[ ! -f "$runtime_dylib" ]]; then
  "$repo_root/Scripts/build-metalxr-runtime.sh"
fi

cat >"$tracking_state" <<'STATE'
hmd 7 1000 15 1.25 1.5 -0.5 0 0 0 1
controller 0 8 1100 15 1 0.7 0.4 0.1 -0.2 -0.2 1.2 -0.6 0 0 0 1 -0.25 1.1 -0.55 0 0 0 1
controller 1 9 1200 15 2 0.2 0.9 0.5 0.25 0.2 1.2 -0.6 0 0 0 1 0.25 1.1 -0.55 0 0 0 1
STATE
rm -f "$haptic_command"

cat > "$probe_source" <<'PROBE'
#include "MetalXRRuntime/openxr_minimal.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* runtimeRequest);
typedef XrResult (*PFN_xrCreateInstance)(const XrInstanceCreateInfo* createInfo, XrInstance* instance);
typedef XrResult (*PFN_xrDestroyInstance)(XrInstance instance);
typedef XrResult (*PFN_xrStringToPath)(XrInstance instance, const char* pathString, XrPath* path);
typedef XrResult (*PFN_xrGetSystem)(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId);
typedef XrResult (*PFN_xrCreateSession)(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session);
typedef XrResult (*PFN_xrDestroySession)(XrSession session);
typedef XrResult (*PFN_xrBeginSession)(XrSession session, const XrSessionBeginInfo* beginInfo);
typedef XrResult (*PFN_xrCreateReferenceSpace)(XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space);
typedef XrResult (*PFN_xrDestroySpace)(XrSpace space);
typedef XrResult (*PFN_xrLocateViews)(XrSession session, const XrViewLocateInfo* viewLocateInfo, XrViewState* viewState, uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrView* views);
typedef XrResult (*PFN_xrCreateActionSet)(XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet);
typedef XrResult (*PFN_xrDestroyActionSet)(XrActionSet actionSet);
typedef XrResult (*PFN_xrCreateAction)(XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action);
typedef XrResult (*PFN_xrAttachSessionActionSets)(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo);
typedef XrResult (*PFN_xrSyncActions)(XrSession session, const XrActionsSyncInfo* syncInfo);
typedef XrResult (*PFN_xrGetActionStateBoolean)(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateBoolean* state);
typedef XrResult (*PFN_xrGetActionStateFloat)(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateFloat* state);
typedef XrResult (*PFN_xrGetActionStateVector2f)(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateVector2f* state);
typedef XrResult (*PFN_xrGetActionStatePose)(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStatePose* state);
typedef XrResult (*PFN_xrCreateActionSpace)(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space);
typedef XrResult (*PFN_xrLocateSpace)(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location);
typedef XrResult (*PFN_xrApplyHapticFeedback)(XrSession session, const XrHapticActionInfo* hapticActionInfo, const XrHapticBaseHeader* hapticFeedback);

static int near(float actual, float expected)
{
    return fabsf(actual - expected) < 0.001f;
}

#define LOAD(handle, type, var, name) \
    type var = NULL; \
    result = runtimeRequest.getInstanceProcAddr((handle), (name), (PFN_xrVoidFunction*)&var); \
    if (result != XR_SUCCESS || var == NULL) { \
        fprintf(stderr, "missing %s result=%d\n", (name), result); \
        return 1; \
    }

int main(int argc, char** argv)
{
    if (argc != 2) {
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
    if (result != XR_SUCCESS) {
        return 1;
    }

    LOAD(NULL, PFN_xrCreateInstance, xrCreateInstance, "xrCreateInstance")
    XrInstanceCreateInfo instanceCreateInfo = {
        XR_TYPE_INSTANCE_CREATE_INFO,
        NULL,
        0,
        { "MetalXR Input Probe", 1, "MetalXR", 1, XR_CURRENT_API_VERSION },
        0,
        NULL,
        0,
        NULL
    };
    XrInstance instance = NULL;
    result = xrCreateInstance(&instanceCreateInfo, &instance);
    if (result != XR_SUCCESS) {
        return 1;
    }

    LOAD(instance, PFN_xrDestroyInstance, xrDestroyInstance, "xrDestroyInstance")
    LOAD(instance, PFN_xrStringToPath, xrStringToPath, "xrStringToPath")
    LOAD(instance, PFN_xrGetSystem, xrGetSystem, "xrGetSystem")
    LOAD(instance, PFN_xrCreateSession, xrCreateSession, "xrCreateSession")
    LOAD(instance, PFN_xrDestroySession, xrDestroySession, "xrDestroySession")
    LOAD(instance, PFN_xrBeginSession, xrBeginSession, "xrBeginSession")
    LOAD(instance, PFN_xrCreateReferenceSpace, xrCreateReferenceSpace, "xrCreateReferenceSpace")
    LOAD(instance, PFN_xrDestroySpace, xrDestroySpace, "xrDestroySpace")
    LOAD(instance, PFN_xrLocateViews, xrLocateViews, "xrLocateViews")
    LOAD(instance, PFN_xrCreateActionSet, xrCreateActionSet, "xrCreateActionSet")
    LOAD(instance, PFN_xrDestroyActionSet, xrDestroyActionSet, "xrDestroyActionSet")
    LOAD(instance, PFN_xrCreateAction, xrCreateAction, "xrCreateAction")
    LOAD(instance, PFN_xrAttachSessionActionSets, xrAttachSessionActionSets, "xrAttachSessionActionSets")
    LOAD(instance, PFN_xrSyncActions, xrSyncActions, "xrSyncActions")
    LOAD(instance, PFN_xrGetActionStateBoolean, xrGetActionStateBoolean, "xrGetActionStateBoolean")
    LOAD(instance, PFN_xrGetActionStateFloat, xrGetActionStateFloat, "xrGetActionStateFloat")
    LOAD(instance, PFN_xrGetActionStateVector2f, xrGetActionStateVector2f, "xrGetActionStateVector2f")
    LOAD(instance, PFN_xrGetActionStatePose, xrGetActionStatePose, "xrGetActionStatePose")
    LOAD(instance, PFN_xrCreateActionSpace, xrCreateActionSpace, "xrCreateActionSpace")
    LOAD(instance, PFN_xrLocateSpace, xrLocateSpace, "xrLocateSpace")
    LOAD(instance, PFN_xrApplyHapticFeedback, xrApplyHapticFeedback, "xrApplyHapticFeedback")

    XrPath leftHand = XR_NULL_PATH;
    XrPath rightHand = XR_NULL_PATH;
    xrStringToPath(instance, "/user/hand/left", &leftHand);
    xrStringToPath(instance, "/user/hand/right", &rightHand);
    XrPath handPaths[] = { leftHand, rightHand };

    XrSystemGetInfo getInfo = { XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    xrGetSystem(instance, &getInfo, &systemId);
    XrGraphicsBindingMetalKHR metalBinding = { XR_TYPE_GRAPHICS_BINDING_METAL_KHR, NULL, NULL };
    XrSessionCreateInfo sessionCreateInfo = { XR_TYPE_SESSION_CREATE_INFO, &metalBinding, 0, systemId };
    XrSession session = NULL;
    xrCreateSession(instance, &sessionCreateInfo, &session);

    XrActionSetCreateInfo actionSetCreateInfo = { 0 };
    actionSetCreateInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    snprintf(actionSetCreateInfo.actionSetName, sizeof(actionSetCreateInfo.actionSetName), "%s", "gameplay");
    snprintf(actionSetCreateInfo.localizedActionSetName, sizeof(actionSetCreateInfo.localizedActionSetName), "%s", "Gameplay");
    XrActionSet actionSet = NULL;
    xrCreateActionSet(instance, &actionSetCreateInfo, &actionSet);

    XrActionCreateInfo actionCreateInfo = { 0 };
    actionCreateInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    actionCreateInfo.countSubactionPaths = 2;
    actionCreateInfo.subactionPaths = handPaths;

    XrAction primary = NULL;
    snprintf(actionCreateInfo.actionName, sizeof(actionCreateInfo.actionName), "%s", "primary_button");
    actionCreateInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    xrCreateAction(actionSet, &actionCreateInfo, &primary);

    XrAction trigger = NULL;
    snprintf(actionCreateInfo.actionName, sizeof(actionCreateInfo.actionName), "%s", "trigger");
    actionCreateInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    xrCreateAction(actionSet, &actionCreateInfo, &trigger);

    XrAction thumbstick = NULL;
    snprintf(actionCreateInfo.actionName, sizeof(actionCreateInfo.actionName), "%s", "thumbstick");
    actionCreateInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    xrCreateAction(actionSet, &actionCreateInfo, &thumbstick);

    XrAction aimPose = NULL;
    snprintf(actionCreateInfo.actionName, sizeof(actionCreateInfo.actionName), "%s", "aim_pose");
    actionCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    xrCreateAction(actionSet, &actionCreateInfo, &aimPose);

    XrAction haptic = NULL;
    snprintf(actionCreateInfo.actionName, sizeof(actionCreateInfo.actionName), "%s", "haptic");
    actionCreateInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    xrCreateAction(actionSet, &actionCreateInfo, &haptic);

    XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, NULL, 1, &actionSet };
    xrAttachSessionActionSets(session, &attachInfo);
    XrActiveActionSet activeSet = { actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO, NULL, 1, &activeSet };
    xrSyncActions(session, &syncInfo);

    XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
    xrBeginSession(session, &beginInfo);

    XrReferenceSpaceCreateInfo localCreate = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_LOCAL, { {0, 0, 0, 1}, {0, 0, 0} } };
    XrSpace localSpace = NULL;
    xrCreateReferenceSpace(session, &localCreate, &localSpace);
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, localSpace };
    XrViewState viewState = { XR_TYPE_VIEW_STATE, NULL, 0 };
    XrView views[2] = { { XR_TYPE_VIEW, NULL, { {0, 0, 0, 1}, {0, 0, 0} }, {0, 0, 0, 0} }, { XR_TYPE_VIEW, NULL, { {0, 0, 0, 1}, {0, 0, 0} }, {0, 0, 0, 0} } };
    uint32_t viewCount = 0;
    xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views);
    printf("views left=%f right=%f flags=0x%llx\n", views[0].pose.position.x, views[1].pose.position.x, (unsigned long long)viewState.viewStateFlags);
    if (!near(views[0].pose.position.x, 1.218f) || !near(views[1].pose.position.x, 1.282f)) {
        return 1;
    }

    XrActionStateGetInfo stateInfo = { XR_TYPE_ACTION_STATE_GET_INFO, NULL, primary, leftHand };
    XrActionStateBoolean booleanState = { 0 };
    xrGetActionStateBoolean(session, &stateInfo, &booleanState);
    if (booleanState.currentState != XR_TRUE) {
        return 1;
    }

    stateInfo.action = trigger;
    stateInfo.subactionPath = rightHand;
    XrActionStateFloat floatState = { 0 };
    xrGetActionStateFloat(session, &stateInfo, &floatState);
    if (!near(floatState.currentState, 0.2f)) {
        return 1;
    }

    stateInfo.action = thumbstick;
    XrActionStateVector2f vectorState = { 0 };
    xrGetActionStateVector2f(session, &stateInfo, &vectorState);
    if (!near(vectorState.currentState.x, 0.5f) || !near(vectorState.currentState.y, 0.25f)) {
        return 1;
    }

    stateInfo.action = aimPose;
    XrActionStatePose poseState = { 0 };
    xrGetActionStatePose(session, &stateInfo, &poseState);
    if (poseState.isActive != XR_TRUE) {
        return 1;
    }

    XrActionSpaceCreateInfo actionSpaceCreate = { XR_TYPE_ACTION_SPACE_CREATE_INFO, NULL, aimPose, leftHand, { {0, 0, 0, 1}, {0, 0, 0} } };
    XrSpace aimSpace = NULL;
    xrCreateActionSpace(session, &actionSpaceCreate, &aimSpace);
    XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION, NULL, 0, { {0, 0, 0, 1}, {0, 0, 0} } };
    xrLocateSpace(aimSpace, localSpace, 0, &location);
    printf("leftAim x=%f y=%f z=%f\n", location.pose.position.x, location.pose.position.y, location.pose.position.z);
    if (!near(location.pose.position.x, -0.2f)) {
        return 1;
    }

    XrHapticActionInfo hapticInfo = { XR_TYPE_HAPTIC_ACTION_INFO, NULL, haptic, rightHand };
    XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION, NULL, 200000000, 120.0f, 0.75f };
    xrApplyHapticFeedback(session, &hapticInfo, (const XrHapticBaseHeader*)&vibration);

    xrDestroySpace(aimSpace);
    xrDestroySpace(localSpace);
    xrDestroySession(session);
    xrDestroyActionSet(actionSet);
    xrDestroyInstance(instance);
    return 0;
}
PROBE

clang -std=c11 -Wall -Wextra -Werror \
  -I "$runtime_dir/include" \
  "$probe_source" \
  -o "$probe_binary"

METALXR_TRACKING_STATE_PATH="$tracking_state" \
METALXR_HAPTIC_COMMAND_PATH="$haptic_command" \
  "$probe_binary" "$runtime_dylib"

if [[ ! -s "$haptic_command" ]]; then
  echo "Expected haptic command file was not written: $haptic_command" >&2
  exit 1
fi

echo "Tracking state: $tracking_state"
cat "$tracking_state"
echo "Haptic command: $haptic_command"
cat "$haptic_command"
