#include <stdint.h>

#include <atomic>
#include <cstring>

#include "IUnityGraphics.h"

#if defined(__ANDROID__)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <jni.h>
#endif

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace {

constexpr int kMaxSlots = 2;
constexpr int kCreateExternalTextureEventBase = 0x4d580100;
constexpr int kUpdateSurfaceTextureEventBase = 0x4d580200;

std::atomic<uint32_t> g_surface_textures[kMaxSlots];
std::atomic<uint32_t> g_unity_textures[kMaxSlots];
std::atomic<int> g_texture_width[kMaxSlots];
std::atomic<int> g_texture_height[kMaxSlots];
std::atomic<int> g_surface_update_serial[kMaxSlots];
std::atomic<int> g_surface_update_success[kMaxSlots];
std::atomic<int> g_blit_visible[kMaxSlots];
std::atomic<int> g_blit_invisible_count[kMaxSlots];
std::atomic<int> g_blit_near_black_logged[kMaxSlots];
std::atomic<int> g_surface_present_unavailable;

#if defined(__ANDROID__)
JavaVM* g_java_vm = nullptr;
jclass g_surface_decoder_class = nullptr;
jmethodID g_update_tex_image_for_slot = nullptr;
jmethodID g_transform_matrix_for_slot = nullptr;
GLuint g_blit_program = 0;
GLuint g_blit_fbo = 0;
GLuint g_blit_vertex_buffer = 0;
GLint g_blit_position_location = -1;
GLint g_blit_tex_coord_location = -1;
GLint g_blit_matrix_location = -1;
GLint g_blit_texture_location = -1;
#endif

bool IsValidSlot(int slot) {
    return slot >= 0 && slot < kMaxSlots;
}

int DecodeEventSlot(int event_id, int event_base) {
    int slot = event_id - event_base;
    return IsValidSlot(slot) ? slot : -1;
}

void CreateExternalTextureForSlot(int slot) {
    if (!IsValidSlot(slot)) {
        return;
    }

#if defined(__ANDROID__)
    if (g_surface_textures[slot].load() == 0) {
        GLuint surface_texture = 0;
        glGenTextures(1, &surface_texture);
        if (surface_texture == 0) {
            return;
        }

        glBindTexture(GL_TEXTURE_EXTERNAL_OES, surface_texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        g_surface_textures[slot].store(surface_texture);
    }

    const int width = g_texture_width[slot].load();
    const int height = g_texture_height[slot].load();
    if (width <= 0 || height <= 0) {
        return;
    }

    uint32_t stored_unity_texture = g_unity_textures[slot].load();
    if (stored_unity_texture != 0) {
        glBindTexture(GL_TEXTURE_2D, stored_unity_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    GLuint unity_texture = 0;
    glGenTextures(1, &unity_texture);
    if (unity_texture == 0) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, unity_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_unity_textures[slot].store(unity_texture);
#else
    (void)slot;
#endif
}

#if defined(__ANDROID__)
void LogPluginError(const char* message) {
    __android_log_print(ANDROID_LOG_WARN, "MetalXRQuestGL", "%s", message != nullptr ? message : "unknown error");
}

void LogPluginGLError(const char* message, GLenum error) {
    __android_log_print(
        ANDROID_LOG_WARN,
        "MetalXRQuestGL",
        "%s gl_error=0x%04x",
        message != nullptr ? message : "unknown error",
        static_cast<unsigned int>(error));
}

void RestoreCapability(GLenum capability, GLboolean was_enabled) {
    if (was_enabled == GL_TRUE) {
        glEnable(capability);
    } else {
        glDisable(capability);
    }
}

bool BlitOutputLooksVisible(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    const int sample_x[] = {width / 4, width / 2, (width * 3) / 4, width / 2};
    const int sample_y[] = {height / 4, height / 2, height / 2, (height * 3) / 4};
    int bright_samples = 0;
    int color_samples = 0;
    for (int i = 0; i < 4; ++i) {
        GLubyte pixel[4] = {0, 0, 0, 0};
        glReadPixels(sample_x[i], sample_y[i], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        GLenum read_error = glGetError();
        if (read_error != GL_NO_ERROR) {
            return true;
        }

        int max_channel = pixel[0];
        int min_channel = pixel[0];
        for (int channel = 1; channel < 3; ++channel) {
            if (pixel[channel] > max_channel) {
                max_channel = pixel[channel];
            }
            if (pixel[channel] < min_channel) {
                min_channel = pixel[channel];
            }
        }
        if (max_channel > 12) {
            bright_samples++;
        }
        if (max_channel - min_channel > 6) {
            color_samples++;
        }
    }

    return bright_samples > 0 && color_samples > 0;
}

void IdentityMatrix(float matrix[16]) {
    std::memset(matrix, 0, sizeof(float) * 16);
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        LogPluginError("shader compile failed");
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool EnsureBlitProgram() {
    if (g_blit_program != 0 && g_blit_fbo != 0 && g_blit_vertex_buffer != 0) {
        return true;
    }

    static const char* kVertexShader =
        "attribute vec2 a_position;\n"
        "attribute vec2 a_tex_coord;\n"
        "uniform mat4 u_tex_matrix;\n"
        "varying vec2 v_tex_coord;\n"
        "void main() {\n"
        "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
        "  vec4 transformed = u_tex_matrix * vec4(a_tex_coord, 0.0, 1.0);\n"
        "  v_tex_coord = transformed.xy;\n"
        "}\n";

    static const char* kFragmentShader =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\n"
        "uniform samplerExternalOES u_texture;\n"
        "varying vec2 v_tex_coord;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(u_texture, v_tex_coord);\n"
        "}\n";

    GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment_shader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vertex_shader == 0 || fragment_shader == 0) {
        if (vertex_shader != 0) {
            glDeleteShader(vertex_shader);
        }
        if (fragment_shader != 0) {
            glDeleteShader(fragment_shader);
        }
        return false;
    }

    GLuint program = glCreateProgram();
    if (program == 0) {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }

    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        LogPluginError("blit program link failed");
        glDeleteProgram(program);
        return false;
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (fbo == 0) {
        glDeleteProgram(program);
        return false;
    }

    static const GLfloat kVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    GLuint vertex_buffer = 0;
    glGenBuffers(1, &vertex_buffer);
    if (vertex_buffer == 0) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteProgram(program);
        return false;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    g_blit_program = program;
    g_blit_fbo = fbo;
    g_blit_vertex_buffer = vertex_buffer;
    g_blit_position_location = glGetAttribLocation(program, "a_position");
    g_blit_tex_coord_location = glGetAttribLocation(program, "a_tex_coord");
    g_blit_matrix_location = glGetUniformLocation(program, "u_tex_matrix");
    g_blit_texture_location = glGetUniformLocation(program, "u_texture");
    return g_blit_position_location >= 0 &&
           g_blit_tex_coord_location >= 0 &&
           g_blit_matrix_location >= 0 &&
           g_blit_texture_location >= 0;
}

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

bool ReadSurfaceTransformMatrix(JNIEnv* env, int slot, float matrix[16]) {
    IdentityMatrix(matrix);
    if (env == nullptr || g_surface_decoder_class == nullptr || g_transform_matrix_for_slot == nullptr) {
        return false;
    }

    jobject matrix_object = env->CallStaticObjectMethod(g_surface_decoder_class, g_transform_matrix_for_slot, slot);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (matrix_object == nullptr) {
        return false;
    }

    jfloatArray matrix_array = static_cast<jfloatArray>(matrix_object);
    jsize matrix_length = env->GetArrayLength(matrix_array);
    if (matrix_length >= 16) {
        env->GetFloatArrayRegion(matrix_array, 0, 16, matrix);
    }
    env->DeleteLocalRef(matrix_object);
    return matrix_length >= 16;
}

bool CallSurfaceTextureUpdate(int slot, float matrix[16]) {
    IdentityMatrix(matrix);
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
    if (updated == JNI_TRUE) {
        ReadSurfaceTransformMatrix(env, slot, matrix);
    }

    if (attached && g_java_vm != nullptr) {
        g_java_vm->DetachCurrentThread();
    }
    return updated == JNI_TRUE;
}

bool BlitSurfaceTextureToUnityTexture(int slot, const float matrix[16]) {
    if (!IsValidSlot(slot) || !EnsureBlitProgram()) {
        return false;
    }

    const GLuint surface_texture = static_cast<GLuint>(g_surface_textures[slot].load());
    const GLuint unity_texture = static_cast<GLuint>(g_unity_textures[slot].load());
    const int width = g_texture_width[slot].load();
    const int height = g_texture_height[slot].load();
    if (surface_texture == 0 || unity_texture == 0 || width <= 0 || height <= 0) {
        return false;
    }

    GLint previous_program = 0;
    GLint previous_fbo = 0;
    GLint previous_array_buffer = 0;
    GLint previous_active_texture = GL_TEXTURE0;
    GLint previous_viewport[4] = {0, 0, 0, 0};
    GLboolean previous_blend = glIsEnabled(GL_BLEND);
    GLboolean previous_cull_face = glIsEnabled(GL_CULL_FACE);
    GLboolean previous_depth_test = glIsEnabled(GL_DEPTH_TEST);
    GLboolean previous_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean previous_stencil_test = glIsEnabled(GL_STENCIL_TEST);
    GLboolean previous_color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, previous_color_mask);
    while (glGetError() != GL_NO_ERROR) {
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_blit_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, unity_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LogPluginError("surface texture blit framebuffer was incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
        return false;
    }

    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g_blit_program);
    glBindBuffer(GL_ARRAY_BUFFER, g_blit_vertex_buffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, surface_texture);
    glUniform1i(g_blit_texture_location, 0);
    glUniformMatrix4fv(g_blit_matrix_location, 1, GL_FALSE, matrix);
    glEnableVertexAttribArray(static_cast<GLuint>(g_blit_position_location));
    glEnableVertexAttribArray(static_cast<GLuint>(g_blit_tex_coord_location));
    glVertexAttribPointer(
        static_cast<GLuint>(g_blit_position_location),
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(GLfloat),
        reinterpret_cast<const void*>(0));
    glVertexAttribPointer(
        static_cast<GLuint>(g_blit_tex_coord_location),
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(GLfloat),
        reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(static_cast<GLuint>(g_blit_position_location));
    glDisableVertexAttribArray(static_cast<GLuint>(g_blit_tex_coord_location));

    GLenum error = glGetError();
    bool visible = true;
    if (error == GL_NO_ERROR && g_blit_visible[slot].load() == 0) {
        visible = BlitOutputLooksVisible(width, height);
        if (visible) {
            g_blit_visible[slot].store(1);
            g_blit_invisible_count[slot].store(0);
        } else {
            const int invisible_count = g_blit_invisible_count[slot].fetch_add(1) + 1;
            if (invisible_count < 6) {
                visible = true;
            } else if (g_blit_near_black_logged[slot].exchange(1) == 0) {
                g_surface_present_unavailable.store(1);
                LogPluginError("surface texture blit output stayed near-black; using CPU decode fallback");
            }
        }
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previous_array_buffer));
    glUseProgram(static_cast<GLuint>(previous_program));
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    RestoreCapability(GL_BLEND, previous_blend);
    RestoreCapability(GL_CULL_FACE, previous_cull_face);
    RestoreCapability(GL_DEPTH_TEST, previous_depth_test);
    RestoreCapability(GL_SCISSOR_TEST, previous_scissor_test);
    RestoreCapability(GL_STENCIL_TEST, previous_stencil_test);
    glColorMask(
        previous_color_mask[0],
        previous_color_mask[1],
        previous_color_mask[2],
        previous_color_mask[3]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_fbo));
    glActiveTexture(static_cast<GLenum>(previous_active_texture));
    if (error != GL_NO_ERROR) {
        LogPluginGLError("surface texture blit failed", error);
    }
    return error == GL_NO_ERROR && visible;
}
#endif

void UpdateSurfaceTextureForSlot(int slot) {
    if (!IsValidSlot(slot)) {
        return;
    }

#if defined(__ANDROID__)
    float matrix[16];
    bool updated = CallSurfaceTextureUpdate(slot, matrix);
    bool blitted = updated &&
                   g_surface_present_unavailable.load() == 0 &&
                   BlitSurfaceTextureToUnityTexture(slot, matrix);
    g_surface_update_success[slot].store(blitted ? 1 : 0);
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
    return IsValidSlot(slot) ? static_cast<int>(g_surface_textures[slot].load()) : 0;
}

int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_GetUnityTexture(int slot) {
    return IsValidSlot(slot) ? static_cast<int>(g_unity_textures[slot].load()) : 0;
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API MetalXRQuestGL_SetExternalTextureSize(int slot, int width, int height) {
    if (!IsValidSlot(slot) || width <= 0 || height <= 0) {
        return;
    }

    g_texture_width[slot].store(width);
    g_texture_height[slot].store(height);
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
    uint32_t stored_surface_texture = g_surface_textures[slot].load();
    if (stored_surface_texture != 0) {
        GLuint texture = static_cast<GLuint>(stored_surface_texture);
        glDeleteTextures(1, &texture);
    }
    uint32_t stored_unity_texture = g_unity_textures[slot].load();
    if (stored_unity_texture != 0) {
        GLuint texture = static_cast<GLuint>(stored_unity_texture);
        glDeleteTextures(1, &texture);
    }
#endif

    g_surface_textures[slot].store(0);
    g_unity_textures[slot].store(0);
    g_texture_width[slot].store(0);
    g_texture_height[slot].store(0);
    g_surface_update_serial[slot].store(0);
    g_surface_update_success[slot].store(0);
    g_blit_visible[slot].store(0);
    g_blit_invisible_count[slot].store(0);
    g_blit_near_black_logged[slot].store(0);
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
        g_transform_matrix_for_slot = nullptr;
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

    g_transform_matrix_for_slot = env->GetStaticMethodID(
        g_surface_decoder_class,
        "transformMatrixForSlot",
        "(I)[F");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        g_transform_matrix_for_slot = nullptr;
    }
}
#endif

}  // extern "C"
