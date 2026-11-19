#pragma once

#include "config.h"
#include "ring_buffer.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace clipper {

struct AudioStats {
    uint64_t aacFramesEncoded = 0;
    uint64_t desktopPackets = 0;
    uint64_t micPackets = 0;
    uint64_t deviceReopens = 0;
    uint64_t errors = 0;
    bool desktopActive = false;
    bool micActive = false;
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Start(const AppConfig& cfg, int64_t epochQpc, PacketRingBuffer& audioRing, std::string& errOut);
    void Stop();

    void SetMicEnabled(bool enabled);
    bool IsMicEnabled() const { return micEnabled_.load(std::memory_order_relaxed); }

    std::vector<uint8_t> AudioSpecificConfig() const;
    AudioStats Stats() const;

private:
    void ThreadMain();

    AppConfig cfg_{};
    int64_t epochQpc_ = 0;
    PacketRingBuffer* ring_ = nullptr;

    HANDLE stopEvent_ = nullptr;
    HANDLE initDoneEvent_ = nullptr;
    bool initOk_ = false;
    std::string initErr_;
    std::thread thread_;

    std::atomic<bool> micEnabled_{ true };
    mutable std::mutex stateMutex_;
    std::vector<uint8_t> asc_{ 0x11, 0x90 };
    AudioStats stats_{};
};

}
