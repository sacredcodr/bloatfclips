#include "settings_window.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace clipper {

namespace {

constexpr wchar_t kSettingsWndClass[] = L"ClipperSettingsWindow";
constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT_PTR kThumbTimerId = 2;

constexpr float kWndW = 960.0f;
constexpr float kWndH = 640.0f;
constexpr float kSidebarW = 220.0f;
constexpr float kHeaderH = 68.0f;
constexpr float kFooterH = 62.0f;
constexpr float kViewTop = kHeaderH;
constexpr float kViewBottom = kWndH - kFooterH;
constexpr float kViewH = kViewBottom - kViewTop;
constexpr float kContentPadL = 28.0f;
constexpr float kContentPadR = 36.0f;
constexpr float kContentW = kWndW - kSidebarW - kContentPadL - kContentPadR;

constexpr D2D1_COLOR_F C(uint32_t rgb, float a = 1.0f) {
    return D2D1_COLOR_F{
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        a
    };
}

constexpr D2D1_COLOR_F kColBg = C(0x0B0C10);
constexpr D2D1_COLOR_F kColSidebar = C(0x0F1117);
constexpr D2D1_COLOR_F kColHeaderBg = C(0x0C0D12);
constexpr D2D1_COLOR_F kColCard = C(0x14161F);
constexpr D2D1_COLOR_F kColCardHover = C(0x1A1D29);
constexpr D2D1_COLOR_F kColWell = C(0x0D0F16);
constexpr D2D1_COLOR_F kColWellHover = C(0x151824);
constexpr D2D1_COLOR_F kColBorder = C(0x232634);
constexpr D2D1_COLOR_F kColBorderHover = C(0x373C52);
constexpr D2D1_COLOR_F kColDivider = C(0x1D202C);

constexpr D2D1_COLOR_F kColAccent = C(0x5E6AD2);
constexpr D2D1_COLOR_F kColAccentHover = C(0x6E7AE2);
constexpr D2D1_COLOR_F kColAccentTint = C(0x5E6AD2, 0.14f);
constexpr D2D1_COLOR_F kColAccentBorder = C(0x5E6AD2, 0.65f);

constexpr D2D1_COLOR_F kColText = C(0xF3F4F8);
constexpr D2D1_COLOR_F kColTextSec = C(0x9BA1B2);
constexpr D2D1_COLOR_F kColTextMuted = C(0x666C7E);

constexpr D2D1_COLOR_F kColGreen = C(0x22C55E);
constexpr D2D1_COLOR_F kColGreenTint = C(0x22C55E, 0.14f);
constexpr D2D1_COLOR_F kColAmber = C(0xF59E0B);
constexpr D2D1_COLOR_F kColAmberTint = C(0xF59E0B, 0.15f);
constexpr D2D1_COLOR_F kColRed = C(0xEF4444);
constexpr D2D1_COLOR_F kColRedTint = C(0xEF4444, 0.15f);

constexpr D2D1_COLOR_F kColKeyBase = C(0x090A0F);
constexpr D2D1_COLOR_F kColKeyBody = C(0x222635);
constexpr D2D1_COLOR_F kColKeyBorder = C(0x3B4159);
constexpr D2D1_COLOR_F kColKeyHi = C(0x555D7E, 0.55f);

constexpr uint32_t kDurationPresets[] = { 15, 30, 60, 90, 120, 180, 300 };

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

std::wstring FormatIntegerComma(uint64_t val) {
    wchar_t buf[32] = {};
    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%llu", static_cast<unsigned long long>(val));
    std::wstring raw(buf);
    std::wstring out;
    int count = 0;
    for (size_t i = raw.size(); i > 0; --i) {
        if (count > 0 && (count % 3) == 0) out.push_back(L',');
        out.push_back(raw[i - 1]);
        count++;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}

SettingsWindow::SettingsWindow(SettingsCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

SettingsWindow::~SettingsWindow() {
    CloseIfOpen();
}

void SettingsWindow::ShowOrFocus(HINSTANCE hInst, HICON icon, SettingsTab initialTab) {
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
    RECT wr{
        0, 0,
        MulDiv(static_cast<int>(kWndW), dpi_, 96),
        MulDiv(static_cast<int>(kWndH), dpi_, 96)
    };
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, static_cast<UINT>(dpi_));
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;
    int posX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - winW) / 2;
    int posY = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - winH) / 2;

    hwnd_ = CreateWindowExW(
        0, kSettingsWndClass, L"Clipper", style,
        posX, posY, winW, winH,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return;

    BOOL dark = TRUE;
    DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
    COLORREF captionCol = RGB(13, 14, 19);
    COLORREF borderCol = RGB(35, 38, 52);
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &captionCol, sizeof(captionCol));
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &borderCol, sizeof(borderCol));

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::SetActiveTab(SettingsTab tab) {
    if (recordingHotkey_) EndHotkeyCapture(true);
    if (activeTab_ != tab) {
        activeTab_ = tab;
        scrollY_ = 0.0f;
        maxScroll_ = 0.0f;
        hoveredHit_ = {};
    }
    if (activeTab_ == SettingsTab::Clips) {
        ReloadClips();
    } else if (activeTab_ == SettingsTab::Capture) {
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
    if (!roundStroke_) {
        D2D1_STROKE_STYLE_PROPERTIES props{};
        props.startCap = D2D1_CAP_STYLE_ROUND;
        props.endCap = D2D1_CAP_STYLE_ROUND;
        props.dashCap = D2D1_CAP_STYLE_ROUND;
        props.lineJoin = D2D1_LINE_JOIN_ROUND;
        props.miterLimit = 4.0f;
        props.dashStyle = D2D1_DASH_STYLE_SOLID;
        d2dFactory_->CreateStrokeStyle(&props, nullptr, 0, &roundStroke_);
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
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false, fmtBrand_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true, fmtPageTitle_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true, fmtSection_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL, false, true, fmtBody_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true, fmtBodySemi_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, false, true, fmtSmall_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, true, false, fmtSmallWrap_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Segoe UI", 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true, fmtMicro_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Consolas", 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true, fmtMono_);
        CreateConfiguredTextFormat(dwriteFactory_.Get(), L"Consolas", 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false, fmtKeycap_);
    }
    return d2dFactory_ && dwriteFactory_ && fmtBody_;
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
    fmtSection_.Reset();
    fmtBody_.Reset();
    fmtBodySemi_.Reset();
    fmtSmall_.Reset();
    fmtSmallWrap_.Reset();
    fmtMicro_.Reset();
    fmtMono_.Reset();
    fmtKeycap_.Reset();
    roundStroke_.Reset();
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

void SettingsWindow::ComputeRamEstimate(double& outMb, float& outRatio, bool& outOverCap) const {
    AppConfig est = editedCfg_;
    est.bitrateMbps = std::max<uint32_t>(est.bitrateMbps, 2u);
    est.replaySeconds = std::max<uint32_t>(est.replaySeconds, 5u);
    double totalBytes = static_cast<double>(est.VideoArenaBytes() + est.AudioArenaBytes());
    outMb = totalBytes / (1024.0 * 1024.0);
    uint64_t budget = static_cast<uint64_t>(est.bitrateMbps) * est.replaySeconds;
    outRatio = ClampF(static_cast<float>(budget) / static_cast<float>(kMaxBitrateSecondsBudget), 0.0f, 1.0f);
    outOverCap = budget > kMaxBitrateSecondsBudget;
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

void SettingsWindow::DrawPillButton(D2D1_RECT_F rect, const wchar_t* label, const ui::IconId* icon,
                                    HitKey key, bool primary, bool subtleDanger) {
    bool hovered = (hoveredHit_ == key);
    D2D1_COLOR_F fill = kColWell;
    D2D1_COLOR_F border = kColBorder;
    D2D1_COLOR_F fg = kColText;

    if (primary) {
        fill = hovered ? kColAccentHover : kColAccent;
        border = fill;
        fg = C(0xFFFFFF);
    } else if (subtleDanger) {
        fill = hovered ? kColRedTint : kColWell;
        border = hovered ? C(0xEF4444, 0.55f) : kColBorder;
        fg = hovered ? kColRed : kColTextSec;
    } else {
        fill = hovered ? kColCardHover : kColWell;
        border = hovered ? kColBorderHover : kColBorder;
        fg = kColText;
    }

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 7.0f, 7.0f);
    brush_->SetColor(fill);
    rt_->FillRoundedRectangle(rr, brush_.Get());
    if (!primary || hovered) {
        brush_->SetColor(border);
        rt_->DrawRoundedRectangle(rr, brush_.Get(), 1.0f);
    }

    bool hasLabel = (label && label[0] != L'\0');
    if (icon && hasLabel) {
        std::wstring_view labelSv(label);
        float textW = 0.0f;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->CreateTextLayout(
                labelSv.data(), static_cast<UINT32>(labelSv.size()), fmtBodySemi_.Get(), 300.0f, 30.0f, &layout))) {
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            textW = tm.width;
        }
        float iconSz = 14.0f;
        float gap = 6.0f;
        float totalW = iconSz + gap + textW;
        float cx = (rect.left + rect.right) * 0.5f;
        float cy = (rect.top + rect.bottom) * 0.5f;
        float startX = cx - totalW * 0.5f;

        brush_->SetColor(fg);
        ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(), *icon,
            D2D1::RectF(startX, cy - iconSz * 0.5f, startX + iconSz, cy + iconSz * 0.5f), 1.6f);

        DrawTextLine(labelSv, fmtBodySemi_.Get(),
            D2D1::RectF(startX + iconSz + gap, rect.top, rect.right - 4.0f, rect.bottom),
            fg, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else if (icon) {
        float cy = (rect.top + rect.bottom) * 0.5f;
        float cx = (rect.left + rect.right) * 0.5f;
        float half = 7.5f;
        brush_->SetColor(fg);
        ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(), *icon,
            D2D1::RectF(cx - half, cy - half, cx + half, cy + half), 1.6f);
    } else if (hasLabel) {
        DrawTextLine(label, fmtBodySemi_.Get(), rect, fg,
            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    hitTargets_.push_back({ rect, key, false });
}

void SettingsWindow::DrawToggleSwitch(D2D1_RECT_F rect, bool on, bool hovered) {
    float h = rect.bottom - rect.top;
    float r = h * 0.5f;
    D2D1_COLOR_F track = on ? (hovered ? kColAccentHover : kColAccent) : (hovered ? C(0x303546) : C(0x252938));
    brush_->SetColor(track);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(rect, r, r), brush_.Get());

    if (!on) {
        brush_->SetColor(kColBorderHover);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(rect, r, r), brush_.Get(), 1.0f);
    }

    float knobR = r - 3.0f;
    float knobX = on ? (rect.right - r) : (rect.left + r);
    float knobY = (rect.top + rect.bottom) * 0.5f;
    brush_->SetColor(on ? C(0xFFFFFF) : C(0xC5CAD8));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, knobY), knobR, knobR), brush_.Get());
}

float SettingsWindow::DrawKeycapGroup(float startX, float centerY, const std::string& display,
                                      bool darkStyle, float maxRight) {
    std::vector<std::wstring> parts = SplitKeycaps(display);
    if (parts.empty()) return startX;

    float x = startX;
    float capH = darkStyle ? 28.0f : 22.0f;
    float padX = darkStyle ? 10.0f : 7.0f;
    float minW = darkStyle ? 30.0f : 22.0f;
    float gap = darkStyle ? 6.0f : 4.0f;
    IDWriteTextFormat* fmt = darkStyle ? fmtKeycap_.Get() : fmtMicro_.Get();

    for (size_t i = 0; i < parts.size(); ++i) {
        float textW = 14.0f;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->CreateTextLayout(
                parts[i].c_str(), static_cast<UINT32>(parts[i].size()), fmt, 200.0f, 30.0f, &layout))) {
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            textW = std::ceil(tm.width);
        }
        float w = std::max(minW, textW + padX * 2.0f);
        if (maxRight > 0.0f && x + w > maxRight) break;

        float top = centerY - capH * 0.5f;
        D2D1_RECT_F baseR = D2D1::RectF(x, top + 2.0f, x + w, top + capH + 2.0f);
        brush_->SetColor(kColKeyBase);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(baseR, 5.0f, 5.0f), brush_.Get());

        D2D1_RECT_F capR = D2D1::RectF(x, top, x + w, top + capH);
        brush_->SetColor(kColKeyBody);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(capR, 5.0f, 5.0f), brush_.Get());
        brush_->SetColor(kColKeyBorder);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(capR, 5.0f, 5.0f), brush_.Get(), 1.0f);

        brush_->SetColor(kColKeyHi);
        rt_->DrawLine(D2D1::Point2F(x + 4.0f, top + 1.0f), D2D1::Point2F(x + w - 4.0f, top + 1.0f), brush_.Get(), 1.0f);

        DrawTextLine(parts[i], fmt, capR, kColText,
            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        x += w + gap;
    }
    return x;
}

void SettingsWindow::DrawSidebar(D2D1_RECT_F area) {
    brush_->SetColor(kColSidebar);
    rt_->FillRectangle(area, brush_.Get());

    brush_->SetColor(kColDivider);
    rt_->DrawLine(D2D1::Point2F(area.right, 0.0f), D2D1::Point2F(area.right, kWndH), brush_.Get(), 1.0f);

    D2D1_RECT_F logoBox = D2D1::RectF(18.0f, 19.0f, 48.0f, 49.0f);
    brush_->SetColor(kColAccent);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(logoBox, 8.0f, 8.0f), brush_.Get());
    brush_->SetColor(C(0xFFFFFF));
    ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(), ui::IconId::BrandEmblem,
        D2D1::RectF(23.0f, 24.0f, 43.0f, 44.0f), 1.8f);

    DrawTextLine(L"Clipper", fmtBrand_.Get(),
        D2D1::RectF(58.0f, 18.0f, 140.0f, 40.0f), kColText);
    DrawTextLine(L"Zero-Overhead", fmtMicro_.Get(),
        D2D1::RectF(58.0f, 36.0f, 152.0f, 52.0f), kColTextMuted);

    D2D1_RECT_F nvPill = D2D1::RectF(156.0f, 24.0f, 204.0f, 42.0f);
    brush_->SetColor(kColAccentTint);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(nvPill, 4.0f, 4.0f), brush_.Get());
    brush_->SetColor(C(0x5E6AD2, 0.35f));
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(nvPill, 4.0f, 4.0f), brush_.Get(), 1.0f);
    DrawTextLine(L"NVENC", fmtMicro_.Get(), nvPill, C(0xA5B4FC),
        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    struct NavDef {
        SettingsTab tab;
        const wchar_t* label;
        ui::IconId icon;
    };
    const NavDef navs[] = {
        { SettingsTab::Clips, L"Clips", ui::IconId::FilmClips },
        { SettingsTab::Capture, L"Capture", ui::IconId::MonitorCapture },
        { SettingsTab::Audio, L"Audio", ui::IconId::WaveformAudio },
        { SettingsTab::Hotkeys, L"Hotkeys", ui::IconId::KeyboardKeys },
    };

    float navY = 82.0f;
    for (const auto& n : navs) {
        D2D1_RECT_F nr = D2D1::RectF(12.0f, navY, kSidebarW - 12.0f, navY + 38.0f);
        HitKey key{ HitAction::SelectTab, static_cast<int>(n.tab) };
        bool active = (activeTab_ == n.tab);
        bool hovered = (hoveredHit_ == key);

        if (active) {
            brush_->SetColor(kColCardHover);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(nr, 7.0f, 7.0f), brush_.Get());
            brush_->SetColor(kColBorder);
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(nr, 7.0f, 7.0f), brush_.Get(), 1.0f);

            D2D1_RECT_F accentBar = D2D1::RectF(12.0f, navY + 8.0f, 15.0f, navY + 30.0f);
            brush_->SetColor(kColAccent);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(accentBar, 1.5f, 1.5f), brush_.Get());
        } else if (hovered) {
            brush_->SetColor(kColCard);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(nr, 7.0f, 7.0f), brush_.Get());
        }

        D2D1_COLOR_F iconCol = active ? C(0xA5B4FC) : (hovered ? kColText : kColTextSec);
        D2D1_COLOR_F txtCol = active ? kColText : (hovered ? kColText : kColTextSec);
        brush_->SetColor(iconCol);
        ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(), n.icon,
            D2D1::RectF(26.0f, navY + 10.0f, 44.0f, navY + 28.0f), 1.65f);

        DrawTextLine(n.label, fmtBodySemi_.Get(),
            D2D1::RectF(54.0f, navY, kSidebarW - 50.0f, navY + 38.0f), txtCol);

        if (n.tab == SettingsTab::Clips && !clips_.empty()) {
            wchar_t cntBuf[16] = {};
            std::swprintf(cntBuf, sizeof(cntBuf) / sizeof(wchar_t), L"%zu", clips_.size());
            D2D1_RECT_F badge = D2D1::RectF(kSidebarW - 46.0f, navY + 10.0f, kSidebarW - 20.0f, navY + 28.0f);
            brush_->SetColor(active ? kColAccentTint : kColWell);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(badge, 9.0f, 9.0f), brush_.Get());
            DrawTextLine(cntBuf, fmtMicro_.Get(), badge,
                active ? C(0xC7D2FE) : kColTextMuted,
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        hitTargets_.push_back({ nr, key, false });
        navY += 44.0f;
    }

    D2D1_RECT_F statusCard = D2D1::RectF(12.0f, 396.0f, kSidebarW - 12.0f, 578.0f);
    brush_->SetColor(kColCard);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(statusCard, 10.0f, 10.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(statusCard, 10.0f, 10.0f), brush_.Get(), 1.0f);

    D2D1_COLOR_F dotCol = kColTextMuted;
    D2D1_COLOR_F pillBg = kColWell;
    const wchar_t* stateLabel = L"STOPPED";
    if (status_.state == EngineState::Running) {
        dotCol = kColGreen;
        pillBg = kColGreenTint;
        stateLabel = L"LIVE BUFFER";
    } else if (status_.state == EngineState::Recovering || status_.state == EngineState::Starting) {
        dotCol = kColAmber;
        pillBg = kColAmberTint;
        stateLabel = (status_.state == EngineState::Starting) ? L"STARTING" : L"RECOVERING";
    } else if (status_.state == EngineState::Faulted) {
        dotCol = kColRed;
        pillBg = kColRedTint;
        stateLabel = L"FAULTED";
    }

    D2D1_RECT_F pillR = D2D1::RectF(24.0f, 408.0f, 118.0f, 428.0f);
    brush_->SetColor(pillBg);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(pillR, 10.0f, 10.0f), brush_.Get());
    brush_->SetColor(dotCol);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(34.0f, 418.0f), 3.5f, 3.5f), brush_.Get());
    DrawTextLine(stateLabel, fmtMicro_.Get(),
        D2D1::RectF(42.0f, 408.0f, 114.0f, 428.0f), dotCol);

    wchar_t specBuf[64] = {};
    if (status_.video.width > 0 && status_.video.height > 0) {
        std::swprintf(specBuf, sizeof(specBuf) / sizeof(wchar_t), L"%up · %u",
            status_.video.height, appliedCfg_.fps);
    } else {
        std::swprintf(specBuf, sizeof(specBuf) / sizeof(wchar_t), L"%u fps", appliedCfg_.fps);
    }
    DrawTextLine(specBuf, fmtMicro_.Get(),
        D2D1::RectF(122.0f, 408.0f, 196.0f, 428.0f), kColTextSec, DWRITE_TEXT_ALIGNMENT_TRAILING);

    double activeMb = static_cast<double>(appliedCfg_.VideoArenaBytes() + appliedCfg_.AudioArenaBytes()) / (1024.0 * 1024.0);
    if (status_.state == EngineState::Faulted && !status_.lastError.empty()) {
        std::wstring errW = Utf8ToWide(status_.lastError);
        DrawTextLine(errW, fmtSmallWrap_.Get(),
            D2D1::RectF(24.0f, 436.0f, 196.0f, 480.0f), kColRed,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    } else {
        wchar_t ringInfo[96] = {};
        std::swprintf(ringInfo, sizeof(ringInfo) / sizeof(wchar_t), L"%us replay · %u Mbps",
            appliedCfg_.replaySeconds, appliedCfg_.bitrateMbps);
        DrawTextLine(ringInfo, fmtBodySemi_.Get(),
            D2D1::RectF(24.0f, 438.0f, 196.0f, 456.0f), kColText);

        wchar_t memInfo[64] = {};
        std::swprintf(memInfo, sizeof(memInfo) / sizeof(wchar_t), L"%.0f MB RAM · 0 B disk I/O", activeMb);
        DrawTextLine(memInfo, fmtSmall_.Get(),
            D2D1::RectF(24.0f, 457.0f, 196.0f, 475.0f), kColTextMuted);
    }

    DrawTextLine(L"Hotkey", fmtSmall_.Get(),
        D2D1::RectF(24.0f, 488.0f, 68.0f, 512.0f), kColTextSec);
    DrawKeycapGroup(72.0f, 500.0f, appliedCfg_.hotkeySave.display, false, 198.0f);

    ui::IconId spark = ui::IconId::SparkSave;
    DrawPillButton(D2D1::RectF(24.0f, 528.0f, 196.0f, 564.0f),
        L"Save Clip", &spark, HitKey{ HitAction::SidebarSaveClip, 0 }, true);

    ui::IconId trayIco = ui::IconId::MinimizeTray;
    ui::IconId quitIco = ui::IconId::PowerQuit;
    DrawPillButton(D2D1::RectF(12.0f, 592.0f, 114.0f, 626.0f),
        L"Tray", &trayIco, HitKey{ HitAction::SidebarHide, 0 }, false);
    DrawPillButton(D2D1::RectF(122.0f, 592.0f, kSidebarW - 12.0f, 626.0f),
        L"Quit", &quitIco, HitKey{ HitAction::SidebarQuit, 0 }, false, true);
}

void SettingsWindow::DrawTopHeader() {
    D2D1_RECT_F hdr = D2D1::RectF(kSidebarW, 0.0f, kWndW, kHeaderH);
    brush_->SetColor(kColHeaderBg);
    rt_->FillRectangle(hdr, brush_.Get());

    brush_->SetColor(kColDivider);
    rt_->DrawLine(D2D1::Point2F(kSidebarW, kHeaderH), D2D1::Point2F(kWndW, kHeaderH), brush_.Get(), 1.0f);

    std::wstring title;
    std::wstring subtitle;
    if (activeTab_ == SettingsTab::Clips) {
        title = L"Recent Clips";
        if (clips_.empty()) {
            subtitle = L"Instant replay library";
        } else {
            wchar_t sub[96] = {};
            std::wstring szText = FormatBytesHuman(totalClipsBytes_);
            std::swprintf(sub, sizeof(sub) / sizeof(wchar_t), L"%zu %ls · %ls total",
                clips_.size(), clips_.size() == 1 ? L"clip" : L"clips", szText.c_str());
            subtitle = sub;
        }
        ui::IconId refIco = ui::IconId::RefreshArrows;
        ui::IconId fldIco = ui::IconId::FolderOpen;
        DrawPillButton(D2D1::RectF(774.0f, 18.0f, 808.0f, 50.0f),
            L"", &refIco, HitKey{ HitAction::ClipsRefresh, 0 }, false);
        DrawPillButton(D2D1::RectF(816.0f, 18.0f, 932.0f, 50.0f),
            L"Open Folder", &fldIco, HitKey{ HitAction::ClipsOpenFolder, 0 }, false);
    } else if (activeTab_ == SettingsTab::Capture) {
        title = L"Capture & Quality";
        subtitle = L"DXGI Desktop Duplication · NVENC H.264 low-latency P4";
        ui::IconId refIco = ui::IconId::RefreshArrows;
        DrawPillButton(D2D1::RectF(792.0f, 18.0f, 932.0f, 50.0f),
            L"Refresh Displays", &refIco, HitKey{ HitAction::RefreshMonitors, 0 }, false);
    } else if (activeTab_ == SettingsTab::Audio) {
        title = L"Audio Channels";
        subtitle = L"WASAPI 48 kHz stereo mixed into 192 kbps AAC-LC";
    } else {
        title = L"Hotkeys & Storage";
        subtitle = L"Game-safe global trigger and fast-start MP4 destination";
    }

    DrawTextLine(title, fmtPageTitle_.Get(),
        D2D1::RectF(kSidebarW + kContentPadL, 12.0f, 760.0f, 38.0f), kColText);
    DrawTextLine(subtitle, fmtSmall_.Get(),
        D2D1::RectF(kSidebarW + kContentPadL, 38.0f, 760.0f, 56.0f), kColTextMuted);
}

void SettingsWindow::DrawClipsTab(float contentX, float contentW, float& yCursor) {
    if (clips_.empty()) {
        D2D1_RECT_F card = D2D1::RectF(contentX + 78.0f, yCursor + 78.0f, contentX + contentW - 78.0f, yCursor + 362.0f);
        brush_->SetColor(kColCard);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(card, 14.0f, 14.0f), brush_.Get());
        brush_->SetColor(kColBorder);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(card, 14.0f, 14.0f), brush_.Get(), 1.0f);

        float cx = (card.left + card.right) * 0.5f;
        float icoCy = card.top + 66.0f;
        brush_->SetColor(kColAccentTint);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, icoCy), 28.0f, 28.0f), brush_.Get());
        brush_->SetColor(C(0x818CF8));
        ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(), ui::IconId::FilmClips,
            D2D1::RectF(cx - 14.0f, icoCy - 14.0f, cx + 14.0f, icoCy + 14.0f), 1.7f);

        DrawTextLine(L"No clips saved yet", fmtPageTitle_.Get(),
            D2D1::RectF(card.left + 24.0f, card.top + 108.0f, card.right - 24.0f, card.top + 136.0f),
            kColText, DWRITE_TEXT_ALIGNMENT_CENTER);

        wchar_t msg[192] = {};
        std::swprintf(msg, sizeof(msg) / sizeof(wchar_t),
            L"Press %ls during gameplay to save the last %us buffer.",
            Utf8ToWide(appliedCfg_.hotkeySave.display).c_str(), appliedCfg_.replaySeconds);
        DrawTextLine(msg, fmtBody_.Get(),
            D2D1::RectF(card.left + 32.0f, card.top + 144.0f, card.right - 32.0f, card.top + 170.0f),
            kColTextSec, DWRITE_TEXT_ALIGNMENT_CENTER);

        ui::IconId spark = ui::IconId::SparkSave;
        ui::IconId sliders = ui::IconId::SlidersTune;
        DrawPillButton(D2D1::RectF(cx - 148.0f, card.top + 198.0f, cx - 8.0f, card.top + 236.0f),
            L"Save Clip Now", &spark, HitKey{ HitAction::SidebarSaveClip, 0 }, true);
        hitTargets_.back().inViewport = true;
        DrawPillButton(D2D1::RectF(cx + 8.0f, card.top + 198.0f, cx + 158.0f, card.top + 236.0f),
            L"Capture Setup", &sliders, HitKey{ HitAction::EmptyGoCapture, 0 }, false);
        hitTargets_.back().inViewport = true;

        yCursor += 380.0f;
        return;
    }

    float colGap = 16.0f;
    float cardW = (contentW - colGap) * 0.5f;
    float thumbH = 170.0f;
    float footH = 58.0f;
    float cardH = thumbH + footH;
    float rowGap = 14.0f;

    float baseTop = yCursor + 16.0f;
    for (size_t i = 0; i < clips_.size(); ++i) {
        size_t col = i % 2;
        size_t row = i / 2;
        float x = contentX + static_cast<float>(col) * (cardW + colGap);
        float y = baseTop + static_cast<float>(row) * (cardH + rowGap) - scrollY_;

        if (y + cardH < kViewTop - 20.0f || y > kViewBottom + 20.0f) {
            continue;
        }

        const ClipEntry& clip = clips_[i];
        D2D1_RECT_F cardR = D2D1::RectF(x, y, x + cardW, y + cardH);
        D2D1_RECT_F thumbR = D2D1::RectF(x, y, x + cardW, y + thumbH);

        HitKey playKey{ HitAction::ClipPlay, static_cast<int>(i) };
        HitKey folderKey{ HitAction::ClipShowInFolder, static_cast<int>(i) };
        HitKey deleteKey{ HitAction::ClipDelete, static_cast<int>(i) };
        bool cardHovered = (hoveredHit_ == playKey || hoveredHit_ == folderKey || hoveredHit_ == deleteKey);
        bool thumbHovered = (hoveredHit_ == playKey);

        D2D1_ROUNDED_RECT cardRR = D2D1::RoundedRect(cardR, 10.0f, 10.0f);
        brush_->SetColor(cardHovered ? kColCardHover : kColCard);
        rt_->FillRoundedRectangle(cardRR, brush_.Get());

        auto thumbIt = thumbCache_.find(clip.fullPath);
        ID2D1Bitmap* bmp = (thumbIt != thumbCache_.end()) ? thumbIt->second.bmp.Get() : nullptr;

        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        Microsoft::WRL::ComPtr<ID2D1Layer> clipLayer;
        if (d2dFactory_ &&
            SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(cardRR, &clipGeo)) &&
            SUCCEEDED(rt_->CreateLayer(&clipLayer))) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(thumbR, clipGeo.Get());
            rt_->PushLayer(&lp, clipLayer.Get());

            brush_->SetColor(kColWell);
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
                float tcx = (thumbR.left + thumbR.right) * 0.5f;
                float tcy = (thumbR.top + thumbR.bottom) * 0.5f;
                brush_->SetColor(C(0x282C3C));
                ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(),
                    ui::IconId::FilmClips, D2D1::RectF(tcx - 16.0f, tcy - 16.0f, tcx + 16.0f, tcy + 16.0f), 1.6f);
            }

            if (thumbHovered) {
                brush_->SetColor(C(0x090A0E, 0.45f));
                rt_->FillRectangle(thumbR, brush_.Get());
                float tcx = (thumbR.left + thumbR.right) * 0.5f;
                float tcy = (thumbR.top + thumbR.bottom) * 0.5f;
                brush_->SetColor(C(0x5E6AD2, 0.92f));
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tcx, tcy), 22.0f, 22.0f), brush_.Get());
                brush_->SetColor(C(0xFFFFFF));
                ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(),
                    ui::IconId::PlayTriangle, D2D1::RectF(tcx - 9.0f, tcy - 10.0f, tcx + 11.0f, tcy + 10.0f), 1.5f);
            }

            rt_->PopLayer();
        }

        std::wstring badgeTxt = FormatDurationShort(clip.durationSec);
        if (badgeTxt.empty()) badgeTxt = FormatBytesHuman(clip.sizeBytes);
        float badgeW = std::max(44.0f, static_cast<float>(badgeTxt.size()) * 7.8f + 14.0f);
        D2D1_RECT_F badgeR = D2D1::RectF(
            thumbR.right - badgeW - 10.0f, thumbR.bottom - 28.0f,
            thumbR.right - 10.0f, thumbR.bottom - 8.0f);
        brush_->SetColor(C(0x0B0C10, 0.86f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(badgeR, 5.0f, 5.0f), brush_.Get());
        brush_->SetColor(C(0xFFFFFF, 0.14f));
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(badgeR, 5.0f, 5.0f), brush_.Get(), 1.0f);
        DrawTextLine(badgeTxt, fmtMono_.Get(), badgeR, kColText,
            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        brush_->SetColor(kColDivider);
        rt_->DrawLine(D2D1::Point2F(cardR.left, thumbR.bottom), D2D1::Point2F(cardR.right, thumbR.bottom), brush_.Get(), 1.0f);

        brush_->SetColor(cardHovered ? kColBorderHover : kColBorder);
        rt_->DrawRoundedRectangle(cardRR, brush_.Get(), 1.0f);

        float textRight = cardR.right - 74.0f;
        DrawTextLine(clip.displayTitle, fmtBodySemi_.Get(),
            D2D1::RectF(cardR.left + 14.0f, thumbR.bottom + 9.0f, textRight, thumbR.bottom + 29.0f),
            kColText);

        std::wstring meta = clip.dateLabel;
        if (!meta.empty()) meta += L" · ";
        meta += FormatBytesHuman(clip.sizeBytes);
        DrawTextLine(meta, fmtSmall_.Get(),
            D2D1::RectF(cardR.left + 14.0f, thumbR.bottom + 30.0f, textRight, thumbR.bottom + 48.0f),
            kColTextMuted);

        hitTargets_.push_back({ thumbR, playKey, true });

        ui::IconId fld = ui::IconId::FolderOpen;
        ui::IconId del = ui::IconId::TrashBin;
        DrawPillButton(D2D1::RectF(cardR.right - 68.0f, thumbR.bottom + 15.0f, cardR.right - 40.0f, thumbR.bottom + 43.0f),
            L"", &fld, folderKey, false);
        hitTargets_.back().inViewport = true;
        DrawPillButton(D2D1::RectF(cardR.right - 36.0f, thumbR.bottom + 15.0f, cardR.right - 8.0f, thumbR.bottom + 43.0f),
            L"", &del, deleteKey, false, true);
        hitTargets_.back().inViewport = true;
    }

    size_t rows = (clips_.size() + 1) / 2;
    yCursor += 16.0f + static_cast<float>(rows) * cardH + static_cast<float>(rows > 0 ? rows - 1 : 0) * rowGap + 18.0f;
}

void SettingsWindow::DrawCaptureTab(float contentX, float contentW, float& yCursor) {
    float y = yCursor + 14.0f - scrollY_;

    DrawTextLine(L"Display Source", fmtSection_.Get(),
        D2D1::RectF(contentX, y, contentX + contentW, y + 20.0f), kColTextSec);
    y += 26.0f;

    float colGap = 16.0f;
    float monCardW = (contentW - colGap) * 0.5f;
    float monCardH = 84.0f;

    size_t monCount = monitors_.empty() ? 1 : monitors_.size();
    for (size_t i = 0; i < monCount; ++i) {
        size_t col = i % 2;
        size_t row = i / 2;
        float x = contentX + static_cast<float>(col) * (monCardW + colGap);
        float cy = y + static_cast<float>(row) * (monCardH + 12.0f);

        uint32_t monIdx = monitors_.empty() ? 0 : monitors_[i].index;
        bool selected = (editedCfg_.monitorIndex == monIdx);
        HitKey key{ HitAction::SelectMonitor, static_cast<int>(monIdx) };
        bool hovered = (hoveredHit_ == key);

        D2D1_RECT_F card = D2D1::RectF(x, cy, x + monCardW, cy + monCardH);
        brush_->SetColor(selected ? kColAccentTint : (hovered ? kColCardHover : kColCard));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(card, 10.0f, 10.0f), brush_.Get());
        brush_->SetColor(selected ? kColAccentBorder : (hovered ? kColBorderHover : kColBorder));
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(card, 10.0f, 10.0f), brush_.Get(), selected ? 1.5f : 1.0f);

        D2D1_RECT_F bezel = D2D1::RectF(x + 16.0f, cy + 19.0f, x + 64.0f, cy + 51.0f);
        brush_->SetColor(kColWell);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(bezel, 4.0f, 4.0f), brush_.Get());
        brush_->SetColor(selected ? kColAccent : kColBorderHover);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(bezel, 4.0f, 4.0f), brush_.Get(), 1.5f);
        rt_->DrawLine(D2D1::Point2F(x + 40.0f, cy + 51.0f), D2D1::Point2F(x + 40.0f, cy + 60.0f), brush_.Get(), 2.0f);
        rt_->DrawLine(D2D1::Point2F(x + 28.0f, cy + 60.0f), D2D1::Point2F(x + 52.0f, cy + 60.0f), brush_.Get(), 2.0f);

        wchar_t numBuf[16] = {};
        std::swprintf(numBuf, sizeof(numBuf) / sizeof(wchar_t), L"%u", monIdx + 1);
        DrawTextLine(numBuf, fmtSection_.Get(), bezel,
            selected ? C(0xC7D2FE) : kColTextSec, DWRITE_TEXT_ALIGNMENT_CENTER);

        std::wstring name = monitors_.empty() ? L"Primary Display" : monitors_[i].friendlyName;
        if (name.empty()) name = L"Display " + std::wstring(numBuf);
        DrawTextLine(name, fmtBodySemi_.Get(),
            D2D1::RectF(x + 78.0f, cy + 16.0f, card.right - 38.0f, cy + 36.0f), kColText);

        wchar_t meta[96] = {};
        if (!monitors_.empty() && monitors_[i].width > 0 && monitors_[i].height > 0) {
            if (monitors_[i].refreshHz > 0) {
                std::swprintf(meta, sizeof(meta) / sizeof(wchar_t), L"%u × %u · %u Hz",
                    monitors_[i].width, monitors_[i].height, monitors_[i].refreshHz);
            } else {
                std::swprintf(meta, sizeof(meta) / sizeof(wchar_t), L"%u × %u",
                    monitors_[i].width, monitors_[i].height);
            }
        } else {
            std::swprintf(meta, sizeof(meta) / sizeof(wchar_t), L"DXGI Output %u", monIdx + 1);
        }
        DrawTextLine(meta, fmtSmall_.Get(),
            D2D1::RectF(x + 78.0f, cy + 38.0f, card.right - 16.0f, cy + 56.0f), kColTextSec);

        if (!monitors_.empty() && monitors_[i].isPrimary) {
            D2D1_RECT_F primPill = D2D1::RectF(x + 78.0f, cy + 59.0f, x + 132.0f, cy + 75.0f);
            brush_->SetColor(selected ? C(0x5E6AD2, 0.25f) : kColWell);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(primPill, 4.0f, 4.0f), brush_.Get());
            DrawTextLine(L"Primary", fmtMicro_.Get(), primPill,
                selected ? C(0xC7D2FE) : kColTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (selected) {
            brush_->SetColor(C(0x818CF8));
            ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(),
                ui::IconId::CheckCircle,
                D2D1::RectF(card.right - 30.0f, cy + 14.0f, card.right - 12.0f, cy + 32.0f), 1.8f);
        }

        hitTargets_.push_back({ card, key, true });
    }

    size_t monRows = (monCount + 1) / 2;
    y += static_cast<float>(monRows) * monCardH + static_cast<float>(monRows > 0 ? monRows - 1 : 0) * 12.0f + 16.0f;

    DrawTextLine(L"Quality Preset", fmtSection_.Get(),
        D2D1::RectF(contentX, y, contentX + contentW, y + 20.0f), kColTextSec);
    y += 26.0f;

    struct PresetDef {
        HitAction action;
        const wchar_t* title;
        const wchar_t* sub;
        const wchar_t* tag;
        uint32_t fps;
        uint32_t bitrate;
    };
    const PresetDef presets[] = {
        { HitAction::PresetCompetitive, L"Competitive", L"60 fps · 20 Mbps", L"Recommended", 60, 20 },
        { HitAction::PresetHigh, L"High Quality", L"60 fps · 40 Mbps", L"Sharp Detail", 60, 40 },
        { HitAction::PresetCompact, L"Compact", L"30 fps · 10 Mbps", L"Low RAM", 30, 10 },
    };

    float preGap = 14.0f;
    float preW = (contentW - preGap * 2.0f) / 3.0f;
    float preH = 66.0f;
    for (int i = 0; i < 3; ++i) {
        const auto& p = presets[i];
        float px = contentX + static_cast<float>(i) * (preW + preGap);
        D2D1_RECT_F pr = D2D1::RectF(px, y, px + preW, y + preH);
        HitKey key{ p.action, 0 };
        bool active = (editedCfg_.fps == p.fps && editedCfg_.bitrateMbps == p.bitrate);
        bool hovered = (hoveredHit_ == key);

        brush_->SetColor(active ? kColAccentTint : (hovered ? kColCardHover : kColCard));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(pr, 9.0f, 9.0f), brush_.Get());
        brush_->SetColor(active ? kColAccentBorder : (hovered ? kColBorderHover : kColBorder));
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(pr, 9.0f, 9.0f), brush_.Get(), active ? 1.5f : 1.0f);

        DrawTextLine(p.title, fmtBodySemi_.Get(),
            D2D1::RectF(px + 14.0f, y + 12.0f, px + preW - 88.0f, y + 32.0f), kColText);
        DrawTextLine(p.sub, fmtSmall_.Get(),
            D2D1::RectF(px + 14.0f, y + 36.0f, px + preW - 14.0f, y + 54.0f),
            active ? C(0xC7D2FE) : kColTextMuted);

        D2D1_RECT_F tagR = D2D1::RectF(px + preW - 86.0f, y + 12.0f, px + preW - 10.0f, y + 30.0f);
        brush_->SetColor(active ? C(0x5E6AD2, 0.25f) : kColWell);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(tagR, 4.0f, 4.0f), brush_.Get());
        DrawTextLine(p.tag, fmtMicro_.Get(), tagR,
            active ? C(0xC7D2FE) : kColTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);

        hitTargets_.push_back({ pr, key, true });
    }
    y += preH + 16.0f;

    DrawTextLine(L"Buffer & Encoding", fmtSection_.Get(),
        D2D1::RectF(contentX, y, contentX + contentW, y + 20.0f), kColTextSec);
    y += 26.0f;

    float r1H = 48.0f, r2H = 50.0f, r3H = 60.0f, r4H = 50.0f;
    float cardTotH = r1H + r2H + r3H + r4H;
    D2D1_RECT_F fineCard = D2D1::RectF(contentX, y, contentX + contentW, y + cardTotH);
    brush_->SetColor(kColCard);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(fineCard, 10.0f, 10.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(fineCard, 10.0f, 10.0f), brush_.Get(), 1.0f);

    float ry = y;

    DrawTextLine(L"Frame rate", fmtBodySemi_.Get(),
        D2D1::RectF(contentX + 16.0f, ry + 7.0f, contentX + 260.0f, ry + 25.0f), kColText);
    DrawTextLine(L"Capture and NVENC cadence", fmtSmall_.Get(),
        D2D1::RectF(contentX + 16.0f, ry + 25.0f, contentX + 280.0f, ry + 42.0f), kColTextMuted);

    D2D1_RECT_F segTrack = D2D1::RectF(contentX + contentW - 180.0f, ry + 9.0f, contentX + contentW - 16.0f, ry + 39.0f);
    brush_->SetColor(kColWell);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(segTrack, 7.0f, 7.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(segTrack, 7.0f, 7.0f), brush_.Get(), 1.0f);

    const uint32_t fpsOpts[2] = { 30, 60 };
    const wchar_t* fpsLabels[2] = { L"30 fps", L"60 fps" };
    float segW = ((segTrack.right - segTrack.left) - 6.0f) * 0.5f;
    for (int i = 0; i < 2; ++i) {
        D2D1_RECT_F sr = D2D1::RectF(
            segTrack.left + 3.0f + static_cast<float>(i) * segW,
            segTrack.top + 3.0f,
            segTrack.left + 3.0f + static_cast<float>(i + 1) * segW,
            segTrack.bottom - 3.0f);
        HitKey key{ HitAction::SelectFps, static_cast<int>(fpsOpts[i]) };
        bool sel = (editedCfg_.fps == fpsOpts[i]);
        bool hov = (hoveredHit_ == key);
        if (sel) {
            brush_->SetColor(kColAccent);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(sr, 5.0f, 5.0f), brush_.Get());
        } else if (hov) {
            brush_->SetColor(kColWellHover);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(sr, 5.0f, 5.0f), brush_.Get());
        }
        DrawTextLine(fpsLabels[i], fmtSmall_.Get(), sr,
            sel ? C(0xFFFFFF) : kColTextSec, DWRITE_TEXT_ALIGNMENT_CENTER);
        hitTargets_.push_back({ sr, key, true });
    }

    ry += r1H;
    brush_->SetColor(kColDivider);
    rt_->DrawLine(D2D1::Point2F(contentX, ry), D2D1::Point2F(contentX + contentW, ry), brush_.Get(), 1.0f);

    DrawTextLine(L"Replay length", fmtBodySemi_.Get(),
        D2D1::RectF(contentX + 16.0f, ry + 8.0f, contentX + 240.0f, ry + 26.0f), kColText);
    wchar_t curSecBuf[48] = {};
    std::swprintf(curSecBuf, sizeof(curSecBuf) / sizeof(wchar_t), L"Active window: %us", editedCfg_.replaySeconds);
    DrawTextLine(curSecBuf, fmtSmall_.Get(),
        D2D1::RectF(contentX + 16.0f, ry + 26.0f, contentX + 240.0f, ry + 43.0f), kColTextMuted);

    float pillW = 46.0f;
    float pillGap = 6.0f;
    size_t pillCount = std::size(kDurationPresets);
    float pillsStartX = contentX + contentW - 16.0f - (static_cast<float>(pillCount) * pillW + static_cast<float>(pillCount - 1) * pillGap);
    for (size_t i = 0; i < pillCount; ++i) {
        uint32_t sec = kDurationPresets[i];
        float px = pillsStartX + static_cast<float>(i) * (pillW + pillGap);
        D2D1_RECT_F pr = D2D1::RectF(px, ry + 11.0f, px + pillW, ry + 39.0f);
        HitKey key{ HitAction::SelectSeconds, static_cast<int>(sec) };
        bool sel = (editedCfg_.replaySeconds == sec);
        bool hov = (hoveredHit_ == key);
        brush_->SetColor(sel ? kColAccentTint : (hov ? kColWellHover : kColWell));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(pr, 6.0f, 6.0f), brush_.Get());
        brush_->SetColor(sel ? kColAccentBorder : (hov ? kColBorderHover : kColBorder));
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(pr, 6.0f, 6.0f), brush_.Get(), 1.0f);

        wchar_t lbl[16] = {};
        std::swprintf(lbl, sizeof(lbl) / sizeof(wchar_t), L"%us", sec);
        DrawTextLine(lbl, fmtSmall_.Get(), pr,
            sel ? C(0xC7D2FE) : kColTextSec, DWRITE_TEXT_ALIGNMENT_CENTER);
        hitTargets_.push_back({ pr, key, true });
    }

    ry += r2H;
    brush_->SetColor(kColDivider);
    rt_->DrawLine(D2D1::Point2F(contentX, ry), D2D1::Point2F(contentX + contentW, ry), brush_.Get(), 1.0f);

    DrawTextLine(L"Video bitrate", fmtBodySemi_.Get(),
        D2D1::RectF(contentX + 16.0f, ry + 10.0f, contentX + 240.0f, ry + 28.0f), kColText);
    DrawTextLine(L"NVENC low-latency VBR target", fmtSmall_.Get(),
        D2D1::RectF(contentX + 16.0f, ry + 29.0f, contentX + 240.0f, ry + 47.0f), kColTextMuted);

    wchar_t mbpsTxt[32] = {};
    std::swprintf(mbpsTxt, sizeof(mbpsTxt) / sizeof(wchar_t), L"%u Mbps", editedCfg_.bitrateMbps);
    D2D1_RECT_F mbpsPill = D2D1::RectF(contentX + 266.0f, ry + 17.0f, contentX + 340.0f, ry + 41.0f);
    brush_->SetColor(kColWell);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(mbpsPill, 6.0f, 6.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(mbpsPill, 6.0f, 6.0f), brush_.Get(), 1.0f);
    DrawTextLine(mbpsTxt, fmtMono_.Get(), mbpsPill, C(0xA5B4FC),
        DWRITE_TEXT_ALIGNMENT_CENTER);

    float trLeft = contentX + 358.0f;
    float trRight = contentX + contentW - 18.0f;
    float trCy = ry + 23.0f;
    sliderTrackRect_ = D2D1::RectF(trLeft, trCy - 14.0f, trRight, trCy + 14.0f);

    D2D1_RECT_F rail = D2D1::RectF(trLeft, trCy - 3.0f, trRight, trCy + 3.0f);
    brush_->SetColor(kColWell);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(rail, 3.0f, 3.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(rail, 3.0f, 3.0f), brush_.Get(), 1.0f);

    float t = ClampF((static_cast<float>(editedCfg_.bitrateMbps) - 5.0f) / 75.0f, 0.0f, 1.0f);
    float knobX = trLeft + t * (trRight - trLeft);
    if (knobX > trLeft + 3.0f) {
        brush_->SetColor(kColAccent);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trLeft, trCy - 3.0f, knobX, trCy + 3.0f), 3.0f, 3.0f),
            brush_.Get());
    }

    bool sliderActive = draggingSlider_ || (hoveredHit_.action == HitAction::BitrateSlider);
    if (sliderActive) {
        brush_->SetColor(C(0x5E6AD2, 0.28f));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, trCy), 12.0f, 12.0f), brush_.Get());
    }
    brush_->SetColor(C(0xFFFFFF));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, trCy), 7.5f, 7.5f), brush_.Get());
    brush_->SetColor(kColAccent);
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, trCy), 7.5f, 7.5f), brush_.Get(), 2.0f);

    DrawTextLine(L"5 Mbps", fmtMicro_.Get(), D2D1::RectF(trLeft, ry + 38.0f, trLeft + 56.0f, ry + 54.0f), kColTextMuted);
    DrawTextLine(L"80 Mbps", fmtMicro_.Get(), D2D1::RectF(trRight - 64.0f, ry + 38.0f, trRight, ry + 54.0f),
        kColTextMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);

    hitTargets_.push_back({ sliderTrackRect_, HitKey{ HitAction::BitrateSlider, 0 }, true });

    ry += r3H;
    brush_->SetColor(kColDivider);
    rt_->DrawLine(D2D1::Point2F(contentX, ry), D2D1::Point2F(contentX + contentW, ry), brush_.Get(), 1.0f);

    double estMb = 0.0;
    float ratio = 0.0f;
    bool overCap = false;
    ComputeRamEstimate(estMb, ratio, overCap);

    DrawTextLine(L"Ring RAM budget", fmtBodySemi_.Get(),
        D2D1::RectF(contentX + 16.0f, ry + 7.0f, contentX + 260.0f, ry + 25.0f), kColText);
    wchar_t ramDesc[96] = {};
    if (overCap) {
        std::swprintf(ramDesc, sizeof(ramDesc) / sizeof(wchar_t),
            L"~%.0f MB estimated · will clamp replay window to fit 2 GB cap", estMb);
    } else {
        std::swprintf(ramDesc, sizeof(ramDesc) / sizeof(wchar_t),
            L"~%.0f MB allocated in RAM · 0 B disk writes while idle", estMb);
    }
    DrawTextLine(ramDesc, fmtSmall_.Get(),
        D2D1::RectF(contentX + 16.0f, ry + 26.0f, contentX + 410.0f, ry + 43.0f),
        overCap ? kColAmber : kColTextMuted);

    D2D1_RECT_F barBg = D2D1::RectF(contentX + 434.0f, ry + 20.0f, contentX + contentW - 18.0f, ry + 30.0f);
    brush_->SetColor(kColWell);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(barBg, 5.0f, 5.0f), brush_.Get());
    float fillW = std::max(8.0f, (barBg.right - barBg.left) * ratio);
    brush_->SetColor(overCap ? kColAmber : kColAccent);
    rt_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(barBg.left, barBg.top, std::min(barBg.right, barBg.left + fillW), barBg.bottom), 5.0f, 5.0f),
        brush_.Get());

    y = ry + r4H + 16.0f;
    yCursor += (y - (yCursor - scrollY_));
}

void SettingsWindow::DrawAudioTab(float contentX, float contentW, float& yCursor) {
    float y = yCursor + 18.0f - scrollY_;

    struct AudioRowDef {
        ui::IconId icon;
        const wchar_t* title;
        const wchar_t* desc;
        HitAction action;
        bool checked;
        bool hasLive;
        bool liveActive;
        const wchar_t* liveLabel;
    };

    const AudioRowDef rows[] = {
        {
            ui::IconId::SpeakerOn,
            L"Desktop Audio",
            L"Capture WASAPI system loopback across games and apps",
            HitAction::ToggleDesktopAudio,
            editedCfg_.recordDesktopAudio,
            true,
            status_.audio.desktopActive,
            status_.audio.desktopActive ? L"Streaming" : L"Muted"
        },
        {
            ui::IconId::MicOn,
            L"Microphone",
            L"Mix default communications microphone into AAC replay audio",
            HitAction::ToggleMicAudio,
            editedCfg_.recordMicAudio,
            true,
            status_.audio.micActive,
            status_.audio.micActive ? L"Streaming" : L"Inactive"
        },
        {
            ui::IconId::BellChime,
            L"Save Confirmation Chime",
            L"Play an audible chime when a replay finishes writing to disk",
            HitAction::ToggleChime,
            editedCfg_.saveChime,
            false,
            false,
            L""
        },
    };

    for (const auto& r : rows) {
        D2D1_RECT_F card = D2D1::RectF(contentX, y, contentX + contentW, y + 80.0f);
        HitKey key{ r.action, 0 };
        bool hovered = (hoveredHit_ == key);

        brush_->SetColor(hovered ? kColCardHover : kColCard);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(card, 10.0f, 10.0f), brush_.Get());
        brush_->SetColor(hovered ? kColBorderHover : kColBorder);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(card, 10.0f, 10.0f), brush_.Get(), 1.0f);

        D2D1_RECT_F well = D2D1::RectF(contentX + 18.0f, y + 18.0f, contentX + 62.0f, y + 62.0f);
        brush_->SetColor(r.checked ? kColAccentTint : kColWell);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(well, 9.0f, 9.0f), brush_.Get());
        brush_->SetColor(r.checked ? C(0xA5B4FC) : kColTextMuted);
        ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(), r.icon,
            D2D1::RectF(contentX + 29.0f, y + 29.0f, contentX + 51.0f, y + 51.0f), 1.7f);

        DrawTextLine(r.title, fmtBodySemi_.Get(),
            D2D1::RectF(contentX + 78.0f, y + 19.0f, contentX + contentW - 180.0f, y + 39.0f), kColText);
        DrawTextLine(r.desc, fmtSmall_.Get(),
            D2D1::RectF(contentX + 78.0f, y + 41.0f, contentX + contentW - 168.0f, y + 61.0f), kColTextMuted);

        if (r.hasLive) {
            D2D1_RECT_F pill = D2D1::RectF(contentX + contentW - 160.0f, y + 29.0f, contentX + contentW - 84.0f, y + 51.0f);
            brush_->SetColor(r.liveActive ? kColGreenTint : kColWell);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(pill, 11.0f, 11.0f), brush_.Get());
            DrawTextLine(r.liveLabel, fmtMicro_.Get(), pill,
                r.liveActive ? kColGreen : kColTextMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        D2D1_RECT_F tog = D2D1::RectF(contentX + contentW - 64.0f, y + 28.0f, contentX + contentW - 20.0f, y + 52.0f);
        DrawToggleSwitch(tog, r.checked, hovered);
        hitTargets_.push_back({ card, key, true });

        y += 92.0f;
    }

    y += 10.0f;
    DrawTextLine(L"Live Audio Pipeline", fmtSection_.Get(),
        D2D1::RectF(contentX, y, contentX + contentW, y + 20.0f), kColTextSec);
    y += 28.0f;

    D2D1_RECT_F statCard = D2D1::RectF(contentX, y, contentX + contentW, y + 84.0f);
    brush_->SetColor(kColCard);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(statCard, 10.0f, 10.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(statCard, 10.0f, 10.0f), brush_.Get(), 1.0f);

    struct StatCol {
        const wchar_t* label;
        uint64_t val;
    };
    const StatCol cols[4] = {
        { L"AAC FRAMES", status_.audio.aacFramesEncoded },
        { L"DESKTOP PACKETS", status_.audio.desktopPackets },
        { L"MIC PACKETS", status_.audio.micPackets },
        { L"DEVICE REOPENS", status_.audio.deviceReopens },
    };

    float colW = contentW / 4.0f;
    for (int i = 0; i < 4; ++i) {
        float cx = contentX + static_cast<float>(i) * colW;
        if (i > 0) {
            brush_->SetColor(kColDivider);
            rt_->DrawLine(D2D1::Point2F(cx, y + 16.0f), D2D1::Point2F(cx, y + 68.0f), brush_.Get(), 1.0f);
        }
        std::wstring numStr = FormatIntegerComma(cols[i].val);
        DrawTextLine(cols[i].label, fmtMicro_.Get(),
            D2D1::RectF(cx + 16.0f, y + 18.0f, cx + colW - 12.0f, y + 34.0f), kColTextMuted);
        DrawTextLine(numStr, fmtPageTitle_.Get(),
            D2D1::RectF(cx + 16.0f, y + 38.0f, cx + colW - 12.0f, y + 66.0f), kColText);
    }

    yCursor += (y + 102.0f - (yCursor - scrollY_));
}

void SettingsWindow::DrawHotkeysTab(float contentX, float contentW, float& yCursor) {
    float y = yCursor + 18.0f - scrollY_;

    D2D1_RECT_F hkCard = D2D1::RectF(contentX, y, contentX + contentW, y + 132.0f);
    brush_->SetColor(kColCard);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(hkCard, 10.0f, 10.0f), brush_.Get());
    brush_->SetColor(recordingHotkey_ ? kColAccent : kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(hkCard, 10.0f, 10.0f), brush_.Get(), recordingHotkey_ ? 1.5f : 1.0f);

    D2D1_RECT_F hkWell = D2D1::RectF(contentX + 18.0f, y + 18.0f, contentX + 58.0f, y + 58.0f);
    brush_->SetColor(kColAccentTint);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(hkWell, 9.0f, 9.0f), brush_.Get());
    brush_->SetColor(C(0xA5B4FC));
    ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(),
        ui::IconId::KeyboardKeys, D2D1::RectF(contentX + 27.0f, y + 27.0f, contentX + 49.0f, y + 49.0f), 1.65f);

    DrawTextLine(L"Save Replay Hotkey", fmtBodySemi_.Get(),
        D2D1::RectF(contentX + 72.0f, y + 18.0f, contentX + contentW - 20.0f, y + 38.0f), kColText);
    DrawTextLine(L"Global trigger captured without injecting overlays into games", fmtSmall_.Get(),
        D2D1::RectF(contentX + 72.0f, y + 38.0f, contentX + contentW - 20.0f, y + 56.0f), kColTextMuted);

    D2D1_RECT_F bindBox = D2D1::RectF(contentX + 18.0f, y + 72.0f, contentX + contentW - 196.0f, y + 114.0f);
    HitKey capKey{ HitAction::HotkeyCaptureBtn, 0 };
    bool capHover = (hoveredHit_ == capKey);
    brush_->SetColor(recordingHotkey_ ? kColAccentTint : (capHover ? kColWellHover : kColWell));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(bindBox, 8.0f, 8.0f), brush_.Get());
    brush_->SetColor(recordingHotkey_ ? kColAccent : (capHover ? kColBorderHover : kColBorder));
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(bindBox, 8.0f, 8.0f), brush_.Get(), recordingHotkey_ ? 1.5f : 1.0f);

    if (recordingHotkey_) {
        DrawTextLine(L"Listening... press your key combination", fmtBodySemi_.Get(),
            D2D1::RectF(bindBox.left + 14.0f, bindBox.top, bindBox.right - 70.0f, bindBox.bottom),
            C(0xC7D2FE));
        DrawKeycapGroup(bindBox.right - 58.0f, (bindBox.top + bindBox.bottom) * 0.5f, "Esc", true, bindBox.right - 8.0f);
    } else {
        DrawKeycapGroup(bindBox.left + 14.0f, (bindBox.top + bindBox.bottom) * 0.5f,
            editedCfg_.hotkeySave.display, true, bindBox.right - 14.0f);
    }
    hitTargets_.push_back({ bindBox, capKey, true });

    DrawPillButton(D2D1::RectF(contentX + contentW - 186.0f, y + 75.0f, contentX + contentW - 102.0f, y + 111.0f),
        recordingHotkey_ ? L"Cancel" : L"Change", nullptr, HitKey{ HitAction::HotkeyCaptureBtn, 0 }, false);
    hitTargets_.back().inViewport = true;

    ui::IconId rstIco = ui::IconId::RefreshArrows;
    DrawPillButton(D2D1::RectF(contentX + contentW - 94.0f, y + 75.0f, contentX + contentW - 18.0f, y + 111.0f),
        L"Reset", &rstIco, HitKey{ HitAction::HotkeyResetBtn, 0 }, false);
    hitTargets_.back().inViewport = true;

    y += 148.0f;

    D2D1_RECT_F dirCard = D2D1::RectF(contentX, y, contentX + contentW, y + 132.0f);
    brush_->SetColor(kColCard);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(dirCard, 10.0f, 10.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(dirCard, 10.0f, 10.0f), brush_.Get(), 1.0f);

    D2D1_RECT_F dirWell = D2D1::RectF(contentX + 18.0f, y + 18.0f, contentX + 58.0f, y + 58.0f);
    brush_->SetColor(kColWell);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(dirWell, 9.0f, 9.0f), brush_.Get());
    brush_->SetColor(kColTextSec);
    ui::DrawVectorIcon(rt_.Get(), d2dFactory_.Get(), brush_.Get(), roundStroke_.Get(),
        ui::IconId::FolderOpen, D2D1::RectF(contentX + 27.0f, y + 27.0f, contentX + 49.0f, y + 49.0f), 1.65f);

    DrawTextLine(L"Clips Storage Folder", fmtBodySemi_.Get(),
        D2D1::RectF(contentX + 72.0f, y + 18.0f, contentX + contentW - 20.0f, y + 38.0f), kColText);
    DrawTextLine(L"Fast-start MP4 replays are written atomically here when saved", fmtSmall_.Get(),
        D2D1::RectF(contentX + 72.0f, y + 38.0f, contentX + contentW - 20.0f, y + 56.0f), kColTextMuted);

    D2D1_RECT_F pathBox = D2D1::RectF(contentX + 18.0f, y + 75.0f, contentX + contentW - 224.0f, y + 111.0f);
    brush_->SetColor(kColWell);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(pathBox, 7.0f, 7.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(pathBox, 7.0f, 7.0f), brush_.Get(), 1.0f);

    std::wstring displayPath = editedCfg_.outputDir.empty() ? ExpandEnvPath(editedCfg_.outputDirRaw) : editedCfg_.outputDir;
    DrawTextLine(displayPath, fmtMono_.Get(),
        D2D1::RectF(pathBox.left + 12.0f, pathBox.top, pathBox.right - 12.0f, pathBox.bottom), kColText);

    ui::IconId brIco = ui::IconId::FolderOpen;
    DrawPillButton(D2D1::RectF(contentX + contentW - 214.0f, y + 75.0f, contentX + contentW - 110.0f, y + 111.0f),
        L"Browse", &brIco, HitKey{ HitAction::BrowseFolder, 0 }, false);
    hitTargets_.back().inViewport = true;

    ui::IconId extIco = ui::IconId::ExternalArrow;
    DrawPillButton(D2D1::RectF(contentX + contentW - 102.0f, y + 75.0f, contentX + contentW - 18.0f, y + 111.0f),
        L"Open", &extIco, HitKey{ HitAction::OpenFolderBtn, 0 }, false);
    hitTargets_.back().inViewport = true;

    y += 148.0f;

    D2D1_RECT_F tipCard = D2D1::RectF(contentX, y, contentX + contentW, y + 84.0f);
    brush_->SetColor(kColCard);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(tipCard, 10.0f, 10.0f), brush_.Get());
    brush_->SetColor(kColBorder);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(tipCard, 10.0f, 10.0f), brush_.Get(), 1.0f);

    DrawTextLine(L"Shortcut & Background Notes", fmtBodySemi_.Get(),
        D2D1::RectF(contentX + 18.0f, y + 14.0f, contentX + contentW - 18.0f, y + 34.0f), kColTextSec);
    DrawTextLine(L"Use Ctrl, Alt, or Shift with letters/numbers, or standalone F1-F24/Numpad keys. Closing Clipper keeps it active in the system tray.",
        fmtSmallWrap_.Get(),
        D2D1::RectF(contentX + 18.0f, y + 38.0f, contentX + contentW - 18.0f, y + 76.0f), kColTextMuted,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    yCursor += (y + 100.0f - (yCursor - scrollY_));
}

void SettingsWindow::DrawBottomBar() {
    D2D1_RECT_F bar = D2D1::RectF(kSidebarW, kViewBottom, kWndW, kWndH);
    brush_->SetColor(kColHeaderBg);
    rt_->FillRectangle(bar, brush_.Get());

    brush_->SetColor(kColDivider);
    rt_->DrawLine(D2D1::Point2F(kSidebarW, kViewBottom), D2D1::Point2F(kWndW, kViewBottom), brush_.Get(), 1.0f);

    bool dirty = IsDirty();
    float msgLeft = kSidebarW + kContentPadL;
    float msgRight = dirty ? 690.0f : 790.0f;

    if (!feedbackText_.empty()) {
        D2D1_COLOR_F fbCol = feedbackIsError_ ? kColRed : kColGreen;
        brush_->SetColor(fbCol);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(msgLeft + 5.0f, kViewBottom + 31.0f), 4.0f, 4.0f), brush_.Get());
        DrawTextLine(feedbackText_, fmtBodySemi_.Get(),
            D2D1::RectF(msgLeft + 16.0f, kViewBottom, msgRight, kWndH), fbCol);
    } else if (dirty) {
        brush_->SetColor(kColAmber);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(msgLeft + 5.0f, kViewBottom + 31.0f), 4.0f, 4.0f), brush_.Get());
        DrawTextLine(L"Unsaved configuration changes", fmtBodySemi_.Get(),
            D2D1::RectF(msgLeft + 16.0f, kViewBottom, msgRight, kWndH), kColAmber);
    } else {
        DrawTextLine(L"Closing this window keeps Clipper recording quietly in the system tray.",
            fmtSmall_.Get(), D2D1::RectF(msgLeft, kViewBottom, msgRight, kWndH), kColTextMuted);
    }

    if (dirty) {
        DrawPillButton(D2D1::RectF(706.0f, kViewBottom + 14.0f, 788.0f, kWndH - 14.0f),
            L"Reset", nullptr, HitKey{ HitAction::DiscardChanges, 0 }, false);
        DrawPillButton(D2D1::RectF(798.0f, kViewBottom + 14.0f, 932.0f, kWndH - 14.0f),
            L"Apply Changes", nullptr, HitKey{ HitAction::ApplyChanges, 0 }, true);
    } else {
        D2D1_RECT_F donePill = D2D1::RectF(828.0f, kViewBottom + 14.0f, 932.0f, kWndH - 14.0f);
        brush_->SetColor(kColWell);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(donePill, 7.0f, 7.0f), brush_.Get());
        brush_->SetColor(kColBorder);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(donePill, 7.0f, 7.0f), brush_.Get(), 1.0f);
        DrawTextLine(L"Up to date", fmtSmall_.Get(), donePill, kColTextMuted,
            DWRITE_TEXT_ALIGNMENT_CENTER);
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
            StepThumbnailBatch(4, false);
        }
    }

    rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(rt_->BindDC(memDC_, &rc))) return;

    hitTargets_.clear();

    rt_->BeginDraw();
    rt_->Clear(kColBg);

    DrawSidebar(D2D1::RectF(0.0f, 0.0f, kSidebarW, kWndH));
    DrawTopHeader();

    D2D1_RECT_F viewRect = D2D1::RectF(kSidebarW + 1.0f, kViewTop + 1.0f, kWndW, kViewBottom - 1.0f);
    rt_->PushAxisAlignedClip(viewRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float contentX = kSidebarW + kContentPadL;
    float yCursor = kViewTop;
    switch (activeTab_) {
        case SettingsTab::Clips:
            DrawClipsTab(contentX, kContentW, yCursor);
            break;
        case SettingsTab::Capture:
            DrawCaptureTab(contentX, kContentW, yCursor);
            break;
        case SettingsTab::Audio:
            DrawAudioTab(contentX, kContentW, yCursor);
            break;
        case SettingsTab::Hotkeys:
            DrawHotkeysTab(contentX, kContentW, yCursor);
            break;
    }

    float totalHeight = yCursor - kViewTop;
    maxScroll_ = std::max(0.0f, totalHeight - kViewH);
    scrollY_ = ClampF(scrollY_, 0.0f, maxScroll_);

    if (maxScroll_ > 1.0f) {
        float trackTop = kViewTop + 10.0f;
        float trackH = kViewH - 20.0f;
        float thumbH = std::max(36.0f, trackH * (kViewH / totalHeight));
        float thumbY = trackTop + (trackH - thumbH) * (scrollY_ / maxScroll_);
        brush_->SetColor(kColBorder);
        rt_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(948.0f, thumbY, 954.0f, thumbY + thumbH), 3.0f, 3.0f),
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
        if (it->inViewport && (dipY < kViewTop || dipY > kViewBottom)) {
            continue;
        }
        if (PtInDipRect(it->rect, dipX, dipY)) {
            return &(*it);
        }
    }
    return nullptr;
}

void SettingsWindow::UpdateBitrateFromDip(float dipX) {
    float w = sliderTrackRect_.right - sliderTrackRect_.left;
    if (w <= 1.0f) return;
    float t = ClampF((dipX - sliderTrackRect_.left) / w, 0.0f, 1.0f);
    uint32_t nextMbps = static_cast<uint32_t>(std::lround(5.0f + t * 75.0f));
    nextMbps = std::clamp(nextMbps, 5u, 80u);
    if (editedCfg_.bitrateMbps != nextMbps) {
        editedCfg_.bitrateMbps = nextMbps;
        InvalidateWindow();
    }
}

void SettingsWindow::ExecuteHit(const HitKey& key, float dipX) {
    switch (key.action) {
        case HitAction::SelectTab:
            SetActiveTab(static_cast<SettingsTab>(key.param));
            break;
        case HitAction::SidebarSaveClip:
            if (callbacks_.onSaveReplay) callbacks_.onSaveReplay();
            break;
        case HitAction::SidebarHide:
            DestroyWindow(hwnd_);
            break;
        case HitAction::SidebarQuit:
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
        case HitAction::EmptyGoCapture:
            SetActiveTab(SettingsTab::Capture);
            break;
        case HitAction::RefreshMonitors:
            ReloadMonitors();
            ShowFeedback(L"Display list refreshed.", false);
            break;
        case HitAction::SelectMonitor:
            editedCfg_.monitorIndex = static_cast<uint32_t>(key.param);
            InvalidateWindow();
            break;
        case HitAction::PresetCompetitive:
            editedCfg_.fps = 60;
            editedCfg_.bitrateMbps = 20;
            InvalidateWindow();
            break;
        case HitAction::PresetHigh:
            editedCfg_.fps = 60;
            editedCfg_.bitrateMbps = 40;
            InvalidateWindow();
            break;
        case HitAction::PresetCompact:
            editedCfg_.fps = 30;
            editedCfg_.bitrateMbps = 10;
            InvalidateWindow();
            break;
        case HitAction::SelectFps:
            editedCfg_.fps = static_cast<uint32_t>(key.param);
            InvalidateWindow();
            break;
        case HitAction::SelectSeconds:
            editedCfg_.replaySeconds = static_cast<uint32_t>(key.param);
            InvalidateWindow();
            break;
        case HitAction::BitrateSlider:
            draggingSlider_ = true;
            SetCapture(hwnd_);
            UpdateBitrateFromDip(dipX);
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
            if (recordingHotkey_) EndHotkeyCapture(true);
            else BeginHotkeyCapture();
            break;
        case HitAction::HotkeyResetBtn:
            if (recordingHotkey_) EndHotkeyCapture(false);
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
            if (recordingHotkey_) EndHotkeyCapture(false);
            editedCfg_ = appliedCfg_;
            ShowFeedback(L"Reverted unsaved changes.", false);
            break;
        case HitAction::ApplyChanges: {
            if (recordingHotkey_) EndHotkeyCapture(false);
            std::wstring fb;
            bool isErr = false;
            if (callbacks_.onApply) {
                callbacks_.onApply(editedCfg_, fb, isErr);
            }
            if (callbacks_.getConfig) {
                appliedCfg_ = callbacks_.getConfig();
                editedCfg_ = appliedCfg_;
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

void SettingsWindow::BeginHotkeyCapture() {
    if (recordingHotkey_) return;
    recordingHotkey_ = true;
    if (callbacks_.onSuspendHotkey) callbacks_.onSuspendHotkey(true);
    ShowFeedback(L"Press your shortcut combination, or Esc to cancel.", false);
    SetFocus(hwnd_);
    InvalidateWindow();
}

void SettingsWindow::EndHotkeyCapture(bool cancelMsg) {
    if (!recordingHotkey_) return;
    recordingHotkey_ = false;
    if (callbacks_.onSuspendHotkey) callbacks_.onSuspendHotkey(false);
    if (cancelMsg) {
        ShowFeedback(L"Hotkey capture canceled.", false);
    } else {
        InvalidateWindow();
    }
}

bool SettingsWindow::HandleHotkeyKeyDown(UINT vk, LPARAM lParam) {
    if (!recordingHotkey_) return false;
    if ((lParam & (1LL << 30)) != 0) return true;
    if (vk == VK_ESCAPE) {
        EndHotkeyCapture(true);
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
        EndHotkeyCapture(false);
        ShowFeedback(L"Hotkey set to " + Utf8ToWide(candidate.display) + L". Apply to save.", false);
    } else {
        ShowFeedback(L"Add Ctrl, Alt, or Shift for letters/numbers, or press F1-F24/Numpad.", true);
    }
    return true;
}

LRESULT SettingsWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == kWmSelectTab) {
        int t = static_cast<int>(wParam);
        if (t >= 0 && t <= 3) {
            SetActiveTab(static_cast<SettingsTab>(t));
        }
        return 0;
    }

    switch (msg) {
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
                bool hand = draggingSlider_ || (hoveredHit_.action != HitAction::None);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            float dipX = static_cast<float>(GET_X_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);
            float dipY = static_cast<float>(GET_Y_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);
            if (const HitTarget* hit = HitTestDip(dipX, dipY)) {
                if (recordingHotkey_ && hit->key.action != HitAction::HotkeyCaptureBtn) {
                    EndHotkeyCapture(true);
                }
                ExecuteHit(hit->key, dipX);
            } else if (recordingHotkey_) {
                EndHotkeyCapture(true);
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

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (HandleHotkeyKeyDown(static_cast<UINT>(wParam), lParam)) {
                return 0;
            }
            break;

        case WM_KILLFOCUS:
            if (draggingSlider_) {
                draggingSlider_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            if (recordingHotkey_) {
                EndHotkeyCapture(true);
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
            if (recordingHotkey_) {
                recordingHotkey_ = false;
                if (callbacks_.onSuspendHotkey) callbacks_.onSuspendHotkey(false);
            }
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
