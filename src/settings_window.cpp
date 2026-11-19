#include "settings_window.h"

#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace clipper {

namespace {

constexpr wchar_t kSettingsWndClass[] = L"ClipperSettingsWindow";
constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT_PTR kThumbTimerId = 2;

constexpr float kWndW = 1000.0f;
constexpr float kWndH = 640.0f;
constexpr float kTitleBarH = 40.0f;
constexpr float kRailW = 54.0f;
constexpr float kContentPadL = 16.0f;
constexpr float kContentPadR = 28.0f;
constexpr float kContentX = kRailW + kContentPadL;
constexpr float kContentW = kWndW - kContentX - kContentPadR;
constexpr float kViewTop = 92.0f;
constexpr float kViewBottom = 584.0f;
constexpr float kViewH = kViewBottom - kViewTop;

constexpr D2D1_COLOR_F C(uint32_t rgb, float a = 1.0f) {
    return D2D1_COLOR_F{
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        a
    };
}

constexpr D2D1_COLOR_F kColMicaBase = C(0x1B1C1F);
constexpr D2D1_COLOR_F kColSurface = C(0xFFFFFF, 0.045f);
constexpr D2D1_COLOR_F kColSurfaceHover = C(0xFFFFFF, 0.075f);
constexpr D2D1_COLOR_F kColSurfaceActive = C(0xFFFFFF, 0.115f);
constexpr D2D1_COLOR_F kColBorder = C(0xFFFFFF, 0.055f);
constexpr D2D1_COLOR_F kColBorderTop = C(0xFFFFFF, 0.105f);
constexpr D2D1_COLOR_F kColBorderHover = C(0xFFFFFF, 0.16f);
constexpr D2D1_COLOR_F kColWell = C(0x0E0F11, 0.65f);
constexpr D2D1_COLOR_F kColInputBg = C(0x111214);
constexpr D2D1_COLOR_F kColInputBorder = C(0xFFFFFF, 0.14f);

constexpr D2D1_COLOR_F kColText = C(0xFFFFFF);
constexpr D2D1_COLOR_F kColTextSec = C(0xD4D4D8);
constexpr D2D1_COLOR_F kColTextMuted = C(0x9E9EA8);

constexpr D2D1_COLOR_F kColAccent = C(0xF4F4F5);
constexpr D2D1_COLOR_F kColAccentText = C(0x111113);
constexpr D2D1_COLOR_F kColGreen = C(0x34D399);
constexpr D2D1_COLOR_F kColAmber = C(0xF59E0B);
constexpr D2D1_COLOR_F kColRed = C(0xEF4444);
constexpr D2D1_COLOR_F kColCloseHover = C(0xC42B1C);

constexpr D2D1_COLOR_F kColKeyBody = C(0x232428);
constexpr D2D1_COLOR_F kColKeyBorder = C(0xFFFFFF, 0.14f);

constexpr uint32_t kFpsPresets[] = { 30, 60, 120, 144 };
constexpr uint32_t kReplayPresetsSec[] = { 30, 60, 120, 300, 600 };

std::vector<std::wstring> SplitKeycaps(const std::string& display) {
    std::vector<std::wstring> parts;
    std::wstring wide = Utf8ToWide(display);
    size_t start = 0;
    while (start <= wide.size()) {
        size_t plus = wide.find(L'+', start);
        std::wstring tok = (plus == std::wstring::npos) ? wide.substr(start) : wide.substr(start, plus - start);
        if (!tok.empty()) parts.push_back(std::move(tok));
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }
    return parts;
}

bool ConfigsEqual(const AppConfig& a, const AppConfig& b) {
    return a.monitorIndex == b.monitorIndex &&
           a.fps == b.fps &&
           a.bitrateMbps == b.bitrateMbps &&
           a.replaySeconds == b.replaySeconds &&
           a.recordDesktopAudio == b.recordDesktopAudio &&
           a.recordMicAudio == b.recordMicAudio &&
           a.saveChime == b.saveChime &&
           a.hotkeySave == b.hotkeySave &&
           a.outputDirRaw == b.outputDirRaw;
}

bool PtInDipRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

float ClampF(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

bool CreateConfiguredTextFormat(IDWriteFactory* dw, const wchar_t* family, float size,
                                DWRITE_FONT_WEIGHT weight, bool wrap, bool ellipsis,
                                Microsoft::WRL::ComPtr<IDWriteTextFormat>& outFmt) {
    if (!dw) return false;
    outFmt.Reset();
    HRESULT hr = dw->CreateTextFormat(
        family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"en-US", &outFmt);
    if (FAILED(hr) || !outFmt) return false;
    outFmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis && !wrap) {
        Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(outFmt.Get(), &sign)) && sign) {
            DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            outFmt->SetTrimming(&trim, sign.Get());
        }
    }
    return true;
}

std::wstring FormatReplayBoxValue(uint32_t seconds, bool asMinutes) {
    if (!asMinutes) {
        return std::to_wstring(seconds);
    }
    if ((seconds % 60u) == 0u) {
        return std::to_wstring(seconds / 60u);
    }
    wchar_t buf[32] = {};
    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%.2f", static_cast<double>(seconds) / 60.0);
    std::wstring s(buf);
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s;
}

std::wstring FormatRamHuman(double megabytes) {
    wchar_t buf[48] = {};
    if (megabytes >= 1024.0) {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%.2f GB", megabytes / 1024.0);
    } else {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%.0f MB", megabytes);
    }
    return buf;
}

bool ParseStrictPositiveUint(std::wstring_view s, uint32_t& out) {
    if (s.empty()) return false;
    uint64_t val = 0;
    for (wchar_t ch : s) {
        if (ch < L'0' || ch > L'9') return false;
        val = val * 10ULL + static_cast<uint64_t>(ch - L'0');
        if (val > 1'000'000ULL) val = 1'000'000ULL;
    }
    if (val == 0) return false;
    out = static_cast<uint32_t>(val);
    return true;
}

bool LoadMp4FirstFrameBitmap(const std::wstring& path, ID2D1RenderTarget* rt,
                             Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBmp) {
    struct MfScope {
        bool started = false;
        MfScope() { started = SUCCEEDED(MFStartup(MF_VERSION)); }
        ~MfScope() { if (started) MFShutdown(); }
        MfScope(const MfScope&) = delete;
        MfScope& operator=(const MfScope&) = delete;
    } mf;
    if (!mf.started || !rt) return false;

    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || !attrs) return false;
    if (FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))) return false;

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader)) || !reader) {
        return false;
    }

    constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
    constexpr DWORD kFirstVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader->SetStreamSelection(kAllStreams, FALSE);
    if (FAILED(reader->SetStreamSelection(kFirstVideo, TRUE))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> rgbType;
    if (FAILED(MFCreateMediaType(&rgbType)) || !rgbType) return false;
    if (FAILED(rgbType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) return false;
    if (FAILED(rgbType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32))) return false;
    if (FAILED(reader->SetCurrentMediaType(kFirstVideo, nullptr, rgbType.Get()))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> actualType;
    if (FAILED(reader->GetCurrentMediaType(kFirstVideo, &actualType)) || !actualType) {
        return false;
    }

    UINT32 w = 0;
    UINT32 h = 0;
    if (FAILED(MFGetAttributeSize(actualType.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
        return false;
    }
    INT32 defaultStride = static_cast<INT32>(w * 4u);
    UINT32 dsU = 0;
    if (SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, &dsU))) {
        defaultStride = static_cast<INT32>(dsU);
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    for (int attempt = 0; attempt < 8; ++attempt) {
        DWORD flags = 0;
        Microsoft::WRL::ComPtr<IMFSample> cur;
        HRESULT hr = reader->ReadSample(kFirstVideo, 0, nullptr, &flags, nullptr, &cur);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            Microsoft::WRL::ComPtr<IMFMediaType> changedType;
            if (SUCCEEDED(reader->GetCurrentMediaType(kFirstVideo, &changedType)) && changedType) {
                UINT32 nw = 0;
                UINT32 nh = 0;
                if (SUCCEEDED(MFGetAttributeSize(changedType.Get(), MF_MT_FRAME_SIZE, &nw, &nh)) && nw > 0 && nh > 0) {
                    w = nw;
                    h = nh;
                    defaultStride = static_cast<INT32>(w * 4u);
                    UINT32 cdsU = 0;
                    if (SUCCEEDED(changedType->GetUINT32(MF_MT_DEFAULT_STRIDE, &cdsU))) {
                        defaultStride = static_cast<INT32>(cdsU);
                    }
                }
            }
        }
        if (cur) {
            sample = cur;
            break;
        }
    }
    if (!sample || w > 16384u || h > 16384u) return false;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf) return false;

    Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
    BYTE* pixels = nullptr;
    LONG srcStride = 0;
    DWORD curLen = 0;
    bool locked2d = false;
    bool locked1d = false;

    if (SUCCEEDED(buf.As(&buf2d)) && buf2d && SUCCEEDED(buf2d->Lock2D(&pixels, &srcStride))) {
        locked2d = true;
    } else if (SUCCEEDED(buf->Lock(&pixels, nullptr, &curLen))) {
        locked1d = true;
        srcStride = static_cast<LONG>(defaultStride);
    } else {
        return false;
    }

    auto unlockBuf = [&]() {
        if (locked2d) buf2d->Unlock2D();
        else if (locked1d) buf->Unlock();
    };

    if (!pixels) {
        unlockBuf();
        return false;
    }
    if (srcStride == 0) srcStride = static_cast<LONG>(w * 4u);

    uint32_t rowBytes = w * 4u;
    int64_t absStride64 = srcStride < 0 ? -static_cast<int64_t>(srcStride) : static_cast<int64_t>(srcStride);
    if (absStride64 < rowBytes || absStride64 > 100'000) {
        unlockBuf();
        return false;
    }
    uint32_t absStride = static_cast<uint32_t>(absStride64);
    if (!locked2d && static_cast<uint64_t>(absStride) * h > curLen) {
        unlockBuf();
        return false;
    }

    std::vector<uint8_t> flipped;
    const void* uploadPtr = pixels;
    UINT32 uploadPitch = absStride;
    if (srcStride < 0) {
        flipped.resize(static_cast<size_t>(rowBytes) * h);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* srcRow = locked2d
                ? (pixels + static_cast<ptrdiff_t>(y) * srcStride)
                : (pixels + static_cast<size_t>(h - 1u - y) * absStride);
            std::memcpy(flipped.data() + static_cast<size_t>(y) * rowBytes, srcRow, rowBytes);
        }
        uploadPtr = flipped.data();
        uploadPitch = rowBytes;
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    HRESULT bmpHr = rt->CreateBitmap(D2D1::SizeU(w, h), uploadPtr, uploadPitch, &props, &outBmp);

    unlockBuf();
    return SUCCEEDED(bmpHr) && outBmp;
}

}

SettingsWindow::SettingsWindow(SettingsCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

SettingsWindow::~SettingsWindow() {
    CloseIfOpen();
}

void SettingsWindow::ShowOrFocus(HINSTANCE hInst, HICON icon, SettingsTab initialTab) {
    CommitOrCancelInput(false);
    activeTab_ = initialTab;
    scrollY_ = 0.0f;

    if (hwnd_) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        ReloadClips();
        RefreshStatus();
        InvalidateWindow();
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SettingsWindow::StaticWndProc;
    wc.hInstance = hInst;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kSettingsWndClass;
    RegisterClassExW(&wc);

    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
        dpi_ = static_cast<int>(dpiX);
    } else {
        dpi_ = static_cast<int>(GetDpiForSystem());
        if (dpi_ <= 0) dpi_ = 96;
    }

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int winW = MulDiv(static_cast<int>(kWndW), dpi_, 96);
    int winH = MulDiv(static_cast<int>(kWndH), dpi_, 96);
    int posX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - winW) / 2;
    int posY = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - winH) / 2;

    hwnd_ = CreateWindowExW(
        0, kSettingsWndClass, L"Clipper", style,
        posX, posY, winW, winH,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return;

    BOOL dark = TRUE;
    DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
    COLORREF borderCol = RGB(46, 48, 54);
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &borderCol, sizeof(borderCol));
    MARGINS margins{ 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    SetWindowPos(hwnd_, nullptr, posX, posY, winW, winH, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::SetActiveTab(SettingsTab tab) {
    CommitOrCancelInput(true);
    if (activeTab_ != tab) {
        activeTab_ = tab;
        scrollY_ = 0.0f;
        maxScroll_ = 0.0f;
        hoveredHit_ = {};
    }
    if (activeTab_ == SettingsTab::Clips) {
        ReloadClips();
    } else {
        ReloadMonitors();
    }
    InvalidateWindow();
}

void SettingsWindow::NotifyClipsChanged() {
    if (!hwnd_) return;
    ReloadClips();
    InvalidateWindow();
}

void SettingsWindow::CloseIfOpen() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void SettingsWindow::RefreshStatus() {
    if (!hwnd_) return;
    if (callbacks_.getStatus) {
        status_ = callbacks_.getStatus();
    }
    if (!feedbackText_.empty() && feedbackExpiresMs_ != 0 && GetTickCount64() >= feedbackExpiresMs_) {
        feedbackText_.clear();
        feedbackExpiresMs_ = 0;
    }
    InvalidateWindow();
}

void SettingsWindow::ShowFeedback(const std::wstring& text, bool isError) {
    feedbackText_ = text;
    feedbackIsError_ = isError;
    feedbackExpiresMs_ = GetTickCount64() + 6500ULL;
    InvalidateWindow();
}

LRESULT CALLBACK SettingsWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool SettingsWindow::EnsureDeviceIndependentResources() {
    if (!d2dFactory_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2dFactory_)))) {
            return false;
        }
    }
    if (!dwriteFactory_) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
            return false;
        }
    }
    if (!wicFactory_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory_));
    }
    if (!fmtBrand_) {
        glyphFontName_ = L"Segoe MDL2 Assets";
        Microsoft::WRL::ComPtr<IDWriteFontCollection> sysFonts;
        if (SUCCEEDED(dwriteFactory_->GetSystemFontCollection(&sysFonts, FALSE)) && sysFonts) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sysFonts->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists)) && exists) {
                glyphFontName_ = L"Segoe Fluent Icons";
            }
        }
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false, fmtBrand_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true, fmtPageTitle_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 13.5f, DWRITE_FONT_WEIGHT_MEDIUM, false, true, fmtRowTitle_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, false, true, fmtBody_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true, fmtBodySemi_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, false, true, fmtSmall_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Cascadia Mono", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, false, true, fmtMono_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Cascadia Mono", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false, fmtKeycap_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), glyphFontName_.c_str(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL, false, false, fmtGlyph_);
    }
    return d2dFactory_ && dwriteFactory_ && fmtBody_ && fmtGlyph_;
}

bool SettingsWindow::EnsureBackbuffer(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (memDC_ && memBmp_ && memW_ == w && memH_ == h) return true;

    DestroyBackbuffer();

    HDC screenDc = GetDC(hwnd_);
    memDC_ = CreateCompatibleDC(screenDc);
    if (screenDc) ReleaseDC(hwnd_, screenDc);
    if (!memDC_) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    memBmp_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!memBmp_) {
        DestroyBackbuffer();
        return false;
    }

    oldBmp_ = SelectObject(memDC_, memBmp_);
    memW_ = w;
    memH_ = h;
    return true;
}

void SettingsWindow::DestroyBackbuffer() {
    if (memDC_) {
        if (oldBmp_) {
            SelectObject(memDC_, oldBmp_);
            oldBmp_ = nullptr;
        }
        if (memBmp_) {
            DeleteObject(memBmp_);
            memBmp_ = nullptr;
        }
        DeleteDC(memDC_);
        memDC_ = nullptr;
    } else if (memBmp_) {
        DeleteObject(memBmp_);
        memBmp_ = nullptr;
    }
    memW_ = 0;
    memH_ = 0;
}

bool SettingsWindow::EnsureDeviceResources() {
    if (!hwnd_ || !EnsureDeviceIndependentResources()) return false;
    if (!rt_) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(dpi_), static_cast<float>(dpi_),
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT);
        HRESULT hr = d2dFactory_->CreateDCRenderTarget(&rtProps, &rt_);
        if (FAILED(hr) || !rt_) return false;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!brush_) {
        if (FAILED(rt_->CreateSolidColorBrush(kColText, &brush_)) || !brush_) {
            return false;
        }
    }
    return true;
}

void SettingsWindow::DiscardDeviceResources() {
    thumbCache_.clear();
    brush_.Reset();
    rt_.Reset();
    DestroyBackbuffer();
}

void SettingsWindow::DestroyAllResources() {
    DiscardDeviceResources();
    fmtBrand_.Reset();
    fmtPageTitle_.Reset();
    fmtRowTitle_.Reset();
    fmtBody_.Reset();
    fmtBodySemi_.Reset();
    fmtSmall_.Reset();
    fmtMono_.Reset();
    fmtKeycap_.Reset();
    fmtGlyph_.Reset();
    wicFactory_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
    hitTargets_.clear();
    clips_.clear();
    monitors_.clear();
}

void SettingsWindow::ReloadConfigState() {
    if (callbacks_.getConfig) {
        appliedCfg_ = callbacks_.getConfig();
        editedCfg_ = appliedCfg_;
        replayUnitMinutes_ = (editedCfg_.replaySeconds >= 60u && (editedCfg_.replaySeconds % 60u) == 0u);
    }
    ReloadMonitors();
    ReloadClips();
}

void SettingsWindow::ReloadMonitors() {
    monitors_ = EnumerateCaptureMonitors();
}

void SettingsWindow::ReloadClips() {
    std::wstring dir = appliedCfg_.outputDir;
    if (dir.empty()) dir = ExpandEnvPath(appliedCfg_.outputDirRaw);
    clips_ = ScanClipsDirectory(dir, 48);
    totalClipsBytes_ = 0;
    for (const auto& c : clips_) {
        totalClipsBytes_ += c.sizeBytes;
    }

    if (hwnd_ && !clips_.empty()) {
        SetTimer(hwnd_, kThumbTimerId, 30, nullptr);
    } else if (hwnd_) {
        KillTimer(hwnd_, kThumbTimerId);
    }
}

void SettingsWindow::StepThumbnailBatch(size_t maxCount, bool invalidateOnLoad) {
    if (!rt_ || !wicFactory_) return;
    size_t loaded = 0;
    bool remaining = false;
    for (const auto& clip : clips_) {
        auto it = thumbCache_.find(clip.fullPath);
        if (it != thumbCache_.end() && it->second.attempted) continue;

        if (loaded >= maxCount) {
            remaining = true;
            break;
        }

        ThumbEntry& entry = thumbCache_[clip.fullPath];
        entry.attempted = true;
        loaded++;

        Microsoft::WRL::ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(clip.fullPath.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            Microsoft::WRL::ComPtr<IShellItemImageFactory> imgFactory;
            if (SUCCEEDED(item.As(&imgFactory)) && imgFactory) {
                HBITMAP hbmp = nullptr;
                HRESULT ghr = imgFactory->GetImage(SIZE{ 480, 270 }, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
                if (SUCCEEDED(ghr) && hbmp) {
                    Microsoft::WRL::ComPtr<IWICBitmap> wicBmp;
                    HRESULT whr = wicFactory_->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapIgnoreAlpha, &wicBmp);
                    DeleteObject(hbmp);
                    if (SUCCEEDED(whr) && wicBmp) {
                        Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
                        if (SUCCEEDED(wicFactory_->CreateFormatConverter(&conv)) && conv &&
                            SUCCEEDED(conv->Initialize(wicBmp.Get(), GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                            rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &entry.bmp);
                        }
                    }
                }
            }
        }
        if (!entry.bmp) {
            LoadMp4FirstFrameBitmap(clip.fullPath, rt_.Get(), entry.bmp);
        }
    }

    if (!remaining && hwnd_) {
        KillTimer(hwnd_, kThumbTimerId);
    }
    if (invalidateOnLoad && loaded > 0) {
        InvalidateWindow();
    }
}

void SettingsWindow::InvalidateWindow() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::IsDirty() const {
    return !ConfigsEqual(appliedCfg_, editedCfg_);
}

void SettingsWindow::ComputeRamEstimate(double& outMb, bool& outOverCap, uint32_t& outClampedSecs) const {
    AppConfig est = editedCfg_;
    est.bitrateMbps = std::clamp(est.bitrateMbps, kMinBitrateMbps, kMaxBitrateMbps);
    est.replaySeconds = std::clamp(est.replaySeconds, kMinReplaySeconds, kMaxReplaySeconds);
    double totalBytes = static_cast<double>(est.VideoArenaBytes() + est.AudioArenaBytes());
    outMb = totalBytes / (1024.0 * 1024.0);
    uint64_t budget = static_cast<uint64_t>(est.bitrateMbps) * est.replaySeconds;
    outOverCap = budget > kMaxBitrateSecondsBudget;
    outClampedSecs = std::max(kMinReplaySeconds, kMaxBitrateSecondsBudget / est.bitrateMbps);
}

void SettingsWindow::DrawTextLine(std::wstring_view text, IDWriteTextFormat* fmt,
                                  D2D1_RECT_F rect, D2D1_COLOR_F color,
                                  DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign) {
    if (!rt_ || !fmt || !brush_ || text.empty()) return;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(valign);
    brush_->SetColor(color);
    rt_->DrawText(text.data(), static_cast<UINT32>(text.size()), fmt, rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void SettingsWindow::DrawGlyph(const wchar_t* glyph, float fontSize, D2D1_RECT_F rect, D2D1_COLOR_F color) {
    if (!rt_ || !dwriteFactory_ || !fmtGlyph_ || !brush_ || !glyph || glyph[0] == L'\0') return;
    fmtGlyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmtGlyph_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    float w = std::max(1.0f, rect.right - rect.left);
    float h = std::max(1.0f, rect.bottom - rect.top);
    if (SUCCEEDED(dwriteFactory_->CreateTextLayout(glyph, 1, fmtGlyph_.Get(), w, h, &layout)) && layout) {
        layout->SetFontSize(fontSize, DWRITE_TEXT_RANGE{ 0, 1 });
        brush_->SetColor(color);
        rt_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush_.Get());
    }
}

void SettingsWindow::DrawFluentSurface(D2D1_RECT_F rect, float radius, bool hovered, bool active) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, radius, radius);
    brush_->SetColor(active ? kColSurfaceActive : (hovered ? kColSurfaceHover : kColSurface));
    rt_->FillRoundedRectangle(rr, brush_.Get());

    brush_->SetColor(active ? kColAccent : (hovered ? kColBorderHover : kColBorder));
    rt_->DrawRoundedRectangle(rr, brush_.Get(), active ? 1.4f : 1.0f);

    if (!active) {
        brush_->SetColor(hovered ? C(0xFFFFFF, 0.16f) : kColBorderTop);
        rt_->DrawLine(
            D2D1::Point2F(rect.left + radius, rect.top + 0.5f),
            D2D1::Point2F(rect.right - radius, rect.top + 0.5f),
            brush_.Get(), 1.0f);
    }
}

void SettingsWindow::DrawFluentButton(D2D1_RECT_F rect, const wchar_t* label, const wchar_t* glyph,
                                      HitKey key, bool primary, bool subtleDisabled, bool inViewport) {
    bool hovered = !subtleDisabled && (hoveredHit_ == key);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 5.0f, 5.0f);

    if (primary) {
        if (subtleDisabled) {
            brush_->SetColor(kColSurface);
            rt_->FillRoundedRectangle(rr, brush_.Get());
            brush_->SetColor(kColBorder);
            rt_->DrawRoundedRectangle(rr, brush_.Get(), 1.0f);
        } else {
            brush_->SetColor(hovered ? C(0xFFFFFF) : kColAccent);
            rt_->FillRoundedRectangle(rr, brush_.Get());
        }
    } else {
        DrawFluentSurface(rect, 5.0f, hovered, false);
    }

    D2D1_COLOR_F fg = primary
        ? (subtleDisabled ? kColTextMuted : kColAccentText)
        : (subtleDisabled ? kColTextMuted : kColTextSec);

    bool hasLabel = (label && label[0] != L'\0');
    bool hasGlyph = (glyph && glyph[0] != L'\0');

    if (hasGlyph && hasLabel) {
        std::wstring_view sv(label);
        float textW = 0.0f;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->CreateTextLayout(
                sv.data(), static_cast<UINT32>(sv.size()), fmtBody_.Get(), 300.0f, 30.0f, &layout))) {
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            textW = tm.width;
        }
        float glyphW = 14.0f;
        float gap = 7.0f;
        float totalW = glyphW + gap + textW;
        float cx = (rect.left + rect.right) * 0.5f;
        float startX = cx - totalW * 0.5f;
        DrawGlyph(glyph, 13.0f, D2D1::RectF(startX, rect.top, startX + glyphW, rect.bottom), fg);
        DrawTextLine(sv, fmtBody_.Get(),
            D2D1::RectF(startX + glyphW + gap, rect.top, rect.right - 4.0f, rect.bottom), fg,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else if (hasGlyph) {
        DrawGlyph(glyph, 14.0f, rect, fg);
    } else if (hasLabel) {
        DrawTextLine(label, primary ? fmtBodySemi_.Get() : fmtBody_.Get(), rect, fg,
            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!subtleDisabled) {
        hitTargets_.push_back({ rect, key, inViewport });
    }
}

void SettingsWindow::DrawEditBox(D2D1_RECT_F rect, std::wstring_view displayValue, InputMode mode,
                                 HitKey key, DWRITE_TEXT_ALIGNMENT align) {
    bool editing = (inputMode_ == mode);
    bool hovered = (hoveredHit_ == key);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 5.0f, 5.0f);

    brush_->SetColor(editing ? C(0x090A0C) : kColInputBg);
    rt_->FillRoundedRectangle(rr, brush_.Get());
    brush_->SetColor(editing ? kColAccent : (hovered ? kColBorderHover : kColInputBorder));
    rt_->DrawRoundedRectangle(rr, brush_.Get(), editing ? 1.5f : 1.0f);

    std::wstring shown = editing ? editBuffer_ : std::wstring(displayValue);
    if (editing && replaceOnFirstChar_ && !shown.empty()) {
        brush_->SetColor(C(0xFFFFFF, 0.16f));
        rt_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rect.left + 6.0f, rect.top + 5.0f, rect.right - 6.0f, rect.bottom - 5.0f), 3.0f, 3.0f),
            brush_.Get());
    } else if (editing) {
        shown.push_back(L'|');
    }

    DrawTextLine(shown, fmtKeycap_.Get(),
        D2D1::RectF(rect.left + 6.0f, rect.top, rect.right - 6.0f, rect.bottom),
        kColText, align, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    hitTargets_.push_back({ rect, key, true });
}

void SettingsWindow::DrawToggleSwitch(D2D1_RECT_F rect, bool on, bool hovered) {
    float h = rect.bottom - rect.top;
    float r = h * 0.5f;
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, r, r);

    if (on) {
        brush_->SetColor(hovered ? C(0xFFFFFF) : kColAccent);
        rt_->FillRoundedRectangle(rr, brush_.Get());
    } else {
        brush_->SetColor(hovered ? C(0xFFFFFF, 0.09f) : C(0xFFFFFF, 0.04f));
        rt_->FillRoundedRectangle(rr, brush_.Get());
        brush_->SetColor(hovered ? C(0xFFFFFF, 0.55f) : C(0xFFFFFF, 0.38f));
        rt_->DrawRoundedRectangle(rr, brush_.Get(), 1.0f);
    }

    float knobR = r - 4.0f;
    float knobX = on ? (rect.right - r) : (rect.left + r);
    float knobY = (rect.top + rect.bottom) * 0.5f;
    brush_->SetColor(on ? kColMicaBase : C(0xD4D4D8));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, knobY), knobR, knobR), brush_.Get());
}

void SettingsWindow::DrawKeycapGroupRight(float rightX, float centerY, const std::string& display, float minLeft) {
    std::vector<std::wstring> parts = SplitKeycaps(display);
    if (parts.empty()) return;

    float capH = 26.0f;
    float padX = 9.0f;
    float minW = 28.0f;
    float gap = 5.0f;

    std::vector<float> widths(parts.size(), minW);
    float totalW = 0.0f;
    for (size_t i = 0; i < parts.size(); ++i) {
        float textW = 14.0f;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->CreateTextLayout(
                parts[i].c_str(), static_cast<UINT32>(parts[i].size()), fmtKeycap_.Get(), 180.0f, 28.0f, &layout))) {
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            textW = std::ceil(tm.width);
        }
        widths[i] = std::max(minW, textW + padX * 2.0f);
        totalW += widths[i] + (i + 1 < parts.size() ? gap : 0.0f);
    }

    float x = std::max(minLeft, rightX - totalW);
    float top = centerY - capH * 0.5f;
    for (size_t i = 0; i < parts.size(); ++i) {
        float w = widths[i];
        if (x + w > rightX + 0.5f) break;

        D2D1_RECT_F capR = D2D1::RectF(x, top, x + w, top + capH);
        brush_->SetColor(kColKeyBody);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(capR, 4.0f, 4.0f), brush_.Get());
        brush_->SetColor(kColKeyBorder);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(capR, 4.0f, 4.0f), brush_.Get(), 1.0f);

        DrawTextLine(parts[i], fmtKeycap_.Get(), capR, kColText,
            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        x += w + gap;
    }
}

void SettingsWindow::DrawMicaBackdrop() {
    rt_->Clear(kColMicaBase);

    auto paintGlow = [&](float cx, float cy, float rx, float ry, float peakAlpha) {
        D2D1_GRADIENT_STOP stops[2] = {
            { 0.0f, C(0xFFFFFF, peakAlpha) },
            { 1.0f, C(0xFFFFFF, 0.0f) }
        };
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> col;
        Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> rad;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &col)) && col &&
            SUCCEEDED(rt_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0.0f, 0.0f), rx, ry),
                col.Get(), &rad)) && rad) {
            rt_->FillRectangle(D2D1::RectF(0.0f, 0.0f, kWndW, kWndH), rad.Get());
        }
    };

    paintGlow(120.0f, 64.0f, 440.0f, 300.0f, 0.032f);
    paintGlow(860.0f, 560.0f, 480.0f, 320.0f, 0.025f);
}

void SettingsWindow::DrawTitleBar() {
    D2D1_COLOR_F dotCol = kColTextMuted;
    if (status_.state == EngineState::Running) dotCol = kColGreen;
    else if (status_.state == EngineState::Recovering || status_.state == EngineState::Starting) dotCol = kColAmber;
    else if (status_.state == EngineState::Faulted) dotCol = kColRed;

    brush_->SetColor(dotCol);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(20.0f, 20.0f), 3.5f, 3.5f), brush_.Get());

    DrawTextLine(L"Clipper", fmtBrand_.Get(), D2D1::RectF(34.0f, 0.0f, 88.0f, kTitleBarH), C(0xE4E4E7));

    std::wstring durStr = FormatReplayLengthShort(appliedCfg_.replaySeconds);
    wchar_t specBuf[96] = {};
    if (status_.state == EngineState::Faulted && !status_.lastError.empty()) {
        std::wstring errW = Utf8ToWide(status_.lastError);
        DrawTextLine(errW, fmtMono_.Get(), D2D1::RectF(92.0f, 0.0f, 760.0f, kTitleBarH), kColRed);
    } else if (status_.video.width > 0 && status_.video.height > 0) {
        std::swprintf(specBuf, sizeof(specBuf) / sizeof(wchar_t), L"%up · %u fps · %ls",
            status_.video.height, appliedCfg_.fps, durStr.c_str());
        DrawTextLine(specBuf, fmtMono_.Get(), D2D1::RectF(92.0f, 0.0f, 680.0f, kTitleBarH), kColTextMuted);
    } else {
        std::swprintf(specBuf, sizeof(specBuf) / sizeof(wchar_t), L"%u fps · %ls",
            appliedCfg_.fps, durStr.c_str());
        DrawTextLine(specBuf, fmtMono_.Get(), D2D1::RectF(92.0f, 0.0f, 680.0f, kTitleBarH), kColTextMuted);
    }

    D2D1_RECT_F saveRect = D2D1::RectF(814.0f, 6.0f, 906.0f, 34.0f);
    DrawFluentButton(saveRect, L"Save clip", L"\uE945", HitKey{ HitAction::TitleSaveClip, 0 }, false);

    D2D1_RECT_F minRect = D2D1::RectF(916.0f, 0.0f, 958.0f, kTitleBarH);
    HitKey minKey{ HitAction::WinMinimize, 0 };
    if (hoveredHit_ == minKey) {
        brush_->SetColor(C(0xFFFFFF, 0.08f));
        rt_->FillRectangle(minRect, brush_.Get());
    }
    DrawGlyph(L"\uE921", 10.0f, minRect, hoveredHit_ == minKey ? kColText : kColTextMuted);
    hitTargets_.push_back({ minRect, minKey, false });

    D2D1_RECT_F closeRect = D2D1::RectF(958.0f, 0.0f, kWndW, kTitleBarH);
    HitKey closeKey{ HitAction::WinClose, 0 };
    if (hoveredHit_ == closeKey) {
        brush_->SetColor(kColCloseHover);
        rt_->FillRectangle(closeRect, brush_.Get());
    }
    DrawGlyph(L"\uE8BB", 10.0f, closeRect, hoveredHit_ == closeKey ? kColText : kColTextMuted);
    hitTargets_.push_back({ closeRect, closeKey, false });
}

void SettingsWindow::DrawNavRail() {
    struct RailDef {
        SettingsTab tab;
        const wchar_t* glyph;
    };
    const RailDef tabs[] = {
        { SettingsTab::Clips, L"\uE714" },
        { SettingsTab::Settings, L"\uE713" },
    };

    float y = 48.0f;
    for (const auto& t : tabs) {
        D2D1_RECT_F r = D2D1::RectF(6.0f, y, kRailW - 6.0f, y + 38.0f);
        HitKey key{ HitAction::SelectTab, static_cast<int>(t.tab) };
        bool active = (activeTab_ == t.tab);
        bool hovered = (hoveredHit_ == key);

        if (active || hovered) {
            brush_->SetColor(active ? C(0xFFFFFF, 0.07f) : C(0xFFFFFF, 0.055f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 5.0f, 5.0f), brush_.Get());
        }
        if (active) {
            brush_->SetColor(kColAccent);
            rt_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(7.0f, y + 11.0f, 10.0f, y + 27.0f), 1.5f, 1.5f),
                brush_.Get());
        }

        DrawGlyph(t.glyph, 16.0f, r, active ? kColText : (hovered ? kColTextSec : kColTextMuted));
        hitTargets_.push_back({ r, key, false });
        y += 44.0f;
    }

    auto drawBottomBtn = [&](float topY, const wchar_t* glyph, HitAction action) {
        D2D1_RECT_F r = D2D1::RectF(6.0f, topY, kRailW - 6.0f, topY + 38.0f);
        HitKey key{ action, 0 };
        bool hovered = (hoveredHit_ == key);
        if (hovered) {
            brush_->SetColor(C(0xFFFFFF, 0.055f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 5.0f, 5.0f), brush_.Get());
        }
        DrawGlyph(glyph, 16.0f, r, hovered ? kColText : kColTextMuted);
        hitTargets_.push_back({ r, key, false });
    };

    drawBottomBtn(kWndH - 92.0f, L"\uE89F", HitAction::RailHide);
    drawBottomBtn(kWndH - 50.0f, L"\uE7E8", HitAction::RailQuit);
}

void SettingsWindow::DrawPageHeader() {
    if (activeTab_ == SettingsTab::Clips) {
        DrawTextLine(L"Recent clips", fmtPageTitle_.Get(),
            D2D1::RectF(kContentX, 46.0f, kContentX + 160.0f, 82.0f), kColText);

        wchar_t sub[96] = {};
        if (clips_.empty()) {
            std::swprintf(sub, sizeof(sub) / sizeof(wchar_t), L"No saved files");
        } else {
            std::wstring szText = FormatBytesHuman(totalClipsBytes_);
            std::swprintf(sub, sizeof(sub) / sizeof(wchar_t), L"%zu %ls · %ls",
                clips_.size(), clips_.size() == 1 ? L"clip" : L"clips", szText.c_str());
        }
        DrawTextLine(sub, fmtBody_.Get(),
            D2D1::RectF(kContentX + 136.0f, 52.0f, 700.0f, 80.0f), kColTextMuted);

        DrawFluentButton(D2D1::RectF(774.0f, 48.0f, 858.0f, 78.0f),
            L"Refresh", L"\uE72C", HitKey{ HitAction::ClipsRefresh, 0 }, false);
        DrawFluentButton(D2D1::RectF(866.0f, 48.0f, kWndW - kContentPadR, 78.0f),
            L"Open folder", L"\uE838", HitKey{ HitAction::ClipsOpenFolder, 0 }, false);
    } else {
        DrawTextLine(L"Settings", fmtPageTitle_.Get(),
            D2D1::RectF(kContentX, 46.0f, kContentX + 110.0f, 82.0f), kColText);
        DrawTextLine(L"DXGI · H.264 NVENC", fmtMono_.Get(),
            D2D1::RectF(kContentX + 100.0f, 52.0f, 520.0f, 80.0f), kColTextMuted);

        DrawFluentButton(D2D1::RectF(832.0f, 48.0f, kWndW - kContentPadR, 78.0f),
            L"Refresh displays", L"\uE72C", HitKey{ HitAction::RefreshMonitors, 0 }, false);
    }
}

void SettingsWindow::DrawClipsTab(float contentX, float contentW, float& yCursor) {
    if (clips_.empty()) {
        D2D1_RECT_F card = D2D1::RectF(contentX + 200.0f, yCursor + 86.0f, contentX + contentW - 200.0f, yCursor + 346.0f);
        DrawFluentSurface(card, 8.0f, false, false);

        float cx = (card.left + card.right) * 0.5f;
        DrawGlyph(L"\uE714", 30.0f, D2D1::RectF(cx - 24.0f, card.top + 38.0f, cx + 24.0f, card.top + 82.0f), kColTextMuted);
        DrawTextLine(L"No clips saved yet", fmtPageTitle_.Get(),
            D2D1::RectF(card.left + 20.0f, card.top + 92.0f, card.right - 20.0f, card.top + 124.0f),
            kColText, DWRITE_TEXT_ALIGNMENT_CENTER);

        std::wstring hk = Utf8ToWide(appliedCfg_.hotkeySave.display);
        std::wstring line = L"Press " + hk + L" during gameplay to save a replay.";
        DrawTextLine(line, fmtBody_.Get(),
            D2D1::RectF(card.left + 24.0f, card.top + 132.0f, card.right - 24.0f, card.top + 156.0f),
            kColTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);

        DrawFluentButton(D2D1::RectF(cx - 122.0f, card.top + 184.0f, cx - 6.0f, card.top + 218.0f),
            L"Save clip", L"\uE945", HitKey{ HitAction::TitleSaveClip, 0 }, true, false, true);
        DrawFluentButton(D2D1::RectF(cx + 6.0f, card.top + 184.0f, cx + 122.0f, card.top + 218.0f),
            L"Settings", L"\uE713", HitKey{ HitAction::EmptyGoSettings, 0 }, false, false, true);

        yCursor += 360.0f;
        return;
    }

    float colGap = 16.0f;
    float cardW = (contentW - colGap * 2.0f) / 3.0f;
    float thumbH = std::floor(cardW * 9.0f / 16.0f);
    float metaH = 48.0f;
    float cardH = thumbH + metaH;
    float rowGap = 18.0f;

    float baseTop = yCursor + 6.0f;
    for (size_t i = 0; i < clips_.size(); ++i) {
        size_t col = i % 3;
        size_t row = i / 3;
        float x = contentX + static_cast<float>(col) * (cardW + colGap);
        float y = baseTop + static_cast<float>(row) * (cardH + rowGap) - scrollY_;

        if (y + cardH < kViewTop - 20.0f || y > kViewBottom + 20.0f) continue;

        const ClipEntry& clip = clips_[i];
        D2D1_RECT_F thumbR = D2D1::RectF(x, y, x + cardW, y + thumbH);
        D2D1_ROUNDED_RECT thumbRR = D2D1::RoundedRect(thumbR, 8.0f, 8.0f);

        HitKey playKey{ HitAction::ClipPlay, static_cast<int>(i) };
        HitKey folderKey{ HitAction::ClipShowInFolder, static_cast<int>(i) };
        HitKey deleteKey{ HitAction::ClipDelete, static_cast<int>(i) };
        bool thumbHovered = (hoveredHit_ == playKey);

        auto thumbIt = thumbCache_.find(clip.fullPath);
        ID2D1Bitmap* bmp = (thumbIt != thumbCache_.end()) ? thumbIt->second.bmp.Get() : nullptr;

        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        Microsoft::WRL::ComPtr<ID2D1Layer> clipLayer;
        if (d2dFactory_ &&
            SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(thumbRR, &clipGeo)) &&
            SUCCEEDED(rt_->CreateLayer(&clipLayer))) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(thumbR, clipGeo.Get());
            rt_->PushLayer(&lp, clipLayer.Get());

            brush_->SetColor(C(0x141518));
            rt_->FillRectangle(thumbR, brush_.Get());

            if (bmp) {
                D2D1_SIZE_F bSz = bmp->GetSize();
                float bw = std::max(1.0f, bSz.width);
                float bh = std::max(1.0f, bSz.height);
                float targetAspect = cardW / thumbH;
                float srcAspect = bw / bh;
                D2D1_RECT_F srcR{ 0.0f, 0.0f, bw, bh };
                if (srcAspect > targetAspect) {
                    float cropW = bh * targetAspect;
                    srcR.left = (bw - cropW) * 0.5f;
                    srcR.right = srcR.left + cropW;
                } else if (srcAspect < targetAspect) {
                    float cropH = bw / targetAspect;
                    srcR.top = (bh - cropH) * 0.5f;
                    srcR.bottom = srcR.top + cropH;
                }
                rt_->DrawBitmap(bmp, thumbR, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcR);
            } else {
                DrawGlyph(L"\uE714", 24.0f, thumbR, C(0x52525B));
            }

            if (thumbHovered) {
                brush_->SetColor(C(0x0E0F11, 0.45f));
                rt_->FillRectangle(thumbR, brush_.Get());
                float tcx = (thumbR.left + thumbR.right) * 0.5f;
                float tcy = (thumbR.top + thumbR.bottom) * 0.5f;
                brush_->SetColor(C(0x141518, 0.85f));
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tcx, tcy), 20.0f, 20.0f), brush_.Get());
                brush_->SetColor(C(0xFFFFFF, 0.25f));
                rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(tcx, tcy), 20.0f, 20.0f), brush_.Get(), 1.0f);
                DrawGlyph(L"\uE768", 14.0f, D2D1::RectF(tcx - 10.0f, tcy - 10.0f, tcx + 12.0f, tcy + 10.0f), kColText);
            }

            rt_->PopLayer();
        }

        std::wstring badgeTxt = FormatDurationShort(clip.durationSec);
        if (badgeTxt.empty()) badgeTxt = FormatBytesHuman(clip.sizeBytes);
        float badgeW = std::max(40.0f, static_cast<float>(badgeTxt.size()) * 7.4f + 12.0f);
        D2D1_RECT_F badgeR = D2D1::RectF(
            thumbR.right - badgeW - 8.0f, thumbR.bottom - 24.0f,
            thumbR.right - 8.0f, thumbR.bottom - 8.0f);
        brush_->SetColor(C(0x0E0F11, 0.82f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(badgeR, 4.0f, 4.0f), brush_.Get());
        DrawTextLine(badgeTxt, fmtMono_.Get(), badgeR, kColAccent, DWRITE_TEXT_ALIGNMENT_CENTER);

        hitTargets_.push_back({ thumbR, playKey, true });

        float textRight = thumbR.right - 60.0f;
        DrawTextLine(clip.displayTitle, fmtBodySemi_.Get(),
            D2D1::RectF(x + 2.0f, thumbR.bottom + 8.0f, textRight, thumbR.bottom + 26.0f), kColText);

        std::wstring meta = clip.dateLabel;
        if (!meta.empty()) meta += L" · ";
        meta += FormatBytesHuman(clip.sizeBytes);
        DrawTextLine(meta, fmtSmall_.Get(),
            D2D1::RectF(x + 2.0f, thumbR.bottom + 27.0f, textRight, thumbR.bottom + 44.0f), kColTextMuted);

        auto drawBareAct = [&](D2D1_RECT_F r, const wchar_t* glyph, HitKey k, bool danger) {
            bool h = (hoveredHit_ == k);
            if (h) {
                brush_->SetColor(danger ? C(0xEF4444, 0.16f) : C(0xFFFFFF, 0.08f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 4.0f, 4.0f), brush_.Get());
            }
            DrawGlyph(glyph, 13.0f, r, h ? (danger ? kColRed : kColText) : kColTextMuted);
            hitTargets_.push_back({ r, k, true });
        };

        drawBareAct(D2D1::RectF(thumbR.right - 56.0f, thumbR.bottom + 12.0f, thumbR.right - 30.0f, thumbR.bottom + 38.0f),
            L"\uE8B7", folderKey, false);
        drawBareAct(D2D1::RectF(thumbR.right - 26.0f, thumbR.bottom + 12.0f, thumbR.right, thumbR.bottom + 38.0f),
            L"\uE74D", deleteKey, true);
    }

    size_t rows = (clips_.size() + 2) / 3;
    yCursor += 6.0f + static_cast<float>(rows) * cardH + static_cast<float>(rows > 0 ? rows - 1 : 0) * rowGap + 14.0f;
}

void SettingsWindow::DrawSettingsTab(float contentX, float contentW, float& yCursor) {
    float y = yCursor + 6.0f - scrollY_;
    float rightEdge = contentX + contentW - 16.0f;
    float gap = 6.0f;

    auto drawRowHeader = [&](float rowTop, float rowH, const wchar_t* glyph, const wchar_t* title) {
        DrawFluentSurface(D2D1::RectF(contentX, rowTop, contentX + contentW, rowTop + rowH), 6.0f, false, false);
        DrawGlyph(glyph, 17.0f, D2D1::RectF(contentX + 16.0f, rowTop, contentX + 38.0f, rowTop + rowH), C(0xE4E4E7));
        DrawTextLine(title, fmtRowTitle_.Get(),
            D2D1::RectF(contentX + 50.0f, rowTop, contentX + 320.0f, rowTop + rowH), kColText);
    };

    size_t monCount = monitors_.empty() ? 1 : monitors_.size();
    size_t monCols = 2;
    size_t monRows = (monCount + monCols - 1) / monCols;
    float tileW = 196.0f;
    float tileH = 50.0f;
    float tileGapX = 8.0f;
    float tileGapY = 8.0f;
    float row1H = std::max(70.0f, 20.0f + static_cast<float>(monRows) * tileH + static_cast<float>(monRows > 0 ? monRows - 1 : 0) * tileGapY);

    drawRowHeader(y, row1H, L"\uE7F4", L"Display source");
    for (size_t i = 0; i < monCount; ++i) {
        size_t col = i % monCols;
        size_t row = i / monCols;
        size_t colsInRow = (row == monRows - 1 && (monCount % monCols) != 0) ? (monCount % monCols) : monCols;
        float rowTilesW = static_cast<float>(colsInRow) * tileW + static_cast<float>(colsInRow - 1) * tileGapX;
        float tx = (rightEdge - rowTilesW) + static_cast<float>(col) * (tileW + tileGapX);
        float ty = y + 10.0f + static_cast<float>(row) * (tileH + tileGapY);

        uint32_t monIdx = monitors_.empty() ? 0 : monitors_[i].index;
        bool selected = (editedCfg_.monitorIndex == monIdx);
        HitKey key{ HitAction::SelectMonitor, static_cast<int>(monIdx) };
        bool hovered = (hoveredHit_ == key);
        D2D1_RECT_F tr = D2D1::RectF(tx, ty, tx + tileW, ty + tileH);

        DrawFluentSurface(tr, 5.0f, hovered, selected);

        wchar_t labelBuf[128] = {};
        std::wstring friendly = monitors_.empty() ? L"Primary Display" : monitors_[i].friendlyName;
        if (friendly.empty()) friendly = L"Display";
        std::swprintf(labelBuf, sizeof(labelBuf) / sizeof(wchar_t), L"%u. %ls", monIdx + 1, friendly.c_str());
        DrawTextLine(labelBuf, fmtBodySemi_.Get(), D2D1::RectF(tx + 11.0f, ty + 6.0f, tx + tileW - 10.0f, ty + 25.0f), kColText);

        wchar_t metaBuf[64] = {};
        if (!monitors_.empty() && monitors_[i].width > 0 && monitors_[i].height > 0) {
            if (monitors_[i].refreshHz > 0) {
                std::swprintf(metaBuf, sizeof(metaBuf) / sizeof(wchar_t), L"%ux%u · %uHz",
                    monitors_[i].width, monitors_[i].height, monitors_[i].refreshHz);
            } else {
                std::swprintf(metaBuf, sizeof(metaBuf) / sizeof(wchar_t), L"%ux%u",
                    monitors_[i].width, monitors_[i].height);
            }
        } else {
            std::swprintf(metaBuf, sizeof(metaBuf) / sizeof(wchar_t), L"DXGI Output %u", monIdx + 1);
        }
        DrawTextLine(metaBuf, fmtMono_.Get(), D2D1::RectF(tx + 11.0f, ty + 25.0f, tx + tileW - 10.0f, ty + 43.0f), kColTextMuted);
        hitTargets_.push_back({ tr, key, true });
    }
    y += row1H + gap;

    float rowH = 56.0f;
    drawRowHeader(y, rowH, L"\uE714", L"Frame rate");
    {
        float segW = 46.0f;
        float capW = static_cast<float>(std::size(kFpsPresets)) * segW + 6.0f;
        float capRight = rightEdge - 106.0f;
        D2D1_RECT_F capR = D2D1::RectF(capRight - capW, y + 12.0f, capRight, y + 44.0f);
        brush_->SetColor(kColWell);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(capR, 5.0f, 5.0f), brush_.Get());
        brush_->SetColor(kColBorder);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(capR, 5.0f, 5.0f), brush_.Get(), 1.0f);

        for (size_t i = 0; i < std::size(kFpsPresets); ++i) {
            uint32_t fps = kFpsPresets[i];
            D2D1_RECT_F sr = D2D1::RectF(
                capR.left + 3.0f + static_cast<float>(i) * segW, capR.top + 3.0f,
                capR.left + 3.0f + static_cast<float>(i + 1) * segW, capR.bottom - 3.0f);
            HitKey key{ HitAction::SelectFpsPreset, static_cast<int>(fps) };
            bool active = (editedCfg_.fps == fps);
            bool hov = (hoveredHit_ == key);
            if (active || hov) {
                brush_->SetColor(active ? C(0xFFFFFF, 0.14f) : C(0xFFFFFF, 0.06f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(sr, 4.0f, 4.0f), brush_.Get());
            }
            std::wstring txt = std::to_wstring(fps);
            DrawTextLine(txt, active ? fmtBodySemi_.Get() : fmtBody_.Get(), sr,
                active ? kColText : kColTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            hitTargets_.push_back({ sr, key, true });
        }

        DrawEditBox(D2D1::RectF(rightEdge - 94.0f, y + 12.0f, rightEdge - 30.0f, y + 44.0f),
            std::to_wstring(editedCfg_.fps), InputMode::EditFps, HitKey{ HitAction::EditFpsBox, 0 });
        DrawTextLine(L"fps", fmtSmall_.Get(), D2D1::RectF(rightEdge - 25.0f, y, rightEdge + 4.0f, y + rowH), kColTextMuted);
    }
    y += rowH + gap;

    drawRowHeader(y, rowH, L"\uE916", L"Replay length");
    {
        float segW = 44.0f;
        float capW = static_cast<float>(std::size(kReplayPresetsSec)) * segW + 6.0f;
        float capRight = rightEdge - 182.0f;
        D2D1_RECT_F capR = D2D1::RectF(capRight - capW, y + 12.0f, capRight, y + 44.0f);
        brush_->SetColor(kColWell);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(capR, 5.0f, 5.0f), brush_.Get());
        brush_->SetColor(kColBorder);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(capR, 5.0f, 5.0f), brush_.Get(), 1.0f);

        for (size_t i = 0; i < std::size(kReplayPresetsSec); ++i) {
            uint32_t sec = kReplayPresetsSec[i];
            D2D1_RECT_F sr = D2D1::RectF(
                capR.left + 3.0f + static_cast<float>(i) * segW, capR.top + 3.0f,
                capR.left + 3.0f + static_cast<float>(i + 1) * segW, capR.bottom - 3.0f);
            HitKey key{ HitAction::SelectReplayPreset, static_cast<int>(sec) };
            bool active = (editedCfg_.replaySeconds == sec);
            bool hov = (hoveredHit_ == key);
            if (active || hov) {
                brush_->SetColor(active ? C(0xFFFFFF, 0.14f) : C(0xFFFFFF, 0.06f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(sr, 4.0f, 4.0f), brush_.Get());
            }
            std::wstring lbl = FormatReplayLengthShort(sec);
            DrawTextLine(lbl, active ? fmtBodySemi_.Get() : fmtBody_.Get(), sr,
                active ? kColText : kColTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            hitTargets_.push_back({ sr, key, true });
        }

        std::wstring customVal = FormatReplayBoxValue(editedCfg_.replaySeconds, replayUnitMinutes_);
        DrawEditBox(D2D1::RectF(rightEdge - 170.0f, y + 12.0f, rightEdge - 96.0f, y + 44.0f),
            customVal, InputMode::EditReplay, HitKey{ HitAction::EditReplayBox, 0 });

        D2D1_RECT_F unitR = D2D1::RectF(rightEdge - 88.0f, y + 12.0f, rightEdge, y + 44.0f);
        brush_->SetColor(kColWell);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(unitR, 5.0f, 5.0f), brush_.Get());
        brush_->SetColor(kColBorder);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(unitR, 5.0f, 5.0f), brush_.Get(), 1.0f);

        float uw = (unitR.right - unitR.left - 6.0f) * 0.5f;
        const wchar_t* uLabels[2] = { L"sec", L"min" };
        for (int ui = 0; ui < 2; ++ui) {
            D2D1_RECT_F ur = D2D1::RectF(
                unitR.left + 3.0f + static_cast<float>(ui) * uw, unitR.top + 3.0f,
                unitR.left + 3.0f + static_cast<float>(ui + 1) * uw, unitR.bottom - 3.0f);
            HitKey uk{ HitAction::ToggleReplayUnit, ui };
            bool active = ((ui == 1) == replayUnitMinutes_);
            if (active || hoveredHit_ == uk) {
                brush_->SetColor(active ? C(0xFFFFFF, 0.14f) : C(0xFFFFFF, 0.06f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(ur, 4.0f, 4.0f), brush_.Get());
            }
            DrawTextLine(uLabels[ui], fmtSmall_.Get(), ur, active ? kColText : kColTextMuted,
                DWRITE_TEXT_ALIGNMENT_CENTER);
            hitTargets_.push_back({ ur, uk, true });
        }
    }
    y += rowH + gap;

    drawRowHeader(y, rowH, L"\uE9E9", L"Video bitrate");
    {
        float trLeft = rightEdge - 332.0f;
        float trRight = rightEdge - 108.0f;
        float trCy = y + rowH * 0.5f;
        sliderTrackRect_ = D2D1::RectF(trLeft, trCy - 14.0f, trRight, trCy + 14.0f);

        brush_->SetColor(C(0xFFFFFF, 0.14f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trLeft, trCy - 2.0f, trRight, trCy + 2.0f), 2.0f, 2.0f),
            brush_.Get());

        float t = ClampF((static_cast<float>(editedCfg_.bitrateMbps) - 5.0f) / 75.0f, 0.0f, 1.0f);
        float knobX = trLeft + t * (trRight - trLeft);
        if (knobX > trLeft + 2.0f) {
            brush_->SetColor(kColAccent);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trLeft, trCy - 2.0f, knobX, trCy + 2.0f), 2.0f, 2.0f),
                brush_.Get());
        }

        bool sliderActive = draggingSlider_ || (hoveredHit_.action == HitAction::BitrateSlider);
        brush_->SetColor(C(0x3A3B40));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, trCy), sliderActive ? 9.0f : 8.0f, sliderActive ? 9.0f : 8.0f), brush_.Get());
        brush_->SetColor(kColAccent);
        rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, trCy), sliderActive ? 7.5f : 6.5f, sliderActive ? 7.5f : 6.5f), brush_.Get(), 3.2f);
        hitTargets_.push_back({ sliderTrackRect_, HitKey{ HitAction::BitrateSlider, 0 }, true });

        DrawEditBox(D2D1::RectF(rightEdge - 94.0f, y + 12.0f, rightEdge - 42.0f, y + 44.0f),
            std::to_wstring(editedCfg_.bitrateMbps), InputMode::EditBitrate, HitKey{ HitAction::EditBitrateBox, 0 });
        DrawTextLine(L"Mbps", fmtSmall_.Get(), D2D1::RectF(rightEdge - 36.0f, y, rightEdge + 4.0f, y + rowH), kColTextMuted);
    }
    y += rowH + gap;

    drawRowHeader(y, rowH, L"\uE767", L"Audio & alerts");
    {
        struct ToggleItem {
            const wchar_t* label;
            bool checked;
            HitAction action;
            float labelW;
        };
        const ToggleItem items[3] = {
            { L"Desktop", editedCfg_.recordDesktopAudio, HitAction::ToggleDesktopAudio, 54.0f },
            { L"Mic", editedCfg_.recordMicAudio, HitAction::ToggleMicAudio, 28.0f },
            { L"Chime", editedCfg_.saveChime, HitAction::ToggleChime, 42.0f },
        };
        float xPos = rightEdge;
        for (int i = 2; i >= 0; --i) {
            D2D1_RECT_F togR = D2D1::RectF(xPos - 40.0f, y + 18.0f, xPos, y + 38.0f);
            HitKey tk{ items[i].action, 0 };
            bool hov = (hoveredHit_ == tk);
            DrawToggleSwitch(togR, items[i].checked, hov);
            D2D1_RECT_F lblR = D2D1::RectF(togR.left - items[i].labelW - 8.0f, y, togR.left - 8.0f, y + rowH);
            DrawTextLine(items[i].label, fmtBody_.Get(), lblR, kColTextSec, DWRITE_TEXT_ALIGNMENT_TRAILING);
            hitTargets_.push_back({ D2D1::RectF(lblR.left, y + 8.0f, togR.right, y + rowH - 8.0f), tk, true });
            xPos = lblR.left - 24.0f;
        }
    }
    y += rowH + gap;

    float row6H = 66.0f;
    DrawFluentSurface(D2D1::RectF(contentX, y, contentX + contentW, y + row6H), 6.0f, false, false);
    DrawGlyph(L"\uE765", 17.0f, D2D1::RectF(contentX + 16.0f, y, contentX + 38.0f, y + row6H), C(0xE4E4E7));
    DrawTextLine(L"Save hotkey & clips folder", fmtRowTitle_.Get(),
        D2D1::RectF(contentX + 50.0f, y + 12.0f, contentX + 430.0f, y + 32.0f), kColText);

    std::wstring dirPath = editedCfg_.outputDir.empty() ? ExpandEnvPath(editedCfg_.outputDirRaw) : editedCfg_.outputDir;
    DrawTextLine(dirPath, fmtMono_.Get(),
        D2D1::RectF(contentX + 50.0f, y + 34.0f, contentX + 430.0f, y + 52.0f), kColTextMuted);

    D2D1_RECT_F openR = D2D1::RectF(rightEdge - 52.0f, y + 18.0f, rightEdge, y + 48.0f);
    D2D1_RECT_F browseR = D2D1::RectF(openR.left - 70.0f, y + 18.0f, openR.left - 6.0f, y + 48.0f);
    D2D1_RECT_F resetHkR = D2D1::RectF(browseR.left - 60.0f, y + 18.0f, browseR.left - 6.0f, y + 48.0f);
    D2D1_RECT_F changeR = D2D1::RectF(resetHkR.left - 70.0f, y + 18.0f, resetHkR.left - 6.0f, y + 48.0f);

    if (inputMode_ == InputMode::CaptureHotkey) {
        D2D1_RECT_F listenR = D2D1::RectF(changeR.left - 186.0f, y + 18.0f, changeR.left - 8.0f, y + 48.0f);
        brush_->SetColor(C(0xFFFFFF, 0.08f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(listenR, 5.0f, 5.0f), brush_.Get());
        brush_->SetColor(kColAccent);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(listenR, 5.0f, 5.0f), brush_.Get(), 1.2f);
        DrawTextLine(L"Press shortcut... [Esc]", fmtSmall_.Get(), listenR, kColText, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        DrawKeycapGroupRight(changeR.left - 10.0f, y + row6H * 0.5f, editedCfg_.hotkeySave.display, contentX + 260.0f);
    }

    DrawFluentButton(changeR, inputMode_ == InputMode::CaptureHotkey ? L"Cancel" : L"Change", nullptr,
        HitKey{ HitAction::HotkeyCaptureBtn, 0 }, false, false, true);
    DrawFluentButton(resetHkR, L"Reset", nullptr, HitKey{ HitAction::HotkeyResetBtn, 0 }, false, false, true);
    DrawFluentButton(browseR, L"Browse", nullptr, HitKey{ HitAction::BrowseFolder, 0 }, false, false, true);
    DrawFluentButton(openR, L"Open", nullptr, HitKey{ HitAction::OpenFolderBtn, 0 }, false, false, true);

    y += row6H + 14.0f;
    yCursor += (y - (yCursor - scrollY_));
}

void SettingsWindow::DrawBottomBar() {
    bool dirty = IsDirty();

    if (!feedbackText_.empty()) {
        D2D1_COLOR_F fbCol = feedbackIsError_ ? kColRed : kColGreen;
        brush_->SetColor(fbCol);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kContentX + 5.0f, kViewBottom + 28.0f), 3.5f, 3.5f), brush_.Get());
        DrawTextLine(feedbackText_, fmtBody_.Get(),
            D2D1::RectF(kContentX + 16.0f, kViewBottom, 760.0f, kWndH), fbCol);
    } else if (activeTab_ == SettingsTab::Settings || dirty) {
        double estMb = 0.0;
        bool overCap = false;
        uint32_t clampedSecs = 0;
        ComputeRamEstimate(estMb, overCap, clampedSecs);

        std::wstring ramStr = FormatRamHuman(estMb);
        std::wstring line = L"Estimated ring buffer: " + ramStr + L" in RAM";
        if (overCap) {
            line = L"Ring buffer exceeds 8 GB cap (clamps replay to " + FormatReplayLengthShort(clampedSecs) + L" on apply)";
        }
        DrawTextLine(line, fmtBody_.Get(),
            D2D1::RectF(kContentX, kViewBottom, 760.0f, kWndH),
            overCap ? kColAmber : kColTextMuted);
    } else {
        std::wstring line = L"Hotkey: " + Utf8ToWide(appliedCfg_.hotkeySave.display) + L"  ·  " +
            (appliedCfg_.outputDir.empty() ? ExpandEnvPath(appliedCfg_.outputDirRaw) : appliedCfg_.outputDir);
        DrawTextLine(line, fmtSmall_.Get(), D2D1::RectF(kContentX, kViewBottom, 940.0f, kWndH), kColTextMuted);
    }

    if (activeTab_ == SettingsTab::Settings || dirty) {
        if (dirty) {
            DrawFluentButton(D2D1::RectF(774.0f, kViewBottom + 12.0f, 848.0f, kWndH - 14.0f),
                L"Reset", nullptr, HitKey{ HitAction::DiscardChanges, 0 }, false);
        }
        DrawFluentButton(D2D1::RectF(856.0f, kViewBottom + 12.0f, kWndW - kContentPadR, kWndH - 14.0f),
            L"Apply changes", nullptr, HitKey{ HitAction::ApplyChanges, 0 }, true, !dirty);
    }
}

void SettingsWindow::RenderToDC(HDC targetHdc) {
    if (!targetHdc || !hwnd_ || !EnsureDeviceResources()) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (!EnsureBackbuffer(w, h)) return;

    if (activeTab_ == SettingsTab::Clips && !clips_.empty()) {
        bool needAnyThumb = false;
        for (const auto& c : clips_) {
            auto it = thumbCache_.find(c.fullPath);
            if (it == thumbCache_.end() || !it->second.attempted) {
                needAnyThumb = true;
                break;
            }
        }
        if (needAnyThumb) {
            StepThumbnailBatch(6, false);
        }
    }

    rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(rt_->BindDC(memDC_, &rc))) return;

    hitTargets_.clear();

    rt_->BeginDraw();
    DrawMicaBackdrop();
    DrawTitleBar();
    DrawNavRail();
    DrawPageHeader();

    D2D1_RECT_F viewRect = D2D1::RectF(kRailW, kViewTop, kWndW, kViewBottom);
    rt_->PushAxisAlignedClip(viewRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float yCursor = kViewTop;
    if (activeTab_ == SettingsTab::Clips) {
        DrawClipsTab(kContentX, kContentW, yCursor);
    } else {
        DrawSettingsTab(kContentX, kContentW, yCursor);
    }

    float totalHeight = yCursor - kViewTop;
    maxScroll_ = std::max(0.0f, totalHeight - kViewH);
    scrollY_ = ClampF(scrollY_, 0.0f, maxScroll_);

    if (maxScroll_ > 1.0f) {
        float trackTop = kViewTop + 8.0f;
        float trackH = kViewH - 16.0f;
        float thumbH = std::max(36.0f, trackH * (kViewH / totalHeight));
        float thumbY = trackTop + (trackH - thumbH) * (scrollY_ / maxScroll_);
        brush_->SetColor(C(0xFFFFFF, 0.18f));
        rt_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(kWndW - 10.0f, thumbY, kWndW - 5.0f, thumbY + thumbH), 2.5f, 2.5f),
            brush_.Get());
    }

    rt_->PopAxisAlignedClip();
    DrawBottomBar();

    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        InvalidateWindow();
        return;
    }

    BitBlt(targetHdc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
}

const SettingsWindow::HitTarget* SettingsWindow::HitTestDip(float dipX, float dipY) const {
    for (auto it = hitTargets_.rbegin(); it != hitTargets_.rend(); ++it) {
        if (it->inViewport && (dipY < kViewTop || dipY > kViewBottom)) continue;
        if (PtInDipRect(it->rect, dipX, dipY)) return &(*it);
    }
    return nullptr;
}

void SettingsWindow::EnterInputMode(InputMode nextMode) {
    if (inputMode_ == nextMode) return;
    CommitOrCancelInput(true);

    inputMode_ = nextMode;
    editBuffer_.clear();
    replaceOnFirstChar_ = false;

    if (nextMode == InputMode::EditFps) {
        editBuffer_ = std::to_wstring(editedCfg_.fps);
        replaceOnFirstChar_ = true;
    } else if (nextMode == InputMode::EditReplay) {
        editBuffer_ = FormatReplayBoxValue(editedCfg_.replaySeconds, replayUnitMinutes_);
        replaceOnFirstChar_ = true;
    } else if (nextMode == InputMode::EditBitrate) {
        editBuffer_ = std::to_wstring(editedCfg_.bitrateMbps);
        replaceOnFirstChar_ = true;
    } else if (nextMode == InputMode::CaptureHotkey) {
        if (callbacks_.onSuspendHotkey) callbacks_.onSuspendHotkey(true);
        ShowFeedback(L"Press your shortcut combination, or Esc to cancel.", false);
    }
    InvalidateWindow();
}

void SettingsWindow::CommitOrCancelInput(bool commit, bool showCancelFeedback) {
    InputMode cur = inputMode_;
    if (cur == InputMode::None) return;

    inputMode_ = InputMode::None;
    replaceOnFirstChar_ = false;

    if (cur == InputMode::CaptureHotkey) {
        if (callbacks_.onSuspendHotkey) callbacks_.onSuspendHotkey(false);
        if (!commit && showCancelFeedback) {
            ShowFeedback(L"Hotkey capture canceled.", false);
        } else {
            InvalidateWindow();
        }
        return;
    }

    if (commit && !editBuffer_.empty()) {
        if (cur == InputMode::EditFps) {
            uint32_t fps = 0;
            if (ParseStrictPositiveUint(editBuffer_, fps)) {
                editedCfg_.fps = std::clamp(fps, kMinFps, kMaxFps);
            }
        } else if (cur == InputMode::EditReplay) {
            uint32_t secs = 0;
            if (ParseDurationInput(editBuffer_, replayUnitMinutes_, secs)) {
                editedCfg_.replaySeconds = secs;
                if (editBuffer_.find(L'm') != std::wstring::npos || editBuffer_.find(L'M') != std::wstring::npos) {
                    replayUnitMinutes_ = true;
                } else if (editBuffer_.find(L's') != std::wstring::npos || editBuffer_.find(L'S') != std::wstring::npos) {
                    replayUnitMinutes_ = false;
                }
            }
        } else if (cur == InputMode::EditBitrate) {
            uint32_t mbps = 0;
            if (ParseStrictPositiveUint(editBuffer_, mbps)) {
                editedCfg_.bitrateMbps = std::clamp(mbps, kMinBitrateMbps, kMaxBitrateMbps);
            }
        }
    }

    editBuffer_.clear();
    InvalidateWindow();
}

bool SettingsWindow::HandleInputChar(wchar_t ch) {
    if (inputMode_ == InputMode::None || inputMode_ == InputMode::CaptureHotkey) return false;
    if (ch == L'\r' || ch == L'\n' || ch == 0x1B) return true;

    if (ch == L'\b') {
        if (replaceOnFirstChar_) {
            editBuffer_.clear();
            replaceOnFirstChar_ = false;
        } else if (!editBuffer_.empty()) {
            editBuffer_.pop_back();
        }
        InvalidateWindow();
        return true;
    }

    bool allowed = false;
    size_t maxLen = 4;
    if (inputMode_ == InputMode::EditFps) {
        allowed = (ch >= L'0' && ch <= L'9');
        maxLen = 3;
    } else if (inputMode_ == InputMode::EditBitrate) {
        allowed = (ch >= L'0' && ch <= L'9');
        maxLen = 2;
    } else if (inputMode_ == InputMode::EditReplay) {
        allowed = (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L':' ||
                  ch == L'm' || ch == L'M' || ch == L's' || ch == L'S';
        maxLen = 8;
    }

    if (allowed) {
        if (replaceOnFirstChar_) {
            editBuffer_.clear();
            replaceOnFirstChar_ = false;
        }
        if (editBuffer_.size() < maxLen) {
            editBuffer_.push_back(ch);
            InvalidateWindow();
        }
    }
    return true;
}

bool SettingsWindow::HandleInputKeyDown(UINT vk, LPARAM lParam) {
    if (inputMode_ == InputMode::None) return false;

    if (inputMode_ == InputMode::CaptureHotkey) {
        if ((lParam & (1LL << 30)) != 0) return true;
        if (vk == VK_ESCAPE) {
            CommitOrCancelInput(false, true);
            return true;
        }
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            vk == VK_LWIN || vk == VK_RWIN ||
            vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_LMENU || vk == VK_RMENU) {
            return true;
        }
        uint32_t mods = 0;
        if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
        if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;

        HotkeySpec candidate;
        if (FormatHotkeyFromVk(mods, vk, candidate)) {
            editedCfg_.hotkeySave = candidate;
            CommitOrCancelInput(false, false);
            ShowFeedback(L"Hotkey set to " + Utf8ToWide(candidate.display) + L". Apply to save.", false);
        } else {
            ShowFeedback(L"Add Ctrl, Alt, or Shift for letters/numbers, or press F1-F24/Numpad.", true);
        }
        return true;
    }

    if (vk == VK_RETURN) {
        CommitOrCancelInput(true);
        return true;
    }
    if (vk == VK_ESCAPE) {
        CommitOrCancelInput(false);
        return true;
    }
    if (vk == VK_TAB) {
        InputMode cur = inputMode_;
        CommitOrCancelInput(true);
        if (cur == InputMode::EditFps) EnterInputMode(InputMode::EditReplay);
        else if (cur == InputMode::EditReplay) EnterInputMode(InputMode::EditBitrate);
        return true;
    }
    return false;
}

void SettingsWindow::UpdateBitrateFromDip(float dipX) {
    float w = sliderTrackRect_.right - sliderTrackRect_.left;
    if (w <= 1.0f) return;
    float t = ClampF((dipX - sliderTrackRect_.left) / w, 0.0f, 1.0f);
    uint32_t nextMbps = static_cast<uint32_t>(std::lround(5.0f + t * 75.0f));
    nextMbps = std::clamp(nextMbps, 5u, kMaxBitrateMbps);
    if (editedCfg_.bitrateMbps != nextMbps) {
        editedCfg_.bitrateMbps = nextMbps;
        InvalidateWindow();
    }
}

void SettingsWindow::ExecuteHit(const HitKey& key, float dipX) {
    switch (key.action) {
        case HitAction::WinMinimize:
            ShowWindow(hwnd_, SW_MINIMIZE);
            break;
        case HitAction::WinClose:
        case HitAction::RailHide:
            DestroyWindow(hwnd_);
            break;
        case HitAction::TitleSaveClip:
            if (callbacks_.onSaveReplay) callbacks_.onSaveReplay();
            break;
        case HitAction::SelectTab:
            SetActiveTab(static_cast<SettingsTab>(key.param));
            break;
        case HitAction::RailQuit:
            if (callbacks_.onQuit) callbacks_.onQuit();
            break;
        case HitAction::ClipsRefresh:
            thumbCache_.clear();
            ReloadClips();
            ShowFeedback(L"Clip library refreshed.", false);
            break;
        case HitAction::ClipsOpenFolder:
            OpenFolderInExplorer(appliedCfg_.outputDir.empty() ? ExpandEnvPath(appliedCfg_.outputDirRaw) : appliedCfg_.outputDir);
            break;
        case HitAction::ClipPlay: {
            size_t idx = static_cast<size_t>(key.param);
            if (idx < clips_.size()) {
                ShellExecuteW(hwnd_, L"open", clips_[idx].fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case HitAction::ClipShowInFolder: {
            size_t idx = static_cast<size_t>(key.param);
            if (idx < clips_.size()) {
                RevealFileInExplorer(clips_[idx].fullPath);
            }
            break;
        }
        case HitAction::ClipDelete: {
            size_t idx = static_cast<size_t>(key.param);
            if (idx < clips_.size()) {
                DeleteClipToRecycleBin(idx);
            }
            break;
        }
        case HitAction::EmptyGoSettings:
            SetActiveTab(SettingsTab::Settings);
            break;
        case HitAction::RefreshMonitors:
            ReloadMonitors();
            ShowFeedback(L"Display list refreshed.", false);
            break;
        case HitAction::SelectMonitor:
            editedCfg_.monitorIndex = static_cast<uint32_t>(key.param);
            InvalidateWindow();
            break;
        case HitAction::SelectFpsPreset:
            editedCfg_.fps = static_cast<uint32_t>(key.param);
            InvalidateWindow();
            break;
        case HitAction::EditFpsBox:
            EnterInputMode(InputMode::EditFps);
            break;
        case HitAction::SelectReplayPreset:
            editedCfg_.replaySeconds = static_cast<uint32_t>(key.param);
            replayUnitMinutes_ = (editedCfg_.replaySeconds >= 60u && (editedCfg_.replaySeconds % 60u) == 0u);
            InvalidateWindow();
            break;
        case HitAction::EditReplayBox:
            EnterInputMode(InputMode::EditReplay);
            break;
        case HitAction::ToggleReplayUnit: {
            bool wantMin = (key.param == 1);
            if (replayUnitMinutes_ != wantMin) {
                replayUnitMinutes_ = wantMin;
                InvalidateWindow();
            }
            break;
        }
        case HitAction::BitrateSlider:
            draggingSlider_ = true;
            SetCapture(hwnd_);
            UpdateBitrateFromDip(dipX);
            break;
        case HitAction::EditBitrateBox:
            EnterInputMode(InputMode::EditBitrate);
            break;
        case HitAction::ToggleDesktopAudio:
            editedCfg_.recordDesktopAudio = !editedCfg_.recordDesktopAudio;
            InvalidateWindow();
            break;
        case HitAction::ToggleMicAudio:
            editedCfg_.recordMicAudio = !editedCfg_.recordMicAudio;
            InvalidateWindow();
            break;
        case HitAction::ToggleChime:
            editedCfg_.saveChime = !editedCfg_.saveChime;
            InvalidateWindow();
            break;
        case HitAction::HotkeyCaptureBtn:
            if (inputMode_ == InputMode::CaptureHotkey) {
                CommitOrCancelInput(false, true);
            } else {
                EnterInputMode(InputMode::CaptureHotkey);
            }
            break;
        case HitAction::HotkeyResetBtn:
            editedCfg_.hotkeySave = MakeDefaultConfig().hotkeySave;
            ShowFeedback(L"Hotkey reset to " + Utf8ToWide(editedCfg_.hotkeySave.display) + L". Apply to save.", false);
            break;
        case HitAction::BrowseFolder:
            BrowseForOutputFolder();
            break;
        case HitAction::OpenFolderBtn:
            OpenFolderInExplorer(editedCfg_.outputDir.empty() ? ExpandEnvPath(editedCfg_.outputDirRaw) : editedCfg_.outputDir);
            break;
        case HitAction::DiscardChanges:
            editedCfg_ = appliedCfg_;
            replayUnitMinutes_ = (editedCfg_.replaySeconds >= 60u && (editedCfg_.replaySeconds % 60u) == 0u);
            ShowFeedback(L"Reverted unsaved changes.", false);
            break;
        case HitAction::ApplyChanges: {
            std::wstring fb;
            bool isErr = false;
            if (callbacks_.onApply) {
                callbacks_.onApply(editedCfg_, fb, isErr);
            }
            if (callbacks_.getConfig) {
                appliedCfg_ = callbacks_.getConfig();
                editedCfg_ = appliedCfg_;
                replayUnitMinutes_ = (editedCfg_.replaySeconds >= 60u && (editedCfg_.replaySeconds % 60u) == 0u);
            }
            ReloadClips();
            if (!fb.empty()) ShowFeedback(fb, isErr);
            RefreshStatus();
            break;
        }
        case HitAction::None:
            break;
    }
}

void SettingsWindow::BrowseForOutputFolder() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return;
    }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    }
    std::wstring current = editedCfg_.outputDir.empty() ? ExpandEnvPath(editedCfg_.outputDirRaw) : editedCfg_.outputDir;
    Microsoft::WRL::ComPtr<IShellItem> curItem;
    if (!current.empty() && SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(&curItem)))) {
        dlg->SetFolder(curItem.Get());
    }
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        Microsoft::WRL::ComPtr<IShellItem> resItem;
        if (SUCCEEDED(dlg->GetResult(&resItem))) {
            PWSTR pszPath = nullptr;
            if (SUCCEEDED(resItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                editedCfg_.outputDir = pszPath;
                editedCfg_.outputDirRaw = WideToUtf8(pszPath);
                CoTaskMemFree(pszPath);
                InvalidateWindow();
            }
        }
    }
}

void SettingsWindow::OpenFolderInExplorer(const std::wstring& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void SettingsWindow::RevealFileInExplorer(const std::wstring& fullPath) {
    if (fullPath.empty()) return;
    std::wstring args = L"/select,\"" + fullPath + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void SettingsWindow::DeleteClipToRecycleBin(size_t clipIndex) {
    if (clipIndex >= clips_.size()) return;
    std::wstring name = clips_[clipIndex].fileName;
    std::wstring fromPath = clips_[clipIndex].fullPath;
    fromPath.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd = hwnd_;
    op.wFunc = FO_DELETE;
    op.pFrom = fromPath.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
        thumbCache_.erase(clips_[clipIndex].fullPath);
        ReloadClips();
        hoveredHit_ = {};
        ShowFeedback(L"Moved " + name + L" to Recycle Bin.", false);
    } else {
        ShowFeedback(L"Could not remove " + name + L".", true);
    }
}

LRESULT SettingsWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == kWmSelectTab) {
        int t = static_cast<int>(wParam);
        SetActiveTab(t == 0 ? SettingsTab::Clips : SettingsTab::Settings);
        return 0;
    }

    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) return 0;
            break;

        case WM_NCACTIVATE:
            if (!IsIconic(hwnd_)) return 1;
            break;

        case WM_NCHITTEST: {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd_, &pt);
            float dipX = static_cast<float>(pt.x) * 96.0f / static_cast<float>(dpi_);
            float dipY = static_cast<float>(pt.y) * 96.0f / static_cast<float>(dpi_);
            if (dipY >= 0.0f && dipY < kTitleBarH && !HitTestDip(dipX, dipY)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_CREATE:
            ReloadConfigState();
            RefreshStatus();
            SetTimer(hwnd_, kStatusTimerId, 1000, nullptr);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd_, &ps);
            RenderToDC(hdc);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_PRINTCLIENT:
            RenderToDC(reinterpret_cast<HDC>(wParam));
            return 0;

        case WM_SIZE:
            InvalidateWindow();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = static_cast<int>(HIWORD(wParam));
            if (dpi_ <= 0) dpi_ = 96;
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (rt_) {
                rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
            }
            InvalidateWindow();
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
                trackingMouse_ = TrackMouseEvent(&tme) != FALSE;
            }
            float dipX = static_cast<float>(GET_X_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);
            float dipY = static_cast<float>(GET_Y_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);

            if (draggingSlider_) {
                UpdateBitrateFromDip(dipX);
                return 0;
            }

            const HitTarget* hit = HitTestDip(dipX, dipY);
            HitKey nextHovered = hit ? hit->key : HitKey{};
            if (!(nextHovered == hoveredHit_)) {
                hoveredHit_ = nextHovered;
                InvalidateWindow();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            if (!(hoveredHit_ == HitKey{})) {
                hoveredHit_ = {};
                InvalidateWindow();
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                bool textCur = (hoveredHit_.action == HitAction::EditFpsBox ||
                                hoveredHit_.action == HitAction::EditReplayBox ||
                                hoveredHit_.action == HitAction::EditBitrateBox);
                bool hand = draggingSlider_ || (!textCur && hoveredHit_.action != HitAction::None);
                SetCursor(LoadCursorW(nullptr, textCur ? IDC_IBEAM : (hand ? IDC_HAND : IDC_ARROW)));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            float dipX = static_cast<float>(GET_X_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);
            float dipY = static_cast<float>(GET_Y_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);
            const HitTarget* hit = HitTestDip(dipX, dipY);

            auto matchesCurrentInput = [&](const HitTarget* h) -> bool {
                if (!h) return false;
                if (inputMode_ == InputMode::EditFps && h->key.action == HitAction::EditFpsBox) return true;
                if (inputMode_ == InputMode::EditReplay && h->key.action == HitAction::EditReplayBox) return true;
                if (inputMode_ == InputMode::EditBitrate && h->key.action == HitAction::EditBitrateBox) return true;
                if (inputMode_ == InputMode::CaptureHotkey && h->key.action == HitAction::HotkeyCaptureBtn) return true;
                return false;
            };

            if (inputMode_ != InputMode::None && !matchesCurrentInput(hit)) {
                bool wasHotkey = (inputMode_ == InputMode::CaptureHotkey);
                CommitOrCancelInput(!wasHotkey, wasHotkey);
            }

            if (hit) {
                ExecuteHit(hit->key, dipX);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (draggingSlider_) {
                draggingSlider_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                InvalidateWindow();
            }
            return 0;

        case WM_MOUSEWHEEL: {
            if (maxScroll_ > 0.0f) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                float nextScroll = ClampF(scrollY_ - (static_cast<float>(delta) / 120.0f) * 48.0f, 0.0f, maxScroll_);
                if (std::fabs(nextScroll - scrollY_) > 0.1f) {
                    scrollY_ = nextScroll;
                    InvalidateWindow();
                }
            }
            return 0;
        }

        case WM_CHAR:
            if (HandleInputChar(static_cast<wchar_t>(wParam))) {
                return 0;
            }
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (HandleInputKeyDown(static_cast<UINT>(wParam), lParam)) {
                return 0;
            }
            break;

        case WM_KILLFOCUS:
            if (draggingSlider_) {
                draggingSlider_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            if (inputMode_ != InputMode::None) {
                bool wasHotkey = (inputMode_ == InputMode::CaptureHotkey);
                CommitOrCancelInput(!wasHotkey, wasHotkey);
            }
            return 0;

        case WM_TIMER:
            if (wParam == kStatusTimerId) {
                RefreshStatus();
                return 0;
            }
            if (wParam == kThumbTimerId) {
                StepThumbnailBatch(3, true);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            CommitOrCancelInput(false, false);
            if (draggingSlider_) {
                draggingSlider_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            KillTimer(hwnd_, kStatusTimerId);
            KillTimer(hwnd_, kThumbTimerId);
            DestroyAllResources();
            trackingMouse_ = false;
            hoveredHit_ = {};
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}
