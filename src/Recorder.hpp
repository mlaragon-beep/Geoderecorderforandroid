#pragma once

#include <Geode/Geode.hpp>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <filesystem>

#ifdef GEODE_IS_ANDROID
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#endif

class RecorderOverlay : public cocos2d::CCLayer {
public:
    static RecorderOverlay* create();
    bool init() override;
    void updateVisualState();
    void onToggleRecord(cocos2d::CCObject*);

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override;
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override;
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override;

private:
    cocos2d::CCSprite* m_icon = nullptr;
    cocos2d::CCLabelBMFont* m_timerLabel = nullptr;
    cocos2d::CCPoint m_touchStartPos;
    cocos2d::CCPoint m_nodeStartPos;
    bool m_dragging = false;
    float m_elapsedSec = 0.f;

    void update(float dt) override;
};

class InternalRecorder {
public:
    static InternalRecorder& get() {
        static InternalRecorder instance;
        return instance;
    }

    bool startRecording();
    void stopRecording();
    bool isRecording() const { return m_recording.load(); }

    void captureFrame();
    void pushAudioSamples(const float* interleavedData, unsigned int lengthSamples, int channels);

    RecorderOverlay* m_overlay = nullptr;

private:
    InternalRecorder() = default;
    std::atomic<bool> m_recording{false};
    std::mutex m_muxerMutex;

#ifdef GEODE_IS_ANDROID
    void drainEncoder(AMediaCodec* codec, int& trackIndex, bool endOfStream);
    void setupFMODTap();
    void removeFMODTap();
    void initBlitShader();
    std::filesystem::path resolveOutputPath();

    AMediaCodec* m_videoCodec = nullptr;
    AMediaCodec* m_audioCodec = nullptr;
    AMediaMuxer* m_muxer = nullptr;
    ANativeWindow* m_inputSurface = nullptr;

    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_gameContext = EGL_NO_CONTEXT;
    EGLSurface m_gameSurface = EGL_NO_SURFACE;
    EGLSurface m_encoderSurface = EGL_NO_SURFACE;

    GLuint m_fbo = 0;
    GLuint m_fboTex = 0;
    GLuint m_blitProgram = 0;
    GLuint m_quadVAO = 0;

    int m_videoTrackIndex = -1;
    int m_audioTrackIndex = -1;
    bool m_muxerStarted = false;

    int m_width = 1280;
    int m_height = 720;
    int m_fps = 60;
    int64_t m_startTimeNs = 0;
    int64_t m_lastFrameTimeNs = 0;
    int64_t m_audioSampleCount = 0;
    int m_sampleRate = 48000;
    int m_channels = 2;

    FMOD::DSP* m_captureDSP = nullptr;
#endif
};
