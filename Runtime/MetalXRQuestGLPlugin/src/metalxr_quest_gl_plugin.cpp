#include <stdint.h>

#include <atomic>

#include "IUnityGraphics.h"

#if defined(__ANDROID__)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <jni.h>
#endif

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace {

constexpr int kMaxSlots = 2;
constexpr int kCreateExternalTextureEventBase = 0x4d580100;
constexpr int kUpdateSurfaceTextureEventBase = 0x4d580200;

std::atomic<uint32_t> g_external_textures[kMaxSlots];
std::atomic<int> g_surface_update_serial[kMaxSlots];
std::atomic<int> g_surface_update_success[kMaxSlots];

#if defined(__ANDROID__)
JavaVM* g_java_vm = nullptr;
jclass g_surface_decoder_class = nullptr;
jmethodID g_update_tex_image_for_slot = nullptr;
#endif

bool IsValidSlot(int slot) {
    return slot >= 0 && slot < kMaxSlots;
}

int DecodeEventSlot(int event_id, int event_base) {
    int slot = event_id - event_base;
    return IsValidSlot(slot) ? slot : -1;
}

void CreateExternalTextureForSlot(int slot) {
    if (!IsValidSlot(slot) || g_external_textures[slot].load() != 0) {
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
    g_external_textures[slot].store(texture);
#else
    (void)slot;
#endif
}

#if defined(__ANDROID__)
JNIEnv* GetJniEnv(bool* attached) {
    if (attached != nullptr) {
        *attached = false;
    }
    if (g_java_vm == nullptr) {
        return nullptr;
    }

    JNIEnv* env = nullptr;
    jint get_env = g_java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (get_env == JNI_OK) {
        return env;
    }
    if (get_env != JNI_EDETACHED) {
        return nullptr;
    }

    if (g_java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }
    if (attached != nullptr) {
        *attached = true;
    }
    return env;
}

bool CallSurfaceTextureUpdate(int slot) {
    if (!IsValidSlot(slot) || g_surface_decoder_class == nullptr || g_update_tex_image_for_slot == nullptr) {
        return false;
    }

    bool attached = false;
    JNIEnv* env = GetJniEnv(&attached);
    if (env == nullptr) {
        return false;
    }

    jboolean updated = env->CallStaticBooleanMethod(g_surface_decoder_class, g_update_tex_image_for_slot, slot);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        updated = JNI_FALSE;
    }

    if (attached && g_java_vm != nullptr) {
        g_java_vm->DetachCurrentThread();
    }
    return updated == JNI_TRUE;
}
#endif

void UpdateSurfaceTextureForSlot(int slot) {
    if (!IsValidSlot(slot)) {
        return;
    }

#if defined(__ANDROID__)
    bool updated = CallSurfaceTextureUpdate(slot);
    g_surface_update_success[slot].store(updated ? 1 : 0);
#else
    g_surface_update_success[slot].store(0);
#endif
    g_surface_update_serial[slot].fetch_add(1);
}

void UNITY_INTERFACE_API OnRenderEvent(int event_id) {
    int create_slot = DecodeEventSlot(event_id, kCreateExternalTextureEventBase);
    if (create_slot >= 0) {
        CreateExternalTextureForSlot(create_slot);
        return;
    }

    int update_slot = DecodeEventSlot(event_id, kUpdateSurfaceTextureEventBase);
    if (update_slot >= 0) {
        UpdateSurfaceTextureForSlot(update_slot);
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

int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_UpdateSurfaceTextureEventId(int slot) {
    return IsValidSlot(slot) ? kUpdateSurfaceTextureEventBase + slot : 0;
}

int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_GetExternalTexture(int slot) {
    return IsValidSlot(slot) ? static_cast<int>(g_external_textures[slot].load()) : 0;
}

int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_GetSurfaceUpdateSerial(int slot) {
    return IsValidSlot(slot) ? g_surface_update_serial[slot].load() : 0;
}

int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_GetSurfaceUpdateSuccess(int slot) {
    return IsValidSlot(slot) ? g_surface_update_success[slot].load() : 0;
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_ResetExternalTextureForTests(int slot) {
    if (!IsValidSlot(slot)) {
        return;
    }

#if defined(__ANDROID__)
    uint32_t stored_texture = g_external_textures[slot].load();
    if (stored_texture != 0) {
        GLuint texture = static_cast<GLuint>(stored_texture);
        glDeleteTextures(1, &texture);
    }
#endif

    g_external_textures[slot].store(0);
    g_surface_update_serial[slot].store(0);
    g_surface_update_success[slot].store(0);
}

#if defined(__ANDROID__)
jint JNI_OnLoad(JavaVM* vm, void*) {
    g_java_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_com_metalxr_questclient_MetalXRSurfaceDecoder_nativeRegisterClass(
    JNIEnv* env,
    jclass clazz) {
    if (env == nullptr || clazz == nullptr) {
        return;
    }

    if (g_surface_decoder_class != nullptr) {
        env->DeleteGlobalRef(g_surface_decoder_class);
        g_surface_decoder_class = nullptr;
        g_update_tex_image_for_slot = nullptr;
    }

    g_surface_decoder_class = static_cast<jclass>(env->NewGlobalRef(clazz));
    if (g_surface_decoder_class == nullptr) {
        return;
    }

    g_update_tex_image_for_slot = env->GetStaticMethodID(
        g_surface_decoder_class,
        "updateTexImageForSlot",
        "(I)Z");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        g_update_tex_image_for_slot = nullptr;
    }
}
#endif

}  // extern "C"
