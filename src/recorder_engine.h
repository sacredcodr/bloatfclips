#pragma once

#include "audio_engine.h"
#include "config.h"
#include "ring_buffer.h"
#include "video_pipeline.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace clipper {

enum class EngineState {
    Stopped,
    Starting,
    Running,
    Recovering,
    Stopping,
    Faulted
};

struct SaveResult {
    bool ok = false;
    std::wstring path;
    std::string error;
    double saveMs = 0.0;
    uint32_t videoFrames = 0;
    uint32_t audioFrames = 0;
};

class RecorderEngine {
public:
    using SaveCallback = std::function<void(const SaveResult&)>;

    explicit RecorderEngine(AppConfig cfg);
    ~RecorderEngine();

    RecorderEngine(const RecorderEngine&) = delete;
    RecorderEngine& operator=(const RecorderEngine&) = delete;

    bool Start(std::string& errOut);
    void Stop();

    EngineState State() const;
    void RequestSaveClip(const std::wstring& customPath = L"");
    SaveResult SaveClipSync(const std::wstring& customPath = L"");

    void SetMicEnabled(bool enabled);
    bool IsMicEnabled() const;
    void UpdateSaveSettings(const std::wstring& outputDir, bool saveChime);
    const AppConfig& Config() const { return cfg_; }

    void SetSaveCallback(SaveCallback cb);
    VideoStats GetVideoStats() const { return video_.Stats(); }
    AudioStats GetAudioStats() const { return audio_.Stats(); }

    void SetCollectVideoSubmitSamples(bool enable) { video_.SetCollectSubmitSamples(enable); }
    std::vector<double> TakeVideoSubmitSamplesUs() { return video_.TakeSubmitSamplesUs(); }

private:
    struct PendingSave {
        int64_t requestPtsUs = 0;
        std::wstring customPath;
    };

    void SaverThreadMain();
    SaveResult ExecuteSave(int64_t requestPtsUs, const std::wstring& customPath);
    std::wstring MakeDefaultClipPath() const;

    AppConfig cfg_;
    mutable std::mutex lifecycleMutex_;
    std::atomic<EngineState> state_{ EngineState::Stopped };
    std::atomic<bool> stoppingWait_{ false };
    int64_t epochQpc_ = 0;

    PacketRingBuffer videoRing_;
    PacketRingBuffer audioRing_;
    VideoPipeline video_;
    AudioEngine audio_;

    HANDLE stopEvent_ = nullptr;
    HANDLE saveEvent_ = nullptr;
    std::thread saverThread_;

    mutable std::mutex saveMutex_;
    std::deque<PendingSave> pendingSaves_;
    SaveCallback saveCallback_;
};

}
