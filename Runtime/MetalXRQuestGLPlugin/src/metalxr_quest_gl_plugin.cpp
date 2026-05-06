#include <stdint.h>

#include "IUnityGraphics.h"

#if defined(__ANDROID__)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace {

constexpr int kMaxSlots = 2;
constexpr int kCreateExternalTextureEventBase = 0x4d580100;

#if defined(__ANDROID__)
GLuint g_external_textures[kMaxSlots] = {0, 0};
#else
uint32_t g_external_textures[kMaxSlots] = {0, 0};
#endif

bool IsValidSlot(int slot) {
    return slot >= 0 && slot < kMaxSlots;
}

int DecodeCreateEventSlot(int event_id) {
    int slot = event_id - kCreateExternalTextureEventBase;
    return IsValidSlot(slot) ? slot : -1;
}

void CreateExternalTextureForSlot(int slot) {
    if (!IsValidSlot(slot) || g_external_textures[slot] != 0) {
        return;
    }

#if defined(__ANDROID__)
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        return;
    }

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    g_external_textures[slot] = texture;
#else
    (void)slot;
#endif
}

void UNITY_INTERFACE_API OnRenderEvent(int event_id) {
    int create_slot = DecodeCreateEventSlot(event_id);
    if (create_slot >= 0) {
        CreateExternalTextureForSlot(create_slot);
    }
}

}  // namespace

extern "C" {

UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_GetRenderEventFunc() {
    return OnRenderEvent;
}

int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_CreateExternalTextureEventId(int slot) {
    return IsValidSlot(slot) ? kCreateExternalTextureEventBase + slot : 0;
}

int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_GetExternalTexture(int slot) {
    return IsValidSlot(slot) ? static_cast<int>(g_external_textures[slot]) : 0;
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_ResetExternalTextureForTests(int slot) {
    if (!IsValidSlot(slot)) {
        return;
    }

#if defined(__ANDROID__)
    if (g_external_textures[slot] != 0) {
        GLuint texture = g_external_textures[slot];
        glDeleteTextures(1, &texture);
    }
#endif

    g_external_textures[slot] = 0;
}

}  // extern "C"
