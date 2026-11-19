#include "audio_engine.h"

#include "clock.h"
#include "mp4_muxer.h"

#include <audioclient.h>
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace clipper {

namespace {

constexpr uint32_t kOutRate = 48000;
constexpr uint32_t kOutChannels = 2;
constexpr uint32_t kAacFrameSamples = 1024;
constexpr uint32_t kAacBitrateBps = 192000;
constexpr int64_t kAacFrameDurUs = 21333;
constexpr int64_t kLeadMarginUs = 25000;

template <size_t CapFrames>
class StereoFloatFifo {
public:
    size_t Frames() const { return count_; }

    void Clear() {
        head_ = 0;
        count_ = 0;
    }

    void Push(const float* lrInterleaved, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            if (count_ == CapFrames) {
                head_ = (head_ + 1) % CapFrames;
                count_--;
            }
            size_t tail = (head_ + count_) % CapFrames;
            buf_[tail * 2] = lrInterleaved[i * 2];
            buf_[tail * 2 + 1] = lrInterleaved[i * 2 + 1];
            count_++;
        }
    }

    void DropOldest(size_t n) {
        size_t drop = std::min(n, count_);
        head_ = (head_ + drop) % CapFrames;
        count_ -= drop;
    }

    size_t Pop(float* outLr, size_t maxFrames) {
        size_t take = std::min(maxFrames, count_);
        for (size_t i = 0; i < take; ++i) {
            size_t idx = (head_ + i) % CapFrames;
            outLr[i * 2] = buf_[idx * 2];
            outLr[i * 2 + 1] = buf_[idx * 2 + 1];
        }
        head_ = (head_ + take) % CapFrames;
        count_ -= take;
        return take;
    }

private:
    std::array<float, CapFrames * 2> buf_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

class WasapiSource {
public:
    explicit WasapiSource(bool isLoopback) : isLoopback_(isLoopback) {}
    ~WasapiSource() { Close(); }

    bool IsOpen() const { return client_ != nullptr && capture_ != nullptr; }

    bool TryOpen(int64_t nowUs) {
        if (IsOpen()) return true;
        if (nowUs < nextRetryUs_) return false;

        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            nextRetryUs_ = nowUs + 1'000'000;
            return false;
        }

        Microsoft::WRL::ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(isLoopback_ ? eRender : eCapture, eConsole, &device);
        if (FAILED(hr)) {
            nextRetryUs_ = nowUs + 1'000'000;
            return false;
        }

        Microsoft::WRL::ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) {
            nextRetryUs_ = nowUs + 1'000'000;
            return false;
        }

        WAVEFORMATEX* pwfx = nullptr;
        hr = client->GetMixFormat(&pwfx);
        if (FAILED(hr) || !pwfx) {
            nextRetryUs_ = nowUs + 1'000'000;
            return false;
        }

        bool formatSupported = ParseFormat(pwfx);
        DWORD flags = isLoopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        if (formatSupported) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1'000'000, 0, pwfx, nullptr);
        } else {
            hr = E_FAIL;
        }
        CoTaskMemFree(pwfx);
        if (FAILED(hr)) {
            nextRetryUs_ = nowUs + 1'000'000;
            return false;
        }

        Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) {
            nextRetryUs_ = nowUs + 1'000'000;
            return false;
        }

        hr = client->Start();
        if (FAILED(hr)) {
            nextRetryUs_ = nowUs + 1'000'000;
            return false;
        }

        client_ = client;
        capture_ = capture;
        resamplePhase_ = 0.0;
        prevL_ = 0.0f;
        prevR_ = 0.0f;
        return true;
    }

    void Close() {
        if (client_) client_->Stop();
        capture_.Reset();
        client_.Reset();
    }

    template <size_t CapFrames>
    void Poll(int64_t nowUs, bool keepAudio, StereoFloatFifo<CapFrames>& fifo, uint64_t& packetsOut, uint64_t& reopensOut) {
        if (!TryOpen(nowUs)) return;

        while (true) {
            UINT32 packetFrames = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packetFrames);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING || FAILED(hr)) {
                Close();
                reopensOut++;
                nextRetryUs_ = nowUs + 1'000'000;
                return;
            }
            if (packetFrames == 0) break;

            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING || FAILED(hr)) {
                Close();
                reopensOut++;
                nextRetryUs_ = nowUs + 1'000'000;
                return;
            }

            if (numFrames > 0 && keepAudio) {
                packetsOut++;
                ConvertAndPush(pData, numFrames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, fifo);
            }
            capture_->ReleaseBuffer(numFrames);
        }

        if (fifo.Frames() > 3840) {
            fifo.DropOldest(fifo.Frames() - 1440);
        }
    }

private:
    bool ParseFormat(const WAVEFORMATEX* wfx) {
        srcRate_ = wfx->nSamplesPerSec;
        srcChannels_ = wfx->nChannels;
        srcBits_ = wfx->wBitsPerSample;
        srcFloat_ = false;

        if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            srcFloat_ = (srcBits_ == 32);
        } else if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
            srcFloat_ = false;
        } else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
            if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                srcFloat_ = (srcBits_ == 32);
            } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
                srcFloat_ = false;
            } else {
                return false;
            }
        } else {
            return false;
        }

        if (srcRate_ == 0 || srcChannels_ == 0) return false;
        return srcFloat_ || srcBits_ == 16 || srcBits_ == 32;
    }

    template <size_t CapFrames>
    void ConvertAndPush(const BYTE* data, UINT32 numFrames, bool silent, StereoFloatFifo<CapFrames>& fifo) {
        scratchIn_.resize(static_cast<size_t>(numFrames) * 2);
        if (silent || !data) {
            std::fill(scratchIn_.begin(), scratchIn_.end(), 0.0f);
        } else if (srcFloat_) {
            const float* f = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < numFrames; ++i) {
                const float* fr = f + static_cast<size_t>(i) * srcChannels_;
                float l = std::clamp(fr[0], -1.0f, 1.0f);
                float r = (srcChannels_ >= 2) ? std::clamp(fr[1], -1.0f, 1.0f) : l;
                scratchIn_[i * 2] = l;
                scratchIn_[i * 2 + 1] = r;
            }
        } else if (srcBits_ == 16) {
            const int16_t* s = reinterpret_cast<const int16_t*>(data);
            for (UINT32 i = 0; i < numFrames; ++i) {
                const int16_t* fr = s + static_cast<size_t>(i) * srcChannels_;
                float l = std::clamp(static_cast<float>(fr[0]) / 32768.0f, -1.0f, 1.0f);
                float r = (srcChannels_ >= 2) ? std::clamp(static_cast<float>(fr[1]) / 32768.0f, -1.0f, 1.0f) : l;
                scratchIn_[i * 2] = l;
                scratchIn_[i * 2 + 1] = r;
            }
        } else if (srcBits_ == 32) {
            const int32_t* s = reinterpret_cast<const int32_t*>(data);
            for (UINT32 i = 0; i < numFrames; ++i) {
                const int32_t* fr = s + static_cast<size_t>(i) * srcChannels_;
                float l = std::clamp(static_cast<float>(static_cast<double>(fr[0]) / 2147483648.0), -1.0f, 1.0f);
                float r = (srcChannels_ >= 2) ? std::clamp(static_cast<float>(static_cast<double>(fr[1]) / 2147483648.0), -1.0f, 1.0f) : l;
                scratchIn_[i * 2] = l;
                scratchIn_[i * 2 + 1] = r;
            }
        }

        if (srcRate_ == kOutRate) {
            fifo.Push(scratchIn_.data(), numFrames);
            prevL_ = scratchIn_[(numFrames - 1) * 2];
            prevR_ = scratchIn_[(numFrames - 1) * 2 + 1];
            return;
        }

        double ratio = static_cast<double>(srcRate_) / static_cast<double>(kOutRate);
        scratchOut_.clear();
        scratchOut_.reserve(static_cast<size_t>(std::ceil(numFrames / ratio) + 4) * 2);

        double pos = resamplePhase_;
        while (pos < static_cast<double>(numFrames)) {
            size_t idx0 = static_cast<size_t>(pos);
            double frac = pos - static_cast<double>(idx0);

            float l0 = (idx0 == 0) ? prevL_ : scratchIn_[(idx0 - 1) * 2];
            float r0 = (idx0 == 0) ? prevR_ : scratchIn_[(idx0 - 1) * 2 + 1];
            float l1 = scratchIn_[idx0 * 2];
            float r1 = scratchIn_[idx0 * 2 + 1];

            scratchOut_.push_back(l0 + static_cast<float>(frac) * (l1 - l0));
            scratchOut_.push_back(r0 + static_cast<float>(frac) * (r1 - r0));
            pos += ratio;
        }
        resamplePhase_ = pos - static_cast<double>(numFrames);
        prevL_ = scratchIn_[(numFrames - 1) * 2];
        prevR_ = scratchIn_[(numFrames - 1) * 2 + 1];

        if (!scratchOut_.empty()) {
            fifo.Push(scratchOut_.data(), scratchOut_.size() / 2);
        }
    }

    bool isLoopback_ = true;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    UINT32 srcRate_ = kOutRate;
    WORD srcChannels_ = 2;
    WORD srcBits_ = 32;
    bool srcFloat_ = true;
    double resamplePhase_ = 0.0;
    float prevL_ = 0.0f;
    float prevR_ = 0.0f;
    int64_t nextRetryUs_ = 0;
    std::vector<float> scratchIn_;
    std::vector<float> scratchOut_;
};

}

AudioEngine::~AudioEngine() {
    Stop();
}

bool AudioEngine::Start(const AppConfig& cfg, int64_t epochQpc, PacketRingBuffer& audioRing, std::string& errOut) {
    Stop();
    cfg_ = cfg;
    epochQpc_ = epochQpc;
    ring_ = &audioRing;
    micEnabled_.store(cfg.recordMicAudio, std::memory_order_relaxed);
    initOk_ = false;
    initErr_.clear();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stats_ = AudioStats{};
        asc_ = { 0x11, 0x90 };
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    initDoneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_ || !initDoneEvent_) {
        errOut = "CreateEventW failed";
        Stop();
        return false;
    }

    thread_ = std::thread(&AudioEngine::ThreadMain, this);
    WaitForSingleObject(initDoneEvent_, INFINITE);
    CloseHandle(initDoneEvent_);
    initDoneEvent_ = nullptr;

    if (!initOk_) {
        errOut = initErr_;
        Stop();
        return false;
    }
    return true;
}

void AudioEngine::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (initDoneEvent_) {
        CloseHandle(initDoneEvent_);
        initDoneEvent_ = nullptr;
    }
}

void AudioEngine::SetMicEnabled(bool enabled) {
    micEnabled_.store(enabled, std::memory_order_relaxed);
}

std::vector<uint8_t> AudioEngine::AudioSpecificConfig() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return asc_;
}

AudioStats AudioEngine::Stats() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return stats_;
}

void AudioEngine::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mfHr = MFStartup(MF_VERSION);
    DWORD taskIdx = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

    if (FAILED(mfHr)) {
        initErr_ = "MFStartup failed";
        initOk_ = false;
        SetEvent(initDoneEvent_);
        if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IMFTransform> mft;
    HRESULT hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft));
    if (FAILED(hr)) {
        initErr_ = "CLSID_AACMFTEncoder CoCreateInstance failed";
        initOk_ = false;
        SetEvent(initDoneEvent_);
        MFShutdown();
        if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inType, outType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kOutChannels);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kOutRate * 4);

    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kOutChannels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBitrateBps / 8);
    outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);

    if (FAILED(mft->SetInputType(0, inType.Get(), 0)) ||
        FAILED(mft->SetOutputType(0, outType.Get(), 0)) ||
        FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
        initErr_ = "AAC MFT initialization failed";
        initOk_ = false;
        SetEvent(initDoneEvent_);
        mft.Reset();
        MFShutdown();
        if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
        CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> actualOut;
    if (SUCCEEDED(mft->GetOutputCurrentType(0, &actualOut))) {
        std::vector<uint8_t> ud(64);
        UINT32 udLen = 0;
        if (SUCCEEDED(actualOut->GetBlob(MF_MT_USER_DATA, ud.data(), static_cast<UINT32>(ud.size()), &udLen))) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            asc_ = ExtractAudioSpecificConfigFromMfUserData(ud.data(), udLen);
        }
    }

    initOk_ = true;
    SetEvent(initDoneEvent_);

    WasapiSource desktopSrc(true);
    WasapiSource micSrc(false);
    StereoFloatFifo<4096> desktopFifo;
    StereoFloatFifo<4096> micFifo;

    MFT_OUTPUT_STREAM_INFO osi{};
    mft->GetOutputStreamInfo(0, &osi);
    const DWORD outBufSz = std::max<DWORD>(osi.cbSize, 4096);

    uint64_t emittedSamples = 0;
    int64_t nextAacPtsUs = 0;
    std::array<float, kAacFrameSamples * 2> dBlock{};
    std::array<float, kAacFrameSamples * 2> mBlock{};
    std::array<int16_t, kAacFrameSamples * 2> mixPcm{};

    auto drainMft = [&](int64_t pktPtsUs) {
        while (true) {
            Microsoft::WRL::ComPtr<IMFSample> outSample;
            Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf;
            MFCreateSample(&outSample);
            MFCreateMemoryBuffer(outBufSz, &outBuf);
            outSample->AddBuffer(outBuf.Get());
            MFT_OUTPUT_DATA_BUFFER odb{};
            odb.pSample = outSample.Get();
            DWORD status = 0;
            HRESULT ohr = mft->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) odb.pEvents->Release();
            if (ohr == MF_E_TRANSFORM_NEED_MORE_INPUT || ohr == MF_E_TRANSFORM_STREAM_CHANGE) break;
            if (FAILED(ohr)) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                stats_.errors++;
                break;
            }
            Microsoft::WRL::ComPtr<IMFMediaBuffer> locked;
            outSample->ConvertToContiguousBuffer(&locked);
            LONGLONG outHns = 0;
            int64_t actualPtsUs = SUCCEEDED(outSample->GetSampleTime(&outHns)) ? (outHns / 10LL) : pktPtsUs;
            BYTE* ptr = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(locked->Lock(&ptr, nullptr, &len)) && len > 0) {
                ring_->Push(ptr, len, actualPtsUs, static_cast<uint32_t>(kAacFrameDurUs), true);
                std::lock_guard<std::mutex> lock(stateMutex_);
                stats_.aacFramesEncoded++;
            }
            if (locked) locked->Unlock();
        }
    };

    while (WaitForSingleObject(stopEvent_, 10) == WAIT_TIMEOUT) {
        int64_t nowUs = QpcDeltaToUs(NowQpcTicks() - epochQpc_);
        uint64_t dPkts = 0, mPkts = 0, reopens = 0;

        if (cfg_.recordDesktopAudio) {
            desktopSrc.Poll(nowUs, true, desktopFifo, dPkts, reopens);
        }
        bool micOn = micEnabled_.load(std::memory_order_relaxed);
        if (cfg_.recordMicAudio) {
            micSrc.Poll(nowUs, micOn, micFifo, mPkts, reopens);
            if (!micOn && micFifo.Frames() > 0) micFifo.Clear();
        }

        if (dPkts > 0 || mPkts > 0 || reopens > 0) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            stats_.desktopPackets += dPkts;
            stats_.micPackets += mPkts;
            stats_.deviceReopens += reopens;
            stats_.desktopActive = desktopSrc.IsOpen();
            stats_.micActive = micOn && micSrc.IsOpen();
        }

        while (nowUs - kLeadMarginUs >= nextAacPtsUs + kAacFrameDurUs) {
            dBlock.fill(0.0f);
            mBlock.fill(0.0f);
            if (cfg_.recordDesktopAudio) {
                desktopFifo.Pop(dBlock.data(), kAacFrameSamples);
            }
            if (cfg_.recordMicAudio && micOn) {
                micFifo.Pop(mBlock.data(), kAacFrameSamples);
            }

            for (size_t i = 0; i < kAacFrameSamples * 2; ++i) {
                float summed = std::clamp(dBlock[i] + mBlock[i], -1.0f, 1.0f);
                mixPcm[i] = static_cast<int16_t>(std::lround(summed * 32767.0f));
            }

            Microsoft::WRL::ComPtr<IMFSample> inSample;
            Microsoft::WRL::ComPtr<IMFMediaBuffer> inBuf;
            const DWORD byteLen = kAacFrameSamples * 4;
            MFCreateSample(&inSample);
            MFCreateMemoryBuffer(byteLen, &inBuf);
            BYTE* dst = nullptr;
            inBuf->Lock(&dst, nullptr, nullptr);
            std::memcpy(dst, mixPcm.data(), byteLen);
            inBuf->Unlock();
            inBuf->SetCurrentLength(byteLen);
            inSample->AddBuffer(inBuf.Get());

            LONGLONG sampleTimeHns = (static_cast<LONGLONG>(emittedSamples) * 10'000'000LL) / kOutRate;
            LONGLONG sampleDurHns = (static_cast<LONGLONG>(kAacFrameSamples) * 10'000'000LL) / kOutRate;
            inSample->SetSampleTime(sampleTimeHns);
            inSample->SetSampleDuration(sampleDurHns);

            HRESULT phr = mft->ProcessInput(0, inSample.Get(), 0);
            if (phr == MF_E_NOTACCEPTING) {
                drainMft(nextAacPtsUs);
                phr = mft->ProcessInput(0, inSample.Get(), 0);
            }
            if (FAILED(phr)) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                stats_.errors++;
            } else {
                drainMft(nextAacPtsUs);
            }

            emittedSamples += kAacFrameSamples;
            nextAacPtsUs = (static_cast<int64_t>(emittedSamples) * 1'000'000LL) / kOutRate;
        }
    }

    mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    drainMft(nextAacPtsUs);

    desktopSrc.Close();
    micSrc.Close();
    mft.Reset();
    MFShutdown();
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
    CoUninitialize();
}

}
