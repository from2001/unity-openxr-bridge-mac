package com.metalxr.questclient;

import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.view.Surface;

import java.nio.ByteBuffer;

public final class MetalXRSurfaceDecoder {
    private static final int MAX_TEXTURE_SLOTS = 2;
    private static final MetalXRSurfaceDecoder[] DECODERS_BY_SLOT = new MetalXRSurfaceDecoder[MAX_TEXTURE_SLOTS];

    static {
        try {
            System.loadLibrary("metalxrquestgl");
            nativeRegisterClass();
        } catch (Throwable ignored) {
        }
    }

    private final String eyeName;
    private final int textureSlot;
    private final MediaCodec.BufferInfo bufferInfo;

    private MediaCodec codec;
    private SurfaceTexture surfaceTexture;
    private Surface surface;
    private final float[] textureTransform;
    private int textureName;
    private int configuredWidth;
    private int configuredHeight;
    private boolean ownsTexture;
    private volatile boolean frameAvailable;
    private String lastError;

    public MetalXRSurfaceDecoder(String eyeName) {
        this(eyeName, -1);
    }

    public MetalXRSurfaceDecoder(String eyeName, int textureSlot) {
        this.eyeName = eyeName == null ? "unknown" : eyeName;
        this.textureSlot = textureSlot;
        this.bufferInfo = new MediaCodec.BufferInfo();
        this.textureTransform = new float[16];
        this.lastError = "";
        setIdentityTransform();
    }

    private static native void nativeRegisterClass();

    public int createExternalTexture() {
        int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        if (textures[0] == 0) {
            lastError = "glGenTextures returned zero for " + eyeName + " eye";
            return 0;
        }

        textureName = textures[0];
        ownsTexture = true;
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureName);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0);
        lastError = "";
        return textureName;
    }

    public boolean configure(int width, int height, int externalTextureName) {
        if (width <= 0 || height <= 0 || externalTextureName == 0) {
            lastError = "invalid Surface decoder configuration for " + eyeName + " eye";
            return false;
        }

        if (codec != null &&
                configuredWidth == width &&
                configuredHeight == height &&
                textureName == externalTextureName) {
            return true;
        }

        releaseDecoderOnly();
        textureName = externalTextureName;
        configuredWidth = width;
        configuredHeight = height;

        try {
            surfaceTexture = new SurfaceTexture(textureName);
            surfaceTexture.setDefaultBufferSize(width, height);
            surfaceTexture.setOnFrameAvailableListener(new SurfaceTexture.OnFrameAvailableListener() {
                @Override
                public void onFrameAvailable(SurfaceTexture texture) {
                    frameAvailable = true;
                }
            });
            surface = new Surface(surfaceTexture);

            MediaFormat format = MediaFormat.createVideoFormat("video/avc", width, height);
            format.setInteger("low-latency", 1);
            codec = MediaCodec.createDecoderByType("video/avc");
            codec.configure(format, surface, null, 0);
            codec.start();
            registerSlot();
            lastError = "";
            return true;
        } catch (Exception exception) {
            lastError = "Surface decoder configure failed for " + eyeName + " eye: " + exception.getMessage();
            releaseDecoderOnly();
            return false;
        }
    }

    public boolean queueFrame(byte[] encodedBytes, long presentationTimeUs) {
        if (codec == null || encodedBytes == null || encodedBytes.length == 0) {
            return false;
        }

        try {
            int inputIndex = codec.dequeueInputBuffer(0);
            if (inputIndex < 0) {
                return false;
            }

            ByteBuffer inputBuffer = codec.getInputBuffer(inputIndex);
            if (inputBuffer == null || encodedBytes.length > inputBuffer.capacity()) {
                codec.queueInputBuffer(inputIndex, 0, 0, presentationTimeUs, 0);
                lastError = "Surface decoder input buffer too small for " + eyeName + " eye";
                return false;
            }

            inputBuffer.clear();
            inputBuffer.put(encodedBytes);
            codec.queueInputBuffer(inputIndex, 0, encodedBytes.length, presentationTimeUs, 0);
            return true;
        } catch (Exception exception) {
            lastError = "Surface decoder input failed for " + eyeName + " eye: " + exception.getMessage();
            releaseDecoderOnly();
            return false;
        }
    }

    public long drainOne(long timeoutUs) {
        if (codec == null) {
            return -1L;
        }

        try {
            int outputIndex = codec.dequeueOutputBuffer(bufferInfo, timeoutUs);
            if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                return -2L;
            }

            if (outputIndex < 0) {
                return -1L;
            }

            boolean render = bufferInfo.size > 0;
            long presentationTimeUs = render ? bufferInfo.presentationTimeUs : -1L;
            codec.releaseOutputBuffer(outputIndex, render);
            return presentationTimeUs;
        } catch (Exception exception) {
            lastError = "Surface decoder output failed for " + eyeName + " eye: " + exception.getMessage();
            releaseDecoderOnly();
            return -1L;
        }
    }

    public boolean updateTexImage() {
        if (surfaceTexture == null) {
            return false;
        }

        try {
            surfaceTexture.updateTexImage();
            surfaceTexture.getTransformMatrix(textureTransform);
            frameAvailable = false;
            return true;
        } catch (Exception exception) {
            lastError = "SurfaceTexture update failed for " + eyeName + " eye: " + exception.getMessage();
            return false;
        }
    }

    public static boolean updateTexImageForSlot(int textureSlot) {
        MetalXRSurfaceDecoder decoder = decoderForSlot(textureSlot);
        if (decoder == null) {
            return false;
        }

        return decoder.updateTexImage();
    }

    public static float[] transformMatrixForSlot(int textureSlot) {
        MetalXRSurfaceDecoder decoder = decoderForSlot(textureSlot);
        if (decoder == null) {
            return identityTransform();
        }

        return decoder.copyTextureTransform();
    }

    public String getLastError() {
        return lastError;
    }

    public boolean isConfigured() {
        return codec != null;
    }

    public int getTextureName() {
        return textureName;
    }

    public void release() {
        releaseDecoderOnly();
        if (ownsTexture && textureName != 0) {
            int[] textures = new int[] { textureName };
            GLES20.glDeleteTextures(1, textures, 0);
        }

        textureName = 0;
        ownsTexture = false;
    }

    private void releaseDecoderOnly() {
        unregisterSlot();

        if (codec != null) {
            try {
                codec.stop();
            } catch (Exception ignored) {
            }

            try {
                codec.release();
            } catch (Exception ignored) {
            }

            codec = null;
        }

        if (surface != null) {
            surface.release();
            surface = null;
        }

        if (surfaceTexture != null) {
            surfaceTexture.release();
            surfaceTexture = null;
        }

        configuredWidth = 0;
        configuredHeight = 0;
        frameAvailable = false;
        setIdentityTransform();
    }

    private float[] copyTextureTransform() {
        float[] copy = new float[16];
        System.arraycopy(textureTransform, 0, copy, 0, textureTransform.length);
        return copy;
    }

    private void setIdentityTransform() {
        for (int index = 0; index < textureTransform.length; index++) {
            textureTransform[index] = 0.0f;
        }

        textureTransform[0] = 1.0f;
        textureTransform[5] = 1.0f;
        textureTransform[10] = 1.0f;
        textureTransform[15] = 1.0f;
    }

    private static float[] identityTransform() {
        float[] matrix = new float[16];
        matrix[0] = 1.0f;
        matrix[5] = 1.0f;
        matrix[10] = 1.0f;
        matrix[15] = 1.0f;
        return matrix;
    }

    private void registerSlot() {
        if (textureSlot < 0 || textureSlot >= MAX_TEXTURE_SLOTS) {
            return;
        }

        synchronized (DECODERS_BY_SLOT) {
            DECODERS_BY_SLOT[textureSlot] = this;
        }
    }

    private void unregisterSlot() {
        if (textureSlot < 0 || textureSlot >= MAX_TEXTURE_SLOTS) {
            return;
        }

        synchronized (DECODERS_BY_SLOT) {
            if (DECODERS_BY_SLOT[textureSlot] == this) {
                DECODERS_BY_SLOT[textureSlot] = null;
            }
        }
    }

    private static MetalXRSurfaceDecoder decoderForSlot(int textureSlot) {
        if (textureSlot < 0 || textureSlot >= MAX_TEXTURE_SLOTS) {
            return null;
        }

        synchronized (DECODERS_BY_SLOT) {
            return DECODERS_BY_SLOT[textureSlot];
        }
    }
}
