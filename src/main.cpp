#include "Recorder.hpp"
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <algorithm>

using namespace geode::prelude;

#ifdef GEODE_IS_ANDROID

static int64_t getSystemTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static FMOD_RESULT F_CALLBACK fmodCaptureCallback(
    FMOD_DSP_STATE* dsp_state,
    float* inbuffer,
    float* outbuffer,
    unsigned int length,
    int inchannels,
    int* outchannels
) {
    memcpy(outbuffer, inbuffer, length * inchannels * sizeof(float));
    *outchannels = inchannels;

    if (InternalRecorder::get().isRecording()) {
        InternalRecorder::get().pushAudioSamples(inbuffer, length, inchannels);
    }
    return FMOD_OK;
}

void InternalRecorder::setupFMODTap() {
    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return;

    int rate = 48000;
    engine->m_system->getSoftwareFormat(&rate, nullptr, nullptr);
    m_sampleRate = rate;

    FMOD_DSP_DESCRIPTION dspDesc = {};
    strncpy(dspDesc.name, "GeodeAudioTap", sizeof(dspDesc.name));
    dspDesc.version = 0x00010000;
    dspDesc.numinputbuffers = 1;
    dspDesc.numoutputbuffers = 1;
    dspDesc.read = fmodCaptureCallback;

    if (engine->m_system->createDSP(&dspDesc, &m_captureDSP) == FMOD_OK && m_captureDSP) {
        FMOD::ChannelGroup* masterGroup = nullptr;
        if (engine->m_system->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup) {
            masterGroup->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, m_captureDSP);
        }
    }
}

void InternalRecorder::removeFMODTap() {
    auto engine = FMODAudioEngine::sharedEngine();
    if (engine && engine->m_system && m_captureDSP) {
        FMOD::ChannelGroup* masterGroup = nullptr;
        if (engine->m_system->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup) {
            masterGroup->removeDSP(m_captureDSP);
        }
        m_captureDSP->release();
        m_captureDSP = nullptr;
    }
}

void InternalRecorder::initBlitShader() {
    if (m_blitProgram != 0) return;

    const char* vsSrc = R"(#version 300 es
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTex;
        out vec2 vTex;
        void main() {
            vTex = aTex;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
    )";

    const char* fsSrc = R"(#version 300 es
        precision mediump float;
        in vec2 vTex;
        out vec4 fragColor;
        uniform sampler2D uTex;
        void main() {
            fragColor = texture(uTex, vTex);
        }
    )";

    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    };

    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    m_blitProgram = glCreateProgram();
    glAttachShader(m_blitProgram, vs);
    glAttachShader(m_blitProgram, fs);
    glLinkProgram(m_blitProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };

    GLuint vbo;
    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &vbo);
    glBindVertexArray(m_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
}

std::filesystem::path InternalRecorder::resolveOutputPath() {
    auto mod = Mod::get();
    std::string filename = fmt::format("GD_{}.mp4", std::time(nullptr));

    if (mod->getSettingValue<bool>("save-to-movies")) {
        std::filesystem::path moviesDir = "/storage/emulated/0/Movies/GeometryDash";
        std::error_code ec;
        std::filesystem::create_directories(moviesDir, ec);
        if (!ec) {
            auto candidate = moviesDir / filename;
            int testFd = open(candidate.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (testFd >= 0) {
                close(testFd);
                return candidate;
            }
        }
    }

    auto fallbackDir = mod->getSaveDir() / "recordings";
    std::filesystem::create_directories(fallbackDir);
    return fallbackDir / filename;
}

bool InternalRecorder::startRecording() {
    if (m_recording.load()) return false;

    auto mod = Mod::get();
    float scale = mod->getSettingValue<double>("resolution-scale");
    m_fps = mod->getSettingValue<int64_t>("target-fps");
    int bitrate = mod->getSettingValue<int64_t>("video-bitrate-mbps") * 1000000;
    bool useHevc = mod->getSettingValue<bool>("use-hevc");
    bool recordAudio = mod->getSettingValue<bool>("record-audio");
    int audioBitrate = mod->getSettingValue<int64_t>("audio-bitrate-kbps") * 1000;

    auto frameSize = CCEGLView::sharedOpenGLView()->getFrameSize();
    m_width = (static_cast<int>(frameSize.width * scale) / 16) * 16;
    m_height = (static_cast<int>(frameSize.height * scale) / 16) * 16;
    if (m_width < 256) m_width = 256;
    if (m_height < 144) m_height = 144;

    const char* videoMime = useHevc ? "video/hevc" : "video/avc";
    m_videoCodec = AMediaCodec_createEncoderByType(videoMime);
    if (!m_videoCodec) return false;

    AMediaFormat* vFormat = AMediaFormat_new();
    AMediaFormat_setString(vFormat, AMEDIAFORMAT_KEY_MIME, videoMime);
    AMediaFormat_setInt32(vFormat, AMEDIAFORMAT_KEY_WIDTH, m_width);
    AMediaFormat_setInt32(vFormat, AMEDIAFORMAT_KEY_HEIGHT, m_height);
    AMediaFormat_setInt32(vFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F000789);
    AMediaFormat_setInt32(vFormat, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(vFormat, AMEDIAFORMAT_KEY_FRAME_RATE, m_fps);
    AMediaFormat_setInt32(vFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);

    if (AMediaCodec_configure(m_videoCodec, vFormat, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK) {
        AMediaFormat_delete(vFormat);
        AMediaCodec_delete(m_videoCodec);
        m_videoCodec = nullptr;
        return false;
    }
    AMediaFormat_delete(vFormat);

    AMediaCodec_createInputSurface(m_videoCodec, &m_inputSurface);
    AMediaCodec_start(m_videoCodec);

    m_eglDisplay = eglGetCurrentDisplay();
    m_gameContext = eglGetCurrentContext();
    m_gameSurface = eglGetCurrentSurface(EGL_DRAW);

    EGLint configID = 0;
    eglQueryContext(m_eglDisplay, m_gameContext, EGL_CONFIG_ID, &configID);
    EGLConfig config;
    EGLint numConfigs;
    EGLint configAttribs[] = { EGL_CONFIG_ID, configID, EGL_NONE };
    eglChooseConfig(m_eglDisplay, configAttribs, &config, 1, &numConfigs);

    m_encoderSurface = eglCreateWindowSurface(m_eglDisplay, config, m_inputSurface, nullptr);

    glGenFramebuffers(1, &m_fbo);
    glGenTextures(1, &m_fboTex);
    glBindTexture(GL_TEXTURE_2D, m_fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fboTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    initBlitShader();

    if (recordAudio) {
        setupFMODTap();
        m_audioCodec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
        AMediaFormat* aFormat = AMediaFormat_new();
        AMediaFormat_setString(aFormat, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(aFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, m_sampleRate);
        AMediaFormat_setInt32(aFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, m_channels);
        AMediaFormat_setInt32(aFormat, AMEDIAFORMAT_KEY_BIT_RATE, audioBitrate);
        AMediaFormat_setInt32(aFormat, AMEDIAFORMAT_KEY_AAC_PROFILE, 2);
        AMediaCodec_configure(m_audioCodec, aFormat, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(aFormat);
        AMediaCodec_start(m_audioCodec);
    }

    auto filePath = resolveOutputPath();
    int fd = open(filePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;

    m_muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    close(fd);

    m_videoTrackIndex = -1;
    m_audioTrackIndex = -1;
    m_muxerStarted = false;
    m_audioSampleCount = 0;
    m_startTimeNs = getSystemTimeNs();
    m_lastFrameTimeNs = 0;
    m_recording.store(true);

    if (m_overlay) m_overlay->updateVisualState();
    Notification::create("Recording Started", NotificationIcon::Success)->show();
    return true;
}

void InternalRecorder::captureFrame() {
    if (!m_recording.load() || !m_videoCodec) return;

    int64_t nowNs = getSystemTimeNs();
    int64_t frameIntervalNs = 1000000000LL / m_fps;
    if (m_lastFrameTimeNs != 0 && (nowNs - m_lastFrameTimeNs) < frameIntervalNs) {
        return;
    }
    m_lastFrameTimeNs = nowNs;

    auto frameSize = CCEGLView::sharedOpenGLView()->getFrameSize();
    int gameW = static_cast<int>(frameSize.width);
    int gameH = static_cast<int>(frameSize.height);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
    glBlitFramebuffer(0, 0, gameW, gameH, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (eglMakeCurrent(m_eglDisplay, m_encoderSurface, m_encoderSurface, m_gameContext)) {
        glViewport(0, 0, m_width, m_height);
        glUseProgram(m_blitProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_fboTex);
        glBindVertexArray(m_quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        eglSwapBuffers(m_eglDisplay, m_encoderSurface);

        eglMakeCurrent(m_eglDisplay, m_gameSurface, m_gameSurface, m_gameContext);
        glViewport(0, 0, gameW, gameH);
    }

    drainEncoder(m_videoCodec, m_videoTrackIndex, false);
}

void InternalRecorder::pushAudioSamples(const float* interleavedData, unsigned int lengthSamples, int channels) {
    if (!m_recording.load() || !m_audioCodec) return;

    ssize_t inIdx = AMediaCodec_dequeueInputBuffer(m_audioCodec, 0);
    if (inIdx >= 0) {
        size_t bufSize = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(m_audioCodec, inIdx, &bufSize);
        if (buf) {
            size_t totalSamples = lengthSamples * channels;
            size_t bytesNeeded = totalSamples * sizeof(int16_t);
            if (bytesNeeded <= bufSize) {
                int16_t* pcm16 = reinterpret_cast<int16_t*>(buf);
                for (size_t i = 0; i < totalSamples; ++i) {
                    float clamped = std::max(-1.0f, std::min(1.0f, interleavedData[i]));
                    pcm16[i] = static_cast<int16_t>(clamped * 32767.0f);
                }
                int64_t ptsUs = (m_audioSampleCount * 1000000LL) / m_sampleRate;
                m_audioSampleCount += lengthSamples;
                AMediaCodec_queueInputBuffer(m_audioCodec, inIdx, 0, bytesNeeded, ptsUs, 0);
            }
        }
    }

    drainEncoder(m_audioCodec, m_audioTrackIndex, false);
}

void InternalRecorder::drainEncoder(AMediaCodec* codec, int& trackIndex, bool endOfStream) {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (!codec || !m_muxer) return;

    AMediaCodecBufferInfo info;
    while (true) {
        ssize_t status = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);
        if (status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            if (trackIndex < 0) {
                AMediaFormat* newFormat = AMediaCodec_getOutputFormat(codec);
                trackIndex = AMediaMuxer_addTrack(m_muxer, newFormat);
                AMediaFormat_delete(newFormat);

                bool audioReady = (m_audioCodec == nullptr) || (m_audioTrackIndex >= 0);
                if (m_videoTrackIndex >= 0 && audioReady && !m_muxerStarted) {
                    AMediaMuxer_start(m_muxer);
                    m_muxerStarted = true;
                }
            }
        } else if (status >= 0) {
            size_t outSize = 0;
            uint8_t* outBuf = AMediaCodec_getOutputBuffer(codec, status, &outSize);
            if (outBuf && info.size > 0 && m_muxerStarted && trackIndex >= 0 &&
                !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                AMediaMuxer_writeSampleData(m_muxer, trackIndex, outBuf, &info);
            }
            AMediaCodec_releaseOutputBuffer(codec, status, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        } else {
            break;
        }
    }
}

void InternalRecorder::stopRecording() {
    if (!m_recording.exchange(false)) return;

    removeFMODTap();

    if (m_videoCodec) {
        AMediaCodec_signalEndOfInputStream(m_videoCodec);
        drainEncoder(m_videoCodec, m_videoTrackIndex, true);
        AMediaCodec_stop(m_videoCodec);
        AMediaCodec_delete(m_videoCodec);
        m_videoCodec = nullptr;
    }

    if (m_audioCodec) {
        AMediaCodec_stop(m_audioCodec);
        AMediaCodec_delete(m_audioCodec);
        m_audioCodec = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_muxerMutex);
        if (m_muxer) {
            if (m_muxerStarted) AMediaMuxer_stop(m_muxer);
            AMediaMuxer_delete(m_muxer);
            m_muxer = nullptr;
        }
    }

    if (m_encoderSurface != EGL_NO_SURFACE) {
        eglDestroySurface(m_eglDisplay, m_encoderSurface);
        m_encoderSurface = EGL_NO_SURFACE;
    }
    if (m_inputSurface) {
        ANativeWindow_release(m_inputSurface);
        m_inputSurface = nullptr;
    }
    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        glDeleteTextures(1, &m_fboTex);
        m_fbo = 0;
        m_fboTex = 0;
    }

    if (m_overlay) m_overlay->updateVisualState();
    Notification::create("Saved MP4 Recording!", NotificationIcon::Info)->show();
}

#endif

RecorderOverlay* RecorderOverlay::create() {
    auto ret = new RecorderOverlay();
    if (ret && ret->init()) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

bool RecorderOverlay::init() {
    if (!CCLayer::init()) return false;

    auto winSize = CCDirector::sharedDirector()->getWinSize();
    this->setPosition({ winSize.width - 42.f, winSize.height - 24.f });
    this->setTouchEnabled(true);
    this->setTouchMode(kCCTouchesOneByOne);
    this->setTouchPriority(-500);

    auto bg = CCScale9Sprite::create("square02_small.png");
    bg->setContentSize({ 68.f, 24.f });
    bg->setOpacity(140);
    this->addChild(bg, 0);

    m_icon = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
    m_icon->setScale(0.28f);
    m_icon->setPosition({ -18.f, 0.f });
    this->addChild(m_icon, 1);

    m_timerLabel = CCLabelBMFont::create("REC", "bigFont.fnt");
    m_timerLabel->setScale(0.32f);
    m_timerLabel->setPosition({ 10.f, 0.f });
    this->addChild(m_timerLabel, 1);

    this->scheduleUpdate();
    updateVisualState();
    return true;
}

void RecorderOverlay::updateVisualState() {
    bool rec = InternalRecorder::get().isRecording();
    m_elapsedSec = 0.f;

    if (rec) {
        m_icon->setDisplayFrame(
            CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName("GJ_stopEditorBtn_001.png")
        );
        m_timerLabel->setString("0:00");
        m_timerLabel->setColor({ 255, 90, 90 });
    } else {
        m_icon->setDisplayFrame(
            CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName("GJ_playBtn2_001.png")
        );
        m_timerLabel->setString("REC");
        m_timerLabel->setColor({ 255, 255, 255 });
    }
}

void RecorderOverlay::update(float dt) {
    this->setVisible(Mod::get()->getSettingValue<bool>("show-overlay"));
    if (InternalRecorder::get().isRecording()) {
        m_elapsedSec += dt;
        int total = static_cast<int>(m_elapsedSec);
        m_timerLabel->setString(fmt::format("{}:{:02d}", total / 60, total % 60).c_str());
    }
}

void RecorderOverlay::onToggleRecord(CCObject*) {
#ifdef GEODE_IS_ANDROID
    if (InternalRecorder::get().isRecording()) {
        InternalRecorder::get().stopRecording();
    } else {
        InternalRecorder::get().startRecording();
    }
#endif
}

bool RecorderOverlay::ccTouchBegan(CCTouch* touch, CCEvent*) {
    if (!this->isVisible()) return false;
    CCPoint local = this->convertTouchToNodeSpace(touch);
    CCRect bounds(-34.f, -12.f, 68.f, 24.f);
    if (bounds.containsPoint(local)) {
        m_touchStartPos = touch->getLocation();
        m_nodeStartPos = this->getPosition();
        m_dragging = false;
        return true;
    }
    return false;
}

void RecorderOverlay::ccTouchMoved(CCTouch* touch, CCEvent*) {
    CCPoint delta = touch->getLocation() - m_touchStartPos;
    if (delta.getLength() > 6.f) {
        m_dragging = true;
        this->setPosition(m_nodeStartPos + delta);
    }
}

void RecorderOverlay::ccTouchEnded(CCTouch*, CCEvent*) {
    if (!m_dragging) {
        onToggleRecord(nullptr);
    }
}

$on_mod(Loaded) {
    Loader::get()->queueInMainThread([] {
        auto overlay = RecorderOverlay::create();
        overlay->setZOrder(9999);
        OverlayManager::get()->addChild(overlay);
        InternalRecorder::get().m_overlay = overlay;
    });
}

class $modify(RecorderEGLHook, CCEGLView) {
    void swapBuffers() {
#ifdef GEODE_IS_ANDROID
        InternalRecorder::get().captureFrame();
#endif
        CCEGLView::swapBuffers();
    }
};

class $modify(RecorderPauseHook, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto sprite = CCSprite::createWithSpriteFrameName(
            InternalRecorder::get().isRecording() ? "GJ_stopEditorBtn_001.png" : "GJ_playBtn2_001.png"
        );
        sprite->setScale(0.45f);

        auto btn = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(RecorderPauseHook::onToggleRecord)
        );
        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({ winSize.width - 35.f, winSize.height - 35.f });
        this->addChild(menu, 100);
    }

    void onToggleRecord(CCObject*) {
#ifdef GEODE_IS_ANDROID
        if (InternalRecorder::get().isRecording()) {
            InternalRecorder::get().stopRecording();
        } else {
            InternalRecorder::get().startRecording();
        }
#endif
    }
};
