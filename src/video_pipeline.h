#pragma once

#include "config.h"
#include "ring_buffer.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nvEncodeAPI.h"

namespace clipper {

struct MonitorInfo {
    uint32_t index = 0;
    std::wstring deviceName;
    std::wstring friendlyName;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refreshHz = 0;
    bool isPrimary = false;
};

std::vector<MonitorInfo> EnumerateCaptureMonitors();

struct VideoStats {
    uint64_t framesCaptured = 0;
    uint64_t framesEncoded = 0;
    uint64_t framesStatic = 0;
    uint64_t recoveries = 0;
    uint64_t errors = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool recovering = false;
};

class VideoPipeline {
public:
    VideoPipeline() = default;
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    bool Start(const AppConfig& cfg, int64_t epochQpc, PacketRingBuffer& videoRing, std::string& errOut);
    void Stop();

    VideoStats Stats() const;
    void SetCollectSubmitSamples(bool enable);
    std::vector<double> TakeSubmitSamplesUs();

private:
    static constexpr int kPoolSize = 4;

    struct EncodeSlot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Tex;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> vpOut;
        NV_ENC_REGISTERED_PTR registeredRes = nullptr;
        NV_ENC_INPUT_PTR mappedRes = nullptr;
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        HANDLE eventHandle = nullptr;
        bool inFlight = false;
        int64_t ptsUs = 0;
        uint32_t durationUs = 0;
    };

    void ThreadMain();
    bool InitAllResources(std::string& errOut);
    bool CreateDuplication(UINT& outWidth, UINT& outHeight, HRESULT& outHr);
    bool InitVideoProcessorAndTextures(UINT width, UINT height, std::string& errOut);
    bool InitNvenc(UINT width, UINT height, std::string& errOut);
    bool RetireSlot(int idx, bool draining);
    void DrainAllSlots();
    bool RecoverCapture(HRESULT ddaHr, bool forceFullReset, bool& forceNextIdr, int64_t& paceStartUs, uint64_t& paceTick);
    void DestroyNvencAndTextures();
    void DestroyAllResources();
    void RebindInputDesktop();

    AppConfig cfg_{};
    int64_t epochQpc_ = 0;
    PacketRingBuffer* ring_ = nullptr;

    HANDLE stopEvent_ = nullptr;
    HANDLE initDoneEvent_ = nullptr;
    bool initOk_ = false;
    std::string initErr_;
    std::thread thread_;

    HDESK initialDesktop_ = nullptr;
    HDESK ownedDesktop_ = nullptr;
    uint32_t encFrameIdx_ = 0;
    bool encoderInitialized_ = false;
    bool eosSent_ = false;
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoCtx_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoCtx1_;
    Microsoft::WRL::ComPtr<IDXGIOutput> targetOutput_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> lastFrameTex_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> vpInView_;

    HMODULE nvDll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST nv_{};
    void* encoder_ = nullptr;
    EncodeSlot slots_[kPoolSize]{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    mutable std::mutex statsMutex_;
    VideoStats stats_{};
    std::atomic<bool> collectSamples_{ false };
    std::vector<double> submitSamplesUs_;
};

}
