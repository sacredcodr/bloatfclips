#include "recorder_engine.h"

#include "clock.h"
#include "mp4_muxer.h"

#include <cstdio>
#include <filesystem>

namespace clipper {

RecorderEngine::RecorderEngine(AppConfig cfg)
    : cfg_(std::move(cfg)),
      videoRing_(cfg_.VideoArenaBytes(), cfg_.VideoMaxSlots()),
      audioRing_(cfg_.AudioArenaBytes(), cfg_.AudioMaxSlots()) {}

RecorderEngine::~RecorderEngine() {
    Stop();
}

bool RecorderEngine::Start(std::string& errOut) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    EngineState cur = state_.load(std::memory_order_acquire);
    if (cur != EngineState::Stopped && cur != EngineState::Faulted) {
        errOut = "engine already active";
        return false;
    }

    stoppingWait_.store(false, std::memory_order_release);
    state_.store(EngineState::Starting, std::memory_order_release);
    videoRing_.Clear();
    audioRing_.Clear();
    epochQpc_ = NowQpcTicks();

    if (!video_.Start(cfg_, epochQpc_, videoRing_, errOut)) {
        state_.store(EngineState::Faulted, std::memory_order_release);
        return false;
    }

    if (cfg_.recordDesktopAudio || cfg_.recordMicAudio) {
        if (!audio_.Start(cfg_, epochQpc_, audioRing_, errOut)) {
            video_.Stop();
            state_.store(EngineState::Faulted, std::memory_order_release);
            return false;
        }
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    saveEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stopEvent_ || !saveEvent_) {
        errOut = "CreateEventW failed";
        audio_.Stop();
        video_.Stop();
        if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
        if (saveEvent_) { CloseHandle(saveEvent_); saveEvent_ = nullptr; }
        state_.store(EngineState::Faulted, std::memory_order_release);
        return false;
    }

    state_.store(EngineState::Running, std::memory_order_release);
    saverThread_ = std::thread(&RecorderEngine::SaverThreadMain, this);
    return true;
}

void RecorderEngine::Stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    EngineState cur = state_.load(std::memory_order_acquire);
    if (cur == EngineState::Stopped) return;
    if (cur == EngineState::Faulted) {
        state_.store(EngineState::Stopped, std::memory_order_release);
        return;
    }

    state_.store(EngineState::Stopping, std::memory_order_release);
    stoppingWait_.store(true, std::memory_order_release);
    videoRing_.NotifyAll();
    audioRing_.NotifyAll();
    if (stopEvent_) SetEvent(stopEvent_);
    if (saverThread_.joinable()) saverThread_.join();
    {
        std::lock_guard<std::mutex> lock(saveMutex_);
        if (stopEvent_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
        if (saveEvent_) {
            CloseHandle(saveEvent_);
            saveEvent_ = nullptr;
        }
        pendingSaves_.clear();
    }
    audio_.Stop();
    video_.Stop();
    state_.store(EngineState::Stopped, std::memory_order_release);
}

EngineState RecorderEngine::State() const {
    EngineState base = state_.load(std::memory_order_acquire);
    if (base == EngineState::Running && video_.Stats().recovering) {
        return EngineState::Recovering;
    }
    return base;
}

void RecorderEngine::RequestSaveClip(const std::wstring& customPath) {
    std::lock_guard<std::mutex> lock(saveMutex_);
    if (state_.load(std::memory_order_acquire) != EngineState::Running || !saveEvent_) return;
    int64_t ptsUs = QpcDeltaToUs(NowQpcTicks() - epochQpc_);
    pendingSaves_.push_back(PendingSave{ ptsUs, customPath });
    SetEvent(saveEvent_);
}

SaveResult RecorderEngine::SaveClipSync(const std::wstring& customPath) {
    EngineState st = state_.load(std::memory_order_acquire);
    if (st != EngineState::Running && st != EngineState::Recovering) {
        SaveResult res;
        res.error = "engine is not running";
        return res;
    }
    int64_t reqPtsUs = QpcDeltaToUs(NowQpcTicks() - epochQpc_);
    return ExecuteSave(reqPtsUs, customPath);
}

void RecorderEngine::SetMicEnabled(bool enabled) {
    audio_.SetMicEnabled(enabled);
}

bool RecorderEngine::IsMicEnabled() const {
    return audio_.IsMicEnabled();
}

void RecorderEngine::SetSaveCallback(SaveCallback cb) {
    std::lock_guard<std::mutex> lock(saveMutex_);
    saveCallback_ = std::move(cb);
}

std::wstring RecorderEngine::MakeDefaultClipPath() const {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[96];
    std::swprintf(name, sizeof(name) / sizeof(wchar_t),
        L"clip_%04u%02u%02u_%02u%02u%02u_%03u.mp4",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return (std::filesystem::path(cfg_.outputDir) / name).wstring();
}

SaveResult RecorderEngine::ExecuteSave(int64_t requestPtsUs, const std::wstring& customPath) {
    SaveResult res;
    videoRing_.WaitForPts(requestPtsUs, stoppingWait_, 60);
    if (cfg_.recordDesktopAudio || cfg_.recordMicAudio) {
        audioRing_.WaitForPts(requestPtsUs - 25000, stoppingWait_, 45);
    }

    int64_t t0 = NowQpcTicks();
    VideoStats vs = video_.Stats();
    int64_t windowUs = static_cast<int64_t>(cfg_.replaySeconds) * 1'000'000LL;

    Mp4MuxInput muxIn;
    muxIn.width = vs.width;
    muxIn.height = vs.height;
    muxIn.video = videoRing_.SnapshotVideoWindow(windowUs, requestPtsUs);
    if (muxIn.video.Empty()) {
        res.error = "no keyframe available in video ring";
        return res;
    }

    if (cfg_.recordDesktopAudio || cfg_.recordMicAudio) {
        muxIn.audio = audioRing_.SnapshotAudioRange(muxIn.video.startPtsUs, muxIn.video.endPtsUs);
        muxIn.audioSpecificConfig = audio_.AudioSpecificConfig();
    }

    res.path = customPath.empty() ? MakeDefaultClipPath() : customPath;
    res.videoFrames = static_cast<uint32_t>(muxIn.video.packets.size());
    res.audioFrames = static_cast<uint32_t>(muxIn.audio.packets.size());

    if (!WriteMp4File(res.path, muxIn, res.error)) {
        return res;
    }

    res.saveMs = static_cast<double>(QpcDeltaToUs(NowQpcTicks() - t0)) / 1000.0;
    res.ok = true;
    return res;
}

void RecorderEngine::SaverThreadMain() {
    HANDLE hs[2] = { saveEvent_, stopEvent_ };
    while (true) {
        DWORD wr = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0 + 1) break;
        if (wr != WAIT_OBJECT_0) break;

        while (true) {
            PendingSave req;
            SaveCallback cb;
            {
                std::lock_guard<std::mutex> lock(saveMutex_);
                if (pendingSaves_.empty()) break;
                req = std::move(pendingSaves_.front());
                pendingSaves_.pop_front();
                cb = saveCallback_;
            }

            SaveResult res = ExecuteSave(req.requestPtsUs, req.customPath);
            if (res.ok && cfg_.saveChime) {
                MessageBeep(MB_OK);
            }
            if (cb) cb(res);
        }
    }
}

}
