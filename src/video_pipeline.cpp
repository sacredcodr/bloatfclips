#include "video_pipeline.h"

#include "clock.h"

#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clipper {

namespace {

std::string HexHr(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

struct DxgiOutputCandidate {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
    UINT vendorId = 0;
};

bool EnumeratePreferredDxgiOutputs(std::vector<DxgiOutputCandidate>& outList, HRESULT* outHr = nullptr) {
    outList.clear();
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (outHr) *outHr = hr;
    if (FAILED(hr)) return false;

    std::vector<DxgiOutputCandidate> nvOutputs;
    std::vector<DxgiOutputCandidate> allOutputs;

    for (UINT ai = 0;; ++ai) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        for (UINT oi = 0;; ++oi) {
            Microsoft::WRL::ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;
            DxgiOutputCandidate c{ adapter, out, od, ad.VendorId };
            if (ad.VendorId == 0x10DE) nvOutputs.push_back(c);
            allOutputs.push_back(std::move(c));
        }
    }

    outList = !nvOutputs.empty() ? std::move(nvOutputs) : std::move(allOutputs);
    return true;
}

std::wstring LookupFriendlyMonitorName(const wchar_t* gdiDeviceName,
                                       const std::vector<DISPLAYCONFIG_PATH_INFO>& paths) {
    if (!gdiDeviceName || !gdiDeviceName[0]) return {};
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = path.sourceInfo.adapterId;
        srcName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS) continue;
        if (wcscmp(srcName.viewGdiDeviceName, gdiDeviceName) != 0) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tgtName{};
        tgtName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgtName.header.size = sizeof(tgtName);
        tgtName.header.adapterId = path.targetInfo.adapterId;
        tgtName.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgtName.header) == ERROR_SUCCESS &&
            tgtName.monitorFriendlyDeviceName[0] != L'\0') {
            return tgtName.monitorFriendlyDeviceName;
        }
    }

    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(gdiDeviceName, 0, &dd, 0) && dd.DeviceString[0] != L'\0') {
        return dd.DeviceString;
    }
    return {};
}

}

std::vector<MonitorInfo> EnumerateCaptureMonitors() {
    std::vector<MonitorInfo> result;
    std::vector<DxgiOutputCandidate> candidates;
    if (!EnumeratePreferredDxgiOutputs(candidates) || candidates.empty()) return result;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) == ERROR_SUCCESS &&
        numPaths > 0) {
        paths.resize(numPaths);
        modes.resize(numModes);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr) == ERROR_SUCCESS) {
            paths.resize(numPaths);
        } else {
            paths.clear();
        }
    }

    result.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        MonitorInfo info;
        info.index = static_cast<uint32_t>(i);
        info.deviceName = c.desc.DeviceName;
        info.friendlyName = LookupFriendlyMonitorName(c.desc.DeviceName, paths);
        if (info.friendlyName.empty()) info.friendlyName = info.deviceName;

        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(c.desc.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
            info.width = dm.dmPelsWidth;
            info.height = dm.dmPelsHeight;
            info.refreshHz = dm.dmDisplayFrequency;
        } else {
            LONG w = c.desc.DesktopCoordinates.right - c.desc.DesktopCoordinates.left;
            LONG h = c.desc.DesktopCoordinates.bottom - c.desc.DesktopCoordinates.top;
            info.width = w > 0 ? static_cast<uint32_t>(w) : 0u;
            info.height = h > 0 ? static_cast<uint32_t>(h) : 0u;
        }

        if (c.desc.Monitor) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(c.desc.Monitor, &mi)) {
                info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}

VideoPipeline::~VideoPipeline() {
    Stop();
}

bool VideoPipeline::Start(const AppConfig& cfg, int64_t epochQpc, PacketRingBuffer& videoRing, std::string& errOut) {
    Stop();
    cfg_ = cfg;
    epochQpc_ = epochQpc;
    ring_ = &videoRing;
    initOk_ = false;
    initErr_.clear();
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = VideoStats{};
        submitSamplesUs_.clear();
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    initDoneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_ || !initDoneEvent_) {
        errOut = "CreateEventW failed";
        Stop();
        return false;
    }

    thread_ = std::thread(&VideoPipeline::ThreadMain, this);
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

void VideoPipeline::Stop() {
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

VideoStats VideoPipeline::Stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void VideoPipeline::SetCollectSubmitSamples(bool enable) {
    collectSamples_.store(enable, std::memory_order_relaxed);
    if (!enable) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        submitSamplesUs_.clear();
    }
}

std::vector<double> VideoPipeline::TakeSubmitSamplesUs() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    std::vector<double> out;
    out.swap(submitSamplesUs_);
    return out;
}

void VideoPipeline::RebindInputDesktop() {
    HDESK next = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP | GENERIC_READ);
    if (!next) {
        next = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
    }
    if (!next) return;
    if (SetThreadDesktop(next)) {
        if (ownedDesktop_ && ownedDesktop_ != next) {
            CloseDesktop(ownedDesktop_);
        }
        ownedDesktop_ = next;
    } else {
        CloseDesktop(next);
    }
}

bool VideoPipeline::CreateDuplication(UINT& outWidth, UINT& outHeight, HRESULT& outHr) {
    duplication_.Reset();
    outHr = E_FAIL;
    if (!targetOutput_) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput5> out5;
    if (SUCCEEDED(targetOutput_.As(&out5))) {
        DXGI_FORMAT fmts[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
        outHr = out5->DuplicateOutput1(dev_.Get(), 0, 1, fmts, &duplication_);
    } else {
        Microsoft::WRL::ComPtr<IDXGIOutput1> out1;
        if (SUCCEEDED(targetOutput_.As(&out1))) {
            outHr = out1->DuplicateOutput(dev_.Get(), &duplication_);
        }
    }
    if (FAILED(outHr) || !duplication_) return false;

    DXGI_OUTDUPL_DESC desc{};
    duplication_->GetDesc(&desc);
    outWidth = desc.ModeDesc.Width;
    outHeight = desc.ModeDesc.Height;
    if (outWidth == 0 || outHeight == 0) {
        DXGI_OUTPUT_DESC od{};
        targetOutput_->GetDesc(&od);
        outWidth = static_cast<UINT>(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
        outHeight = static_cast<UINT>(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
    }
    return outWidth > 0 && outHeight > 0;
}

bool VideoPipeline::InitVideoProcessorAndTextures(UINT width, UINT height, std::string& errOut) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &lastFrameTex_);
    if (FAILED(hr)) {
        errOut = "CreateTexture2D lastFrame failed: " + HexHr(hr);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (SUCCEEDED(dev_->CreateRenderTargetView(lastFrameTex_.Get(), nullptr, &rtv))) {
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx_->ClearRenderTargetView(rtv.Get(), black);
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = width;
    cd.InputHeight = height;
    cd.OutputWidth = width;
    cd.OutputHeight = height;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    hr = videoDev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
    if (FAILED(hr)) {
        errOut = "CreateVideoProcessorEnumerator failed: " + HexHr(hr);
        return false;
    }
    hr = videoDev_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
    if (FAILED(hr)) {
        errOut = "CreateVideoProcessor failed: " + HexHr(hr);
        return false;
    }

    if (videoCtx1_) {
        videoCtx1_->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        videoCtx1_->VideoProcessorSetOutputColorSpace1(vp_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    hr = videoDev_->CreateVideoProcessorInputView(lastFrameTex_.Get(), vpEnum_.Get(), &ivd, &vpInView_);
    if (FAILED(hr)) {
        errOut = "CreateVideoProcessorInputView failed: " + HexHr(hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC nv12Desc = td;
    nv12Desc.Format = DXGI_FORMAT_NV12;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

    for (int i = 0; i < kPoolSize; ++i) {
        hr = dev_->CreateTexture2D(&nv12Desc, nullptr, &slots_[i].nv12Tex);
        if (FAILED(hr)) {
            errOut = "CreateTexture2D NV12 slot failed: " + HexHr(hr);
            return false;
        }
        hr = videoDev_->CreateVideoProcessorOutputView(slots_[i].nv12Tex.Get(), vpEnum_.Get(), &ovd, &slots_[i].vpOut);
        if (FAILED(hr)) {
            errOut = "CreateVideoProcessorOutputView failed: " + HexHr(hr);
            return false;
        }
    }
    return true;
}

bool VideoPipeline::InitNvenc(UINT width, UINT height, std::string& errOut) {
    nvDll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!nvDll_) {
        errOut = "LoadLibraryW(nvEncodeAPI64.dll) failed";
        return false;
    }

    using PFN_Create = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto pCreate = reinterpret_cast<PFN_Create>(GetProcAddress(nvDll_, "NvEncodeAPICreateInstance"));
    if (!pCreate) {
        errOut = "NvEncodeAPICreateInstance symbol missing";
        return false;
    }

    std::memset(&nv_, 0, sizeof(nv_));
    nv_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (pCreate(&nv_) != NV_ENC_SUCCESS) {
        errOut = "NvEncodeAPICreateInstance failed";
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{};
    op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.device = dev_.Get();
    op.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS st = nv_.nvEncOpenEncodeSessionEx(&op, &encoder_);
    if (st != NV_ENC_SUCCESS) {
        errOut = "nvEncOpenEncodeSessionEx failed: " + std::to_string(st);
        return false;
    }

    NV_ENC_PRESET_CONFIG pcfg{};
    pcfg.version = NV_ENC_PRESET_CONFIG_VER;
    pcfg.presetCfg.version = NV_ENC_CONFIG_VER;
    st = nv_.nvEncGetEncodePresetConfigEx(encoder_, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
        NV_ENC_TUNING_INFO_LOW_LATENCY, &pcfg);
    if (st != NV_ENC_SUCCESS) {
        errOut = "nvEncGetEncodePresetConfigEx failed: " + std::to_string(st);
        return false;
    }

    uint32_t avgBitrate = cfg_.bitrateMbps * 1'000'000u;
    uint32_t maxBitrate = cfg_.bitrateMbps * 1'350'000u;

    NV_ENC_CONFIG cfg = pcfg.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.gopLength = cfg_.fps;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    cfg.rcParams.averageBitRate = avgBitrate;
    cfg.rcParams.maxBitRate = maxBitrate;
    cfg.rcParams.vbvBufferSize = maxBitrate / 2u;
    cfg.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    cfg.rcParams.enableLookahead = 0;
    cfg.rcParams.enableAQ = 0;
    cfg.rcParams.enableTemporalAQ = 0;
    cfg.rcParams.zeroReorderDelay = 1;

    auto& h264 = cfg.encodeCodecConfig.h264Config;
    h264.idrPeriod = cfg_.fps;
    h264.repeatSPSPPS = 1;
    h264.chromaFormatIDC = 1;
    h264.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    h264.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    h264.h264VUIParameters.videoSignalTypePresentFlag = 1;
    h264.h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    h264.h264VUIParameters.videoFullRangeFlag = 0;
    h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
    h264.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    h264.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    h264.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

    NV_ENC_INITIALIZE_PARAMS ip{};
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P4_GUID;
    ip.encodeWidth = width;
    ip.encodeHeight = height;
    ip.darWidth = width;
    ip.darHeight = height;
    ip.frameRateNum = cfg_.fps;
    ip.frameRateDen = 1;
    ip.enableEncodeAsync = 1;
    ip.enablePTD = 1;
    ip.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    ip.encodeConfig = &cfg;

    st = nv_.nvEncInitializeEncoder(encoder_, &ip);
    if (st != NV_ENC_SUCCESS) {
        errOut = "nvEncInitializeEncoder failed: " + std::to_string(st);
        return false;
    }

    for (int i = 0; i < kPoolSize; ++i) {
        NV_ENC_REGISTER_RESOURCE rr{};
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.width = width;
        rr.height = height;
        rr.resourceToRegister = slots_[i].nv12Tex.Get();
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (nv_.nvEncRegisterResource(encoder_, &rr) != NV_ENC_SUCCESS) {
            errOut = "nvEncRegisterResource failed";
            return false;
        }
        slots_[i].registeredRes = rr.registeredResource;

        NV_ENC_CREATE_BITSTREAM_BUFFER cbb{};
        cbb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (nv_.nvEncCreateBitstreamBuffer(encoder_, &cbb) != NV_ENC_SUCCESS) {
            errOut = "nvEncCreateBitstreamBuffer failed";
            return false;
        }
        slots_[i].bitstream = cbb.bitstreamBuffer;

        slots_[i].eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!slots_[i].eventHandle) {
            errOut = "CreateEventW failed for NVENC async event";
            return false;
        }
        NV_ENC_EVENT_PARAMS ep{};
        ep.version = NV_ENC_EVENT_PARAMS_VER;
        ep.completionEvent = slots_[i].eventHandle;
        if (nv_.nvEncRegisterAsyncEvent(encoder_, &ep) != NV_ENC_SUCCESS) {
            errOut = "nvEncRegisterAsyncEvent failed";
            return false;
        }
    }

    encFrameIdx_ = 0;
    encoderInitialized_ = false;
    eosSent_ = false;
    width_ = width;
    height_ = height;
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.width = width;
    stats_.height = height;
    return true;
}

bool VideoPipeline::InitAllResources(std::string& errOut) {
    RebindInputDesktop();

    std::vector<DxgiOutputCandidate> list;
    HRESULT enumHr = S_OK;
    if (!EnumeratePreferredDxgiOutputs(list, &enumHr)) {
        errOut = "CreateDXGIFactory1 failed: " + HexHr(enumHr);
        return false;
    }
    if (list.empty()) {
        errOut = "no attached desktop displays found";
        return false;
    }
    if (cfg_.monitorIndex >= list.size()) {
        errOut = "monitor_index out of range";
        return false;
    }

    const DxgiOutputCandidate& chosen = list[cfg_.monitorIndex];
    targetOutput_ = chosen.output;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(chosen.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &fl, 1,
        D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (FAILED(hr)) {
        errOut = "D3D11CreateDevice failed: " + HexHr(hr);
        DestroyAllResources();
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(dev_.As(&mt))) mt->SetMultithreadProtected(TRUE);
    if (FAILED(dev_.As(&videoDev_)) || FAILED(ctx_.As(&videoCtx_))) {
        errOut = "D3D11 video interfaces unavailable";
        DestroyAllResources();
        return false;
    }
    ctx_.As(&videoCtx1_);

    UINT w = 0, h = 0;
    HRESULT dhr = E_FAIL;
    if (!CreateDuplication(w, h, dhr)) {
        errOut = "DuplicateOutput1 failed: " + HexHr(dhr);
        DestroyAllResources();
        return false;
    }
    if (!InitVideoProcessorAndTextures(w, h, errOut) || !InitNvenc(w, h, errOut)) {
        DestroyAllResources();
        return false;
    }
    return true;
}

bool VideoPipeline::RetireSlot(int idx, bool draining) {
    EncodeSlot& s = slots_[idx];
    if (!s.inFlight) return true;

    DWORD waitRes = WAIT_FAILED;
    if (draining) {
        waitRes = WaitForSingleObject(s.eventHandle, 1000);
    } else {
        HANDLE waitHandles[2] = { s.eventHandle, stopEvent_ };
        waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, 2000);
    }

    if (!draining && waitRes == WAIT_OBJECT_0 + 1) {
        return false;
    }

    auto unmapSlot = [&]() {
        if (s.mappedRes && encoder_) {
            nv_.nvEncUnmapInputResource(encoder_, s.mappedRes);
            s.mappedRes = nullptr;
        }
    };

    if (waitRes != WAIT_OBJECT_0) {
        unmapSlot();
        s.inFlight = false;
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.errors++;
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lb{};
    lb.version = NV_ENC_LOCK_BITSTREAM_VER;
    lb.outputBitstream = s.bitstream;
    NVENCSTATUS st = nv_.nvEncLockBitstream(encoder_, &lb);
    if (st != NV_ENC_SUCCESS) {
        unmapSlot();
        s.inFlight = false;
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.errors++;
        return false;
    }

    bool isIdr = (lb.pictureType == NV_ENC_PIC_TYPE_IDR);
    ring_->Push(static_cast<const uint8_t*>(lb.bitstreamBufferPtr),
        lb.bitstreamSizeInBytes, s.ptsUs, s.durationUs, isIdr);
    nv_.nvEncUnlockBitstream(encoder_, s.bitstream);
    unmapSlot();
    s.inFlight = false;

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.framesEncoded++;
    return true;
}

void VideoPipeline::DrainAllSlots() {
    if (!encoder_ || !encoderInitialized_) return;
    int start = static_cast<int>(encFrameIdx_ % kPoolSize);
    for (int k = 0; k < kPoolSize; ++k) {
        int idx = (start + k) % kPoolSize;
        if (slots_[idx].inFlight) RetireSlot(idx, true);
    }
    if (!eosSent_) {
        eosSent_ = true;
        NV_ENC_PIC_PARAMS eos{};
        eos.version = NV_ENC_PIC_PARAMS_VER;
        eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        eos.completionEvent = slots_[0].eventHandle;
        if (nv_.nvEncEncodePicture(encoder_, &eos) == NV_ENC_SUCCESS && eos.completionEvent) {
            WaitForSingleObject(eos.completionEvent, 500);
        }
    }
}

void VideoPipeline::DestroyNvencAndTextures() {
    DrainAllSlots();
    encoderInitialized_ = false;
    if (encoder_) {
        for (int i = 0; i < kPoolSize; ++i) {
            if (slots_[i].mappedRes) {
                nv_.nvEncUnmapInputResource(encoder_, slots_[i].mappedRes);
                slots_[i].mappedRes = nullptr;
            }
            if (slots_[i].eventHandle) {
                NV_ENC_EVENT_PARAMS ep{};
                ep.version = NV_ENC_EVENT_PARAMS_VER;
                ep.completionEvent = slots_[i].eventHandle;
                nv_.nvEncUnregisterAsyncEvent(encoder_, &ep);
                CloseHandle(slots_[i].eventHandle);
                slots_[i].eventHandle = nullptr;
            }
            if (slots_[i].bitstream) {
                nv_.nvEncDestroyBitstreamBuffer(encoder_, slots_[i].bitstream);
                slots_[i].bitstream = nullptr;
            }
            if (slots_[i].registeredRes) {
                nv_.nvEncUnregisterResource(encoder_, slots_[i].registeredRes);
                slots_[i].registeredRes = nullptr;
            }
        }
        nv_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }
    if (nvDll_) {
        FreeLibrary(nvDll_);
        nvDll_ = nullptr;
    }
    for (int i = 0; i < kPoolSize; ++i) {
        slots_[i].vpOut.Reset();
        slots_[i].nv12Tex.Reset();
        slots_[i].inFlight = false;
        slots_[i].mappedRes = nullptr;
    }
    encFrameIdx_ = 0;
    encoderInitialized_ = false;
    eosSent_ = false;
    vpInView_.Reset();
    lastFrameTex_.Reset();
    vp_.Reset();
    vpEnum_.Reset();
}

void VideoPipeline::DestroyAllResources() {
    DestroyNvencAndTextures();
    duplication_.Reset();
    targetOutput_.Reset();
    videoCtx1_.Reset();
    videoCtx_.Reset();
    videoDev_.Reset();
    ctx_.Reset();
    dev_.Reset();
    if (ownedDesktop_) {
        if (initialDesktop_) {
            SetThreadDesktop(initialDesktop_);
        }
        CloseDesktop(ownedDesktop_);
        ownedDesktop_ = nullptr;
    }
}

bool VideoPipeline::RecoverCapture(HRESULT ddaHr, bool forceFullReset, bool& forceNextIdr,
                                   int64_t& paceStartUs, uint64_t& paceTick) {
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.recovering = true;
        stats_.recoveries++;
    }
    DrainAllSlots();
    duplication_.Reset();
    int64_t recStartUs = QpcDeltaToUs(NowQpcTicks() - epochQpc_);

    bool needFullReset = forceFullReset ||
        ddaHr == DXGI_ERROR_DEVICE_REMOVED || ddaHr == DXGI_ERROR_DEVICE_RESET;
    if (needFullReset) {
        ring_->AdvanceGeneration();
        DestroyAllResources();
    }

    static constexpr DWORD kBackoffs[] = { 100, 250, 500, 1000 };
    bool recovered = false;
    for (int attempt = 0; WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT; ++attempt) {
        std::string recErr;
        if (needFullReset) {
            if (InitAllResources(recErr)) {
                recovered = true;
                break;
            }
        } else {
            RebindInputDesktop();
            UINT nw = 0, nh = 0;
            HRESULT rhr = E_FAIL;
            if (CreateDuplication(nw, nh, rhr)) {
                if (nw != width_ || nh != height_) {
                    ring_->AdvanceGeneration();
                    DestroyNvencAndTextures();
                    if (InitVideoProcessorAndTextures(nw, nh, recErr) && InitNvenc(nw, nh, recErr)) {
                        recovered = true;
                        break;
                    }
                    DestroyAllResources();
                    needFullReset = true;
                } else {
                    recovered = true;
                    break;
                }
            } else if (rhr == DXGI_ERROR_DEVICE_REMOVED || rhr == DXGI_ERROR_DEVICE_RESET) {
                ring_->AdvanceGeneration();
                DestroyAllResources();
                needFullReset = true;
            }
        }
        DWORD ms = kBackoffs[std::min(attempt, 3)];
        if (WaitForSingleObject(stopEvent_, ms) == WAIT_OBJECT_0) break;
    }

    int64_t recEndUs = QpcDeltaToUs(NowQpcTicks() - epochQpc_);
    if (!needFullReset && (recEndUs - recStartUs) > 2'000'000LL) {
        ring_->AdvanceGeneration();
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.recovering = false;
    }
    forceNextIdr = true;
    paceStartUs = recEndUs;
    paceTick = 0;
    return recovered;
}

void VideoPipeline::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    initialDesktop_ = GetThreadDesktop(GetCurrentThreadId());

    std::string err;
    if (!InitAllResources(err)) {
        initErr_ = err;
        initOk_ = false;
        DestroyAllResources();
        SetEvent(initDoneEvent_);
        CoUninitialize();
        return;
    }

    initOk_ = true;
    SetEvent(initDoneEvent_);

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    bool usedTimePeriod = false;
    if (!timer) {
        timeBeginPeriod(1);
        usedTimePeriod = true;
    }
    const double framePeriodUs = 1'000'000.0 / static_cast<double>(cfg_.fps);
    const uint32_t nominalDurUs = static_cast<uint32_t>(std::llround(framePeriodUs));

    {
        Microsoft::WRL::ComPtr<IDXGIResource> res;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        if (duplication_->AcquireNextFrame(16, &fi, &res) == S_OK) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTex;
            if (SUCCEEDED(res.As(&desktopTex))) {
                ctx_->CopyResource(lastFrameTex_.Get(), desktopTex.Get());
            }
            duplication_->ReleaseFrame();
        }
    }

    int64_t paceStartUs = QpcDeltaToUs(NowQpcTicks() - epochQpc_);
    uint64_t paceTick = 0;
    encFrameIdx_ = 0;
    bool forceNextIdr = true;

    while (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT) {
        int64_t tStartTicks = NowQpcTicks();
        int64_t ptsUs = QpcDeltaToUs(tStartTicks - epochQpc_);

        bool needRecover = false;
        HRESULT ddaHr = S_OK;
        if (duplication_) {
            Microsoft::WRL::ComPtr<IDXGIResource> res;
            DXGI_OUTDUPL_FRAME_INFO fi{};
            ddaHr = duplication_->AcquireNextFrame(0, &fi, &res);
            if (ddaHr == S_OK) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTex;
                if (SUCCEEDED(res.As(&desktopTex))) {
                    ctx_->CopyResource(lastFrameTex_.Get(), desktopTex.Get());
                }
                duplication_->ReleaseFrame();
                std::lock_guard<std::mutex> lock(statsMutex_);
                if (fi.LastPresentTime.QuadPart != 0) stats_.framesCaptured++;
                else stats_.framesStatic++;
            } else if (ddaHr == DXGI_ERROR_WAIT_TIMEOUT) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.framesStatic++;
            } else {
                needRecover = true;
            }
        } else {
            needRecover = true;
        }

        if (needRecover) {
            if (!RecoverCapture(ddaHr, false, forceNextIdr, paceStartUs, paceTick)) break;
            continue;
        }

        int slotIdx = static_cast<int>(encFrameIdx_ % kPoolSize);
        if (slots_[slotIdx].inFlight) {
            if (!RetireSlot(slotIdx, false)) {
                if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;
                if (!RecoverCapture(E_FAIL, true, forceNextIdr, paceStartUs, paceTick)) break;
                continue;
            }
        }

        D3D11_VIDEO_PROCESSOR_STREAM strm{};
        strm.Enable = TRUE;
        strm.pInputSurface = vpInView_.Get();
        HRESULT vpHr = videoCtx_->VideoProcessorBlt(vp_.Get(), slots_[slotIdx].vpOut.Get(), 0, 1, &strm);
        if (FAILED(vpHr)) {
            {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.errors++;
            }
            if (vpHr == DXGI_ERROR_DEVICE_REMOVED || vpHr == DXGI_ERROR_DEVICE_RESET) {
                if (!RecoverCapture(vpHr, true, forceNextIdr, paceStartUs, paceTick)) break;
            }
            continue;
        }

        NV_ENC_MAP_INPUT_RESOURCE mir{};
        mir.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mir.registeredResource = slots_[slotIdx].registeredRes;
        if (nv_.nvEncMapInputResource(encoder_, &mir) != NV_ENC_SUCCESS) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.errors++;
            continue;
        }

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = width_;
        pic.inputHeight = height_;
        pic.inputPitch = width_;
        pic.frameIdx = encFrameIdx_;
        pic.inputTimeStamp = static_cast<uint64_t>(ptsUs);
        pic.inputDuration = nominalDurUs;
        pic.inputBuffer = mir.mappedResource;
        pic.outputBitstream = slots_[slotIdx].bitstream;
        pic.completionEvent = slots_[slotIdx].eventHandle;
        pic.bufferFmt = mir.mappedBufferFmt;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        if (forceNextIdr) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
            forceNextIdr = false;
        }

        NVENCSTATUS est = nv_.nvEncEncodePicture(encoder_, &pic);
        if (est == NV_ENC_SUCCESS) {
            slots_[slotIdx].mappedRes = mir.mappedResource;
            slots_[slotIdx].inFlight = true;
            slots_[slotIdx].ptsUs = ptsUs;
            slots_[slotIdx].durationUs = nominalDurUs;
            encFrameIdx_++;
        } else {
            nv_.nvEncUnmapInputResource(encoder_, mir.mappedResource);
            if (est != NV_ENC_ERR_NEED_MORE_INPUT) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.errors++;
            }
        }

        if (collectSamples_.load(std::memory_order_relaxed)) {
            double us = static_cast<double>(QpcDeltaToUs(NowQpcTicks() - tStartTicks));
            std::lock_guard<std::mutex> lock(statsMutex_);
            submitSamplesUs_.push_back(us);
        }

        paceTick++;
        int64_t nextTargetUs = paceStartUs + static_cast<int64_t>(std::llround(static_cast<double>(paceTick) * framePeriodUs));
        int64_t nowUs = QpcDeltaToUs(NowQpcTicks() - epochQpc_);
        if (nowUs - nextTargetUs > static_cast<int64_t>(2.0 * framePeriodUs)) {
            paceStartUs = nowUs;
            paceTick = 0;
        } else if (nextTargetUs > nowUs) {
            int64_t waitUs = nextTargetUs - nowUs;
            if (timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -waitUs * 10LL;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE hs[2] = { timer, stopEvent_ };
                WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            } else {
                WaitForSingleObject(stopEvent_, static_cast<DWORD>(waitUs / 1000LL));
            }
        }
    }

    if (timer) CloseHandle(timer);
    DestroyAllResources();
    initialDesktop_ = nullptr;
    if (usedTimePeriod) timeEndPeriod(1);
    CoUninitialize();
}

}
