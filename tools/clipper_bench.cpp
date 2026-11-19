#include "clock.h"
#include "config.h"
#include "recorder_engine.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace clipper;

namespace {

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
}

double Median(std::vector<double> v) {
    return Percentile(std::move(v), 0.50);
}

uint64_t FileTimeToUs(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10ULL;
}

bool VerifyMp4WithSourceReader(const std::wstring& path, uint32_t expectedW, uint32_t expectedH,
                               int minVideoFrames, bool expectAudio) {
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr) || !reader) {
        std::printf("Verify FAIL: MFCreateSourceReaderFromURL HR=0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    PROPVARIANT varDur;
    PropVariantInit(&varDur);
    double durSec = 0.0;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &varDur))) {
        durSec = static_cast<double>(varDur.uhVal.QuadPart) / 10'000'000.0;
    }
    PropVariantClear(&varDur);

    DWORD videoIdx = UINT32_MAX;
    DWORD audioIdx = UINT32_MAX;
    for (DWORD i = 0; i < 4; ++i) {
        ComPtr<IMFMediaType> nat;
        if (FAILED(reader->GetNativeMediaType(i, 0, &nat))) break;
        GUID maj{};
        nat->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Video && videoIdx == UINT32_MAX) {
            videoIdx = i;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(nat.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (w != expectedW || h != expectedH) {
                std::printf("Verify FAIL: expected %ux%u got %ux%u\n", expectedW, expectedH, w, h);
                return false;
            }
        } else if (maj == MFMediaType_Audio && audioIdx == UINT32_MAX) {
            audioIdx = i;
        }
    }

    if (videoIdx == UINT32_MAX) {
        std::printf("Verify FAIL: missing video stream\n");
        return false;
    }
    if (expectAudio && audioIdx == UINT32_MAX) {
        std::printf("Verify FAIL: missing audio stream\n");
        return false;
    }

    ComPtr<IMFMediaType> decV;
    MFCreateMediaType(&decV);
    decV->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decV->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(reader->SetCurrentMediaType(videoIdx, nullptr, decV.Get()))) {
        std::printf("Verify FAIL: SetCurrentMediaType NV12 failed\n");
        return false;
    }

    if (audioIdx != UINT32_MAX) {
        ComPtr<IMFMediaType> decA;
        MFCreateMediaType(&decA);
        decA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        decA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (FAILED(reader->SetCurrentMediaType(audioIdx, nullptr, decA.Get()))) {
            std::printf("Verify FAIL: SetCurrentMediaType PCM failed\n");
            return false;
        }
    }

    bool vDone = false;
    bool aDone = (audioIdx == UINT32_MAX);
    int videoFrames = 0;
    size_t pcmBytes = 0;
    int reads = 0;

    while ((!vDone || !aDone) && reads < 50000) {
        reads++;
        DWORD sIdx = 0, sFlags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &sIdx, &sFlags, nullptr, &sample);
        if (FAILED(hr)) {
            std::printf("Verify FAIL: ReadSample HR=0x%08lX\n", static_cast<unsigned long>(hr));
            return false;
        }
        if (sFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sIdx == videoIdx) vDone = true;
            if (sIdx == audioIdx) aDone = true;
            continue;
        }
        if (!sample) continue;
        if (sIdx == videoIdx) {
            videoFrames++;
        } else if (sIdx == audioIdx) {
            DWORD len = 0;
            sample->GetTotalLength(&len);
            pcmBytes += len;
        }
    }

    std::printf("Verify MP4: dur=%.3fs videoFrames=%d pcmBytes=%zu\n", durSec, videoFrames, pcmBytes);
    if (reads >= 50000) {
        std::printf("Verify FAIL: hit read limit (%d)\n", reads);
        return false;
    }
    if (durSec < 0.5) {
        std::printf("Verify FAIL: container duration too short (%.3fs)\n", durSec);
        return false;
    }
    if (videoFrames < minVideoFrames) {
        std::printf("Verify FAIL: videoFrames %d < %d\n", videoFrames, minVideoFrames);
        return false;
    }
    if (expectAudio && pcmBytes == 0) {
        std::printf("Verify FAIL: zero decoded PCM bytes\n");
        return false;
    }
    return true;
}

int RunSelfTestClip(int seconds, const std::wstring& outPath, AppConfig cfg) {
    uint32_t maxForBudget = kMaxBitrateSecondsBudget / std::max<uint32_t>(1u, cfg.bitrateMbps);
    uint32_t desired = static_cast<uint32_t>(std::max(5, seconds + 5));
    cfg.replaySeconds = std::clamp<uint32_t>(desired, 5u, std::max<uint32_t>(5u, maxForBudget));
    cfg.saveChime = false;

    RecorderEngine engine(cfg);
    engine.SetCollectVideoSubmitSamples(true);
    std::string err;
    if (!engine.Start(err)) {
        std::printf("SelfTest FAIL: RecorderEngine::Start: %s\n", err.c_str());
        return 1;
    }

    Sleep(static_cast<DWORD>(seconds * 1000));
    SaveResult save = engine.SaveClipSync(outPath);
    auto submitUs = engine.TakeVideoSubmitSamplesUs();
    VideoStats vs = engine.GetVideoStats();
    AudioStats as = engine.GetAudioStats();
    engine.Stop();

    if (!save.ok) {
        std::printf("SelfTest FAIL: SaveClipSync: %s\n", save.error.c_str());
        return 1;
    }

    std::error_code ec;
    uint64_t fileBytes = std::filesystem::file_size(outPath, ec);
    std::printf("SelfTest Clip: file=%ls bytes=%llu saveMs=%.2f\n",
        outPath.c_str(), static_cast<unsigned long long>(fileBytes), save.saveMs);
    std::printf("SelfTest Video: %ux%u captured=%llu static=%llu encoded=%llu recoveries=%llu err=%llu | submit med=%.1f us p95=%.1f us\n",
        vs.width, vs.height,
        static_cast<unsigned long long>(vs.framesCaptured),
        static_cast<unsigned long long>(vs.framesStatic),
        static_cast<unsigned long long>(vs.framesEncoded),
        static_cast<unsigned long long>(vs.recoveries),
        static_cast<unsigned long long>(vs.errors),
        Percentile(submitUs, 0.50), Percentile(submitUs, 0.95));
    std::printf("SelfTest Audio: aac=%llu desktopPkts=%llu micPkts=%llu reopens=%llu err=%llu\n",
        static_cast<unsigned long long>(as.aacFramesEncoded),
        static_cast<unsigned long long>(as.desktopPackets),
        static_cast<unsigned long long>(as.micPackets),
        static_cast<unsigned long long>(as.deviceReopens),
        static_cast<unsigned long long>(as.errors));

    int minFrames = std::max(1, (seconds * static_cast<int>(cfg.fps)) - 15);
    bool expectAudio = (cfg.recordDesktopAudio || cfg.recordMicAudio);
    if (!VerifyMp4WithSourceReader(outPath, vs.width, vs.height, minFrames, expectAudio)) {
        return 1;
    }
    if (vs.errors > 0 || as.errors > 0) {
        std::printf("SelfTest FAIL: non-zero pipeline error count\n");
        return 1;
    }
    std::printf("PASS SelfTestClip\n");
    return 0;
}

class Gpu3dWorkload {
public:
    ~Gpu3dWorkload() {
        if (swapChain_) swapChain_->SetFullscreenState(FALSE, nullptr);
        swapChain_.Reset();
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    bool Init() {
        ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> nvAdapter;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 ad{};
            a->GetDesc1(&ad);
            if (ad.VendorId == 0x10DE) {
                nvAdapter = a;
                break;
            }
        }
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
        if (FAILED(D3D11CreateDevice(nvAdapter.Get(), nvAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_))) {
            return false;
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ClipperBenchGpuWnd";
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"ClipperBenchGpuWnd", L"ClipperBenchGPU",
            WS_POPUP | WS_VISIBLE, 16, 16, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd_) return false;

        BOOL tearing = FALSE;
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(factory.As(&f5))) {
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        }
        allowTearing_ = (tearing == TRUE);

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = 640;
        scd.Height = 360;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Flags = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if (FAILED(factory->CreateSwapChainForHwnd(dev_.Get(), hwnd_, &scd, nullptr, nullptr, &swapChain_))) {
            return false;
        }
        factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

        static const char kVsSrc[] =
            "struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
            "VOut main(uint id : SV_VertexID) {\n"
            "  VOut o;\n"
            "  o.uv = float2((id << 1) & 2, id & 2);\n"
            "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
            "  return o;\n"
            "}\n";

        static const char kPsSrc[] =
            "cbuffer FrameCB : register(b0) { float4 u_param; };\n"
            "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
            "  float2 p = uv * 3.14159 + u_param.xy;\n"
            "  float acc = 0.0;\n"
            "  [loop] for (int i = 0; i < 768; ++i) {\n"
            "    p = float2(sin(p.x * 1.37 + p.y), cos(p.y * 1.41 - p.x)) + u_param.zw;\n"
            "    acc += dot(p, p) * 0.015625;\n"
            "  }\n"
            "  return float4(frac(acc), frac(acc * 0.7), frac(acc * 0.3), 1.0);\n"
            "}\n";

        ComPtr<ID3DBlob> vsBlob, psBlob;
        if (FAILED(D3DCompile(kVsSrc, sizeof(kVsSrc) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr))) return false;
        if (FAILED(D3DCompile(kPsSrc, sizeof(kPsSrc) - 1, nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr))) return false;
        if (FAILED(dev_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_))) return false;
        if (FAILED(dev_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_))) return false;

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&cbd, nullptr, &cb_))) return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = 1920; td.Height = 1080; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &rt_))) return false;
        if (FAILED(dev_->CreateRenderTargetView(rt_.Get(), nullptr, &rtv_))) return false;

        D3D11_TEXTURE2D_DESC std{};
        std.Width = 1; std.Height = 1; std.MipLevels = 1; std.ArraySize = 1;
        std.Format = DXGI_FORMAT_R8G8B8A8_UNORM; std.SampleDesc.Count = 1;
        std.Usage = D3D11_USAGE_STAGING; std.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&std, nullptr, &staging_))) return false;

        for (int i = 0; i < kQueryPool; ++i) {
            D3D11_QUERY_DESC qdDis{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC qdTs{ D3D11_QUERY_TIMESTAMP, 0 };
            dev_->CreateQuery(&qdDis, &qPool_[i].disjoint);
            dev_->CreateQuery(&qdTs, &qPool_[i].tsStart);
            dev_->CreateQuery(&qdTs, &qPool_[i].tsEnd);
        }
        return true;
    }

    struct RunMetrics {
        std::vector<double> gpuFrameMs;
        int framesRendered = 0;
        uint32_t samplePixel = 0;
    };

    RunMetrics RunWindow(int seconds) {
        RunMetrics m;
        D3D11_VIEWPORT vp{ 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
        ctx_->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtvs[] = { rtv_.Get() };
        ctx_->OMSetRenderTargets(1, rtvs, nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = { cb_.Get() };
        ctx_->PSSetConstantBuffers(0, 1, cbs);

        for (int i = 0; i < kQueryPool; ++i) qPool_[i].active = false;
        int64_t endTicks = NowQpcTicks() + static_cast<int64_t>(seconds) * QpcFrequency();
        uint64_t frame = 0;

        auto collectQuery = [&](int idx) {
            auto& q = qPool_[idx];
            if (!q.active) return;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dis{};
            while (ctx_->GetData(q.disjoint.Get(), &dis, sizeof(dis), 0) == S_FALSE) {
                Sleep(0);
            }
            UINT64 t0 = 0, t1 = 0;
            while (ctx_->GetData(q.tsStart.Get(), &t0, sizeof(t0), 0) == S_FALSE) {
                Sleep(0);
            }
            while (ctx_->GetData(q.tsEnd.Get(), &t1, sizeof(t1), 0) == S_FALSE) {
                Sleep(0);
            }
            q.active = false;
            if (!dis.Disjoint && dis.Frequency > 0 && t1 >= t0) {
                double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dis.Frequency);
                m.gpuFrameMs.push_back(ms);
            }
        };

        while (NowQpcTicks() < endTicks) {
            int idx = static_cast<int>(frame % kQueryPool);
            collectQuery(idx);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                float* f = static_cast<float*>(mapped.pData);
                f[0] = static_cast<float>(frame % 100) * 0.01f;
                f[1] = 0.37f;
                f[2] = 0.13f;
                f[3] = 0.29f;
                ctx_->Unmap(cb_.Get(), 0);
            }

            ctx_->Begin(qPool_[idx].disjoint.Get());
            ctx_->End(qPool_[idx].tsStart.Get());
            ctx_->OMSetRenderTargets(1, rtvs, nullptr);
            ctx_->Draw(3, 0);
            ctx_->End(qPool_[idx].tsEnd.Get());
            ctx_->End(qPool_[idx].disjoint.Get());
            qPool_[idx].active = true;

            ComPtr<ID3D11Texture2D> backBuf;
            if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) {
                D3D11_BOX copyBox{ 0, 0, 0, 640, 360, 1 };
                ctx_->CopySubresourceRegion(backBuf.Get(), 0, 0, 0, 0, rt_.Get(), 0, &copyBox);
            }
            swapChain_->Present(0, allowTearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);

            MSG msg{};
            while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            m.framesRendered++;
            frame++;
        }

        for (int i = 0; i < kQueryPool; ++i) collectQuery(i);

        D3D11_BOX box{ 500, 500, 0, 501, 501, 1 };
        ctx_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, rt_.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE sr{};
        if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &sr))) {
            m.samplePixel = *static_cast<const uint32_t*>(sr.pData);
            ctx_->Unmap(staging_.Get(), 0);
        }
        return m;
    }

private:
    static constexpr int kQueryPool = 4;
    struct QuerySlot {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> tsStart;
        ComPtr<ID3D11Query> tsEnd;
        bool active = false;
    };
    HWND hwnd_ = nullptr;
    bool allowTearing_ = false;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11Texture2D> rt_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> staging_;
    QuerySlot qPool_[kQueryPool];
};

HWND FindWindowForPid(DWORD targetPid, const wchar_t* className) {
    struct SearchCtx {
        DWORD pid;
        const wchar_t* cls;
        HWND found = nullptr;
    } s{ targetPid, className, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* ctx = reinterpret_cast<SearchCtx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
        wchar_t clsBuf[128] = {};
        if (GetClassNameW(hwnd, clsBuf, 128) > 0 && wcscmp(clsBuf, ctx->cls) == 0) {
            ctx->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

int RunGpuLoadBench(int secondsPerRun, int runsPerSide) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path clipperExe = std::filesystem::path(exePath).parent_path() / L"clipper.exe";
    if (!std::filesystem::exists(clipperExe)) {
        std::printf("Bench FAIL: clipper.exe not found next to clipper_bench.exe (%ls)\n", clipperExe.c_str());
        return 1;
    }

    if (HWND existing = FindWindowW(L"ClipperTrayHiddenWindow", clipper::kAppName)) {
        std::printf("Bench FAIL: clipper.exe is already running; close it first\n");
        return 1;
    }

    Gpu3dWorkload gpu;
    if (!gpu.Init()) {
        std::printf("Bench FAIL: Gpu3dWorkload::Init failed\n");
        return 1;
    }

    std::printf("Warming up 3D GPU workload (1s)...\n");
    auto warm = gpu.RunWindow(1);
    if (warm.gpuFrameMs.empty() || warm.samplePixel == 0) {
        std::printf("Bench FAIL: GPU warmup produced no valid work (frames=%d pixel=0x%08X)\n",
            warm.framesRendered, warm.samplePixel);
        return 1;
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    struct SideResult {
        std::vector<double> p50Ms;
        std::vector<double> p95Ms;
        std::vector<double> fps;
        std::vector<double> clipperCpuPct;
        std::vector<double> clipperRamMb;
        std::vector<uint64_t> clipperDiskWriteBytes;
    };
    SideResult offRes, onRes;

    for (int r = 0; r < runsPerSide; ++r) {
        for (int side = 0; side < 2; ++side) {
            bool isOn = (side == 1);
            PROCESS_INFORMATION pi{};
            HANDLE hReady = nullptr;
            if (isOn) {
                wchar_t readyName[96] = {};
                std::swprintf(readyName, sizeof(readyName) / sizeof(wchar_t),
                    L"Local\\ClipperBenchReady_%lu_%d", GetCurrentProcessId(), r);
                hReady = CreateEventW(nullptr, TRUE, FALSE, readyName);
                std::wstring cmd = L"\"" + clipperExe.wstring() +
                    L"\" --tray --fail-fast --seconds=15 --fps=60 --bitrate=20 --no-chime --ready-event=" + readyName;
                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &pi)) {
                    std::printf("Bench FAIL: CreateProcessW clipper.exe err=%lu\n", GetLastError());
                    if (hReady) CloseHandle(hReady);
                    return 1;
                }
                HANDLE waitHandles[2] = { hReady, pi.hProcess };
                bool hadReady = (hReady != nullptr);
                DWORD wr = WaitForMultipleObjects(hadReady ? 2 : 1, hadReady ? waitHandles : &pi.hProcess, FALSE, 5000);
                if (hReady) { CloseHandle(hReady); hReady = nullptr; }
                if (!hadReady || wr != WAIT_OBJECT_0) {
                    DWORD exitCode = 0;
                    GetExitCodeProcess(pi.hProcess, &exitCode);
                    std::printf("Bench FAIL: clipper.exe failed readiness wait (wr=%lu code=%lu)\n", wr, exitCode);
                    TerminateProcess(pi.hProcess, 1);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    return 1;
                }
            }

            FILETIME ct0{}, et0{}, kt0{}, ut0{}, ct1{}, et1{}, kt1{}, ut1{};
            IO_COUNTERS io0{}, io1{};
            int64_t w0 = NowQpcTicks();
            if (isOn) {
                GetProcessTimes(pi.hProcess, &ct0, &et0, &kt0, &ut0);
                GetProcessIoCounters(pi.hProcess, &io0);
            }

            auto rm = gpu.RunWindow(secondsPerRun);

            int64_t w1 = NowQpcTicks();
            double cpuPct = 0.0, ramMb = 0.0;
            uint64_t diskWrites = 0;
            if (isOn) {
                GetProcessTimes(pi.hProcess, &ct1, &et1, &kt1, &ut1);
                GetProcessIoCounters(pi.hProcess, &io1);
                PROCESS_MEMORY_COUNTERS pmc{};
                GetProcessMemoryInfo(pi.hProcess, &pmc, sizeof(pmc));
                ramMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
                double wallUs = static_cast<double>(QpcDeltaToUs(w1 - w0));
                double procUs = static_cast<double>((FileTimeToUs(kt1) + FileTimeToUs(ut1)) -
                                                    (FileTimeToUs(kt0) + FileTimeToUs(ut0)));
                cpuPct = (procUs / wallUs) * 100.0 / static_cast<double>(si.dwNumberOfProcessors);
                diskWrites = io1.WriteTransferCount - io0.WriteTransferCount;

                HWND cw = FindWindowForPid(pi.dwProcessId, L"ClipperTrayHiddenWindow");
                if (cw) PostMessageW(cw, WM_CLOSE, 0, 0);
                DWORD stopWr = WaitForSingleObject(pi.hProcess, 4000);
                if (stopWr != WAIT_OBJECT_0) {
                    std::printf("\nBench FAIL: child clipper.exe pid=%lu did not exit cleanly\n", pi.dwProcessId);
                    TerminateProcess(pi.hProcess, 1);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    return 1;
                }
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }

            double p50 = Percentile(rm.gpuFrameMs, 0.50);
            double p95 = Percentile(rm.gpuFrameMs, 0.95);
            double fps = static_cast<double>(rm.framesRendered) / static_cast<double>(secondsPerRun);
            std::printf("Run %d [%s]: 3D p50=%.3fms p95=%.3fms fps=%.1f px=0x%08X",
                r + 1, isOn ? "CLIPPER_ON " : "CLIPPER_OFF", p50, p95, fps, rm.samplePixel);
            if (isOn) {
                std::printf(" | cpu=%.3f%% ram=%.1fMB diskWrite=%lluB",
                    cpuPct, ramMb, static_cast<unsigned long long>(diskWrites));
                onRes.p50Ms.push_back(p50);
                onRes.p95Ms.push_back(p95);
                onRes.fps.push_back(fps);
                onRes.clipperCpuPct.push_back(cpuPct);
                onRes.clipperRamMb.push_back(ramMb);
                onRes.clipperDiskWriteBytes.push_back(diskWrites);
            } else {
                offRes.p50Ms.push_back(p50);
                offRes.p95Ms.push_back(p95);
                offRes.fps.push_back(fps);
            }
            std::printf("\n");
        }
    }

    auto minmax = [](const std::vector<double>& v) {
        auto [mn, mx] = std::minmax_element(v.begin(), v.end());
        return std::pair<double, double>{ *mn, *mx };
    };
    double offP50 = Median(offRes.p50Ms), onP50 = Median(onRes.p50Ms);
    double offP95 = Median(offRes.p95Ms), onP95 = Median(onRes.p95Ms);
    double offFps = Median(offRes.fps), onFps = Median(onRes.fps);
    auto [offMin, offMax] = minmax(offRes.p50Ms);
    auto [onMin, onMax] = minmax(onRes.p50Ms);
    uint64_t maxDisk = *std::max_element(onRes.clipperDiskWriteBytes.begin(), onRes.clipperDiskWriteBytes.end());

    std::printf("\n=== SUMMARY (%d alternating runs x %ds) ===\n", runsPerSide, secondsPerRun);
    std::printf("OFF: 3D p50=%.3f ms (range %.3f..%.3f) | p95=%.3f ms | FPS=%.1f\n",
        offP50, offMin, offMax, offP95, offFps);
    std::printf("ON : 3D p50=%.3f ms (range %.3f..%.3f) | p95=%.3f ms | FPS=%.1f\n",
        onP50, onMin, onMax, onP95, onFps);
    std::printf("Delta p50: %+.3f ms (%+.2f%%)\n", onP50 - offP50, ((onP50 - offP50) / offP50) * 100.0);
    std::printf("clipper.exe footprint: median CPU=%.3f%% | median RAM=%.1f MB | max active disk write=%llu B\n",
        Median(onRes.clipperCpuPct), Median(onRes.clipperRamMb), static_cast<unsigned long long>(maxDisk));
    return 0;
}

}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    bool selfTest = false;
    bool gpuBench = false;
    int seconds = 3;
    int runs = 5;
    std::wstring outPath = L"build\\test_out\\live_replay.mp4";

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--self-test-clip") {
            selfTest = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') seconds = _wtoi(argv[++i]);
        } else if (a == L"--gpu-load-bench") {
            gpuBench = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') seconds = _wtoi(argv[++i]);
        } else if (a == L"--out" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (a.rfind(L"--out=", 0) == 0) {
            outPath = a.substr(6);
        } else if (a == L"--runs" && i + 1 < argc) {
            runs = std::max(1, _wtoi(argv[++i]));
        } else if (a.rfind(L"--runs=", 0) == 0) {
            runs = std::max(1, _wtoi(a.substr(7).c_str()));
        }
    }

    int rc = 0;
    if (selfTest) {
        auto parsed = ApplyCliOverrides(argc, argv, MakeDefaultConfig());
        rc = RunSelfTestClip(std::max(1, seconds), outPath, parsed.config);
    } else if (gpuBench) {
        rc = RunGpuLoadBench(std::max(1, seconds), runs);
    } else {
        std::printf("Usage:\n  clipper_bench --self-test-clip <sec> --out <file.mp4>\n  clipper_bench --gpu-load-bench <sec> --runs <N>\n");
        rc = 1;
    }

    MFShutdown();
    CoUninitialize();
    return rc;
}
