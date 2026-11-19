#include "settings_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace clipper {

namespace {

constexpr wchar_t kSettingsWndClass[] = L"ClipperSettingsWindow";
constexpr UINT_PTR kStatusTimerId = 1;

constexpr int kClientW = 560;
constexpr int kClientH = 688;

constexpr COLORREF kColBg = RGB(14, 15, 18);
constexpr COLORREF kColCard = RGB(22, 23, 27);
constexpr COLORREF kColCardBorder = RGB(38, 40, 48);
constexpr COLORREF kColDivider = RGB(34, 36, 44);
constexpr COLORREF kColInput = RGB(30, 32, 38);
constexpr COLORREF kColInputBorder = RGB(48, 51, 61);
constexpr COLORREF kColHover = RGB(40, 43, 52);
constexpr COLORREF kColPressed = RGB(26, 28, 34);
constexpr COLORREF kColSegTrack = RGB(28, 30, 36);
constexpr COLORREF kColSegActive = RGB(52, 56, 68);
constexpr COLORREF kColSegActiveBorder = RGB(76, 82, 98);
constexpr COLORREF kColKeycap = RGB(35, 38, 46);
constexpr COLORREF kColKeycapBorder = RGB(62, 66, 78);
constexpr COLORREF kColAccent = RGB(94, 106, 210);
constexpr COLORREF kColAccentHover = RGB(110, 122, 224);
constexpr COLORREF kColAccentPressed = RGB(78, 88, 186);
constexpr COLORREF kColText = RGB(238, 240, 245);
constexpr COLORREF kColMuted = RGB(138, 143, 152);
constexpr COLORREF kColGreen = RGB(34, 197, 94);
constexpr COLORREF kColAmber = RGB(245, 158, 11);
constexpr COLORREF kColRed = RGB(239, 68, 68);
constexpr COLORREF kColTrackOff = RGB(50, 54, 66);

void InitCommonControlsOnce() {
    static bool initialized = false;
    if (!initialized) {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
        InitCommonControlsEx(&icc);
        initialized = true;
    }
}

bool ContainsValue(const std::vector<uint32_t>& v, uint32_t val) {
    return std::find(v.begin(), v.end(), val) != v.end();
}

uint32_t BlendChannel(uint32_t bg, uint32_t fg, double a) {
    return static_cast<uint32_t>(std::clamp(std::lround(bg + (fg - bg) * a), 0L, 255L));
}

void BlitSmoothCircle(HDC hdc, int x, int y, int diameter, COLORREF fg, COLORREF bg) {
    if (diameter <= 0) return;
    std::vector<uint32_t> px(static_cast<size_t>(diameter) * static_cast<size_t>(diameter), 0);
    double center = (static_cast<double>(diameter) - 1.0) * 0.5;
    double radius = static_cast<double>(diameter) * 0.5 - 0.5;

    uint32_t fgR = GetRValue(fg), fgG = GetGValue(fg), fgB = GetBValue(fg);
    uint32_t bgR = GetRValue(bg), bgG = GetGValue(bg), bgB = GetBValue(bg);

    for (int py = 0; py < diameter; ++py) {
        for (int pxX = 0; pxX < diameter; ++pxX) {
            double dx = static_cast<double>(pxX) - center;
            double dy = static_cast<double>(py) - center;
            double dist = std::sqrt(dx * dx + dy * dy);
            double cov = std::clamp(radius + 0.5 - dist, 0.0, 1.0);
            uint32_t r = BlendChannel(bgR, fgR, cov);
            uint32_t g = BlendChannel(bgG, fgG, cov);
            uint32_t b = BlendChannel(bgB, fgB, cov);
            px[static_cast<size_t>(py) * diameter + pxX] = (r << 16) | (g << 8) | b;
        }
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = diameter;
    bmi.bmiHeader.biHeight = -diameter;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, x, y, static_cast<DWORD>(diameter), static_cast<DWORD>(diameter),
        0, 0, 0, static_cast<UINT>(diameter), px.data(), &bmi, DIB_RGB_COLORS);
}

void DrawFillRoundRect(HDC hdc, RECT rc, int radius, COLORREF fill, COLORREF border) {
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
}

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

}

SettingsWindow::SettingsWindow(SettingsCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

SettingsWindow::~SettingsWindow() {
    CloseIfOpen();
}

int SettingsWindow::S(int logicalPx) const {
    return MulDiv(logicalPx, dpi_, 96);
}

void SettingsWindow::ShowOrFocus(HINSTANCE hInst, HICON icon) {
    if (hwnd_) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return;
    }

    InitCommonControlsOnce();

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

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT wr{ 0, 0, S(kClientW), S(kClientH) };
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
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::CloseIfOpen() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void SettingsWindow::RefreshStatus() {
    if (!hwnd_ || !callbacks_.getStatus) return;
    SettingsStatusInfo st = callbacks_.getStatus();

    std::wstring nextText;
    COLORREF nextColor = kColMuted;

    if (st.state == EngineState::Running) {
        wchar_t buf[128] = {};
        if (st.video.width > 0 && st.video.height > 0) {
            std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
                L"Recording  %ux%u at %u fps", st.video.width, st.video.height, selectedFps_);
        } else {
            std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"Recording at %u fps", selectedFps_);
        }
        nextText = buf;
        nextColor = kColGreen;
    } else if (st.state == EngineState::Recovering) {
        nextText = L"Recovering desktop capture...";
        nextColor = kColAmber;
    } else if (st.state == EngineState::Starting) {
        nextText = L"Starting capture pipeline...";
        nextColor = kColAmber;
    } else if (st.state == EngineState::Stopping) {
        nextText = L"Stopping...";
        nextColor = kColAmber;
    } else if (st.state == EngineState::Faulted) {
        nextText = st.lastError.empty() ? L"Capture failed to start" : (L"Faulted  " + Utf8ToWide(st.lastError));
        nextColor = kColRed;
    } else {
        nextText = L"Stopped";
        nextColor = kColMuted;
    }

    if (nextText != statusText_ || nextColor != statusColor_) {
        statusText_ = std::move(nextText);
        statusColor_ = nextColor;
        RECT rc{ S(18), S(14), S(416), S(50) };
        InvalidateRect(hwnd_, &rc, FALSE);
    }
}

void SettingsWindow::ShowFeedback(const std::wstring& text, bool isError) {
    feedbackText_ = text;
    feedbackColor_ = isError ? kColRed : kColGreen;
    if (hwnd_) {
        RECT rc{ S(18), S(612), S(544), S(636) };
        InvalidateRect(hwnd_, &rc, FALSE);
    }
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

LRESULT CALLBACK SettingsWindow::HotkeySubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                    UINT_PTR subId, DWORD_PTR refData) {
    auto* self = reinterpret_cast<SettingsWindow*>(refData);
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &SettingsWindow::HotkeySubclassProc, subId);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    if (self && self->recordingHotkey_) {
        if (msg == WM_GETDLGCODE) {
            return DLGC_WANTALLKEYS;
        }
        if (msg == WM_KILLFOCUS) {
            self->EndHotkeyCapture(true);
            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if ((lParam & (1LL << 30)) != 0) return 0;
            UINT vk = static_cast<UINT>(wParam);
            if (vk == VK_ESCAPE) {
                self->EndHotkeyCapture(true);
                return 0;
            }
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                vk == VK_LWIN || vk == VK_RWIN ||
                vk == VK_LSHIFT || vk == VK_RSHIFT ||
                vk == VK_LCONTROL || vk == VK_RCONTROL ||
                vk == VK_LMENU || vk == VK_RMENU) {
                return 0;
            }
            uint32_t mods = 0;
            if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;

            HotkeySpec candidate;
            if (FormatHotkeyFromVk(mods, vk, candidate)) {
                self->pendingHotkey_ = candidate;
                self->EndHotkeyCapture(false);
                self->ShowFeedback(L"Hotkey set to " + Utf8ToWide(candidate.display) + L". Click Apply to activate.", false);
            } else {
                self->ShowFeedback(L"Add Ctrl, Alt, or Shift for letters/numbers, or press F1-F24/Numpad.", true);
            }
            return 0;
        }
        if (msg == WM_CHAR || msg == WM_SYSCHAR) {
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SettingsWindow::ButtonHoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR subId, DWORD_PTR refData) {
    auto* self = reinterpret_cast<SettingsWindow*>(refData);
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &SettingsWindow::ButtonHoverProc, subId);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    if (self) {
        if (msg == WM_MOUSEMOVE) {
            bool redraw = (self->hoveredBtn_ != hwnd);
            self->hoveredBtn_ = hwnd;
            if (hwnd == self->btnSegFps_) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                int xPos = static_cast<short>(LOWORD(lParam));
                int nextSeg = (xPos < rc.right / 2) ? 0 : 1;
                if (nextSeg != self->hoveredSeg_) {
                    self->hoveredSeg_ = nextSeg;
                    redraw = true;
                }
            }
            if (redraw) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (msg == WM_MOUSELEAVE) {
            if (self->hoveredBtn_ == hwnd) {
                self->hoveredBtn_ = nullptr;
                if (hwnd == self->btnSegFps_) self->hoveredSeg_ = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SettingsWindow::ComboBorderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR subId, DWORD_PTR) {
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &SettingsWindow::ComboBorderProc, subId);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (msg == WM_PAINT) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            HPEN bgPen = CreatePen(PS_SOLID, 2, kColInput);
            HGDIOBJ oldPen = SelectObject(hdc, bgPen);
            HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
            SelectObject(hdc, oldPen);
            DeleteObject(bgPen);

            COLORREF edge = (GetFocus() == hwnd) ? kColAccent : kColInputBorder;
            HPEN borderPen = CreatePen(PS_SOLID, 1, edge);
            oldPen = SelectObject(hdc, borderPen);
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPen);
            DeleteObject(borderPen);
            ReleaseDC(hwnd, hdc);
        }
    }
    return lr;
}

LRESULT CALLBACK SettingsWindow::SegFpsKeyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR subId, DWORD_PTR refData) {
    auto* self = reinterpret_cast<SettingsWindow*>(refData);
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &SettingsWindow::SegFpsKeyProc, subId);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    if (self && msg == WM_KEYDOWN) {
        if (wParam == VK_LEFT && self->selectedFps_ != 30) {
            self->selectedFps_ = 30;
            InvalidateRect(hwnd, nullptr, FALSE);
            self->UpdateRamEstimate();
            return 0;
        }
        if (wParam == VK_RIGHT && self->selectedFps_ != 60) {
            self->selectedFps_ = 60;
            InvalidateRect(hwnd, nullptr, FALSE);
            self->UpdateRamEstimate();
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            fontRegular_ = CreateFontW(-S(13), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            fontBold_ = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            fontSection_ = CreateFontW(-S(12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            fontDesc_ = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            fontKeycap_ = CreateFontW(-S(11), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");
            brushBg_ = CreateSolidBrush(kColBg);
            brushCard_ = CreateSolidBrush(kColCard);
            brushInput_ = CreateSolidBrush(kColInput);

            CreateControls();
            if (callbacks_.getConfig) {
                PopulateFromConfig(callbacks_.getConfig());
            }
            RefreshStatus();
            SetTimer(hwnd_, kStatusTimerId, 1000, nullptr);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd_, &ps);
            RECT client{};
            GetClientRect(hwnd_, &client);
            FillRect(hdc, &client, brushBg_);

            RECT statusPill{ S(20), S(15), S(412), S(45) };
            DrawFillRoundRect(hdc, statusPill, S(8), kColCard, kColCardBorder);
            BlitSmoothCircle(hdc, S(31), S(26), S(8), statusColor_, kColCard);

            SetBkMode(hdc, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(hdc, fontBold_);
            SetTextColor(hdc, kColText);
            RECT statusRc{ S(46), S(15), S(402), S(45) };
            DrawTextW(hdc, statusText_.c_str(), -1, &statusRc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            SelectObject(hdc, fontSection_);
            SetTextColor(hdc, kColMuted);
            RECT sec1{ S(22), S(60), S(400), S(80) };
            DrawTextW(hdc, L"Capture", -1, &sec1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT sec2{ S(22), S(304), S(400), S(324) };
            DrawTextW(hdc, L"Audio & alerts", -1, &sec2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT sec3{ S(22), S(480), S(400), S(500) };
            DrawTextW(hdc, L"Shortcut & storage", -1, &sec3, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            DrawRowContainer(hdc, RECT{ S(20), S(84), S(540), S(292) }, { S(136), S(188), S(240) });
            DrawRowContainer(hdc, RECT{ S(20), S(328), S(540), S(466) }, { S(374), S(420) });
            DrawRowContainer(hdc, RECT{ S(20), S(504), S(540), S(606) }, { S(555) });

            struct RowText {
                int yTop;
                int rowH;
                int rightLimit;
                const wchar_t* title;
                const wchar_t* desc;
                COLORREF descColor;
            };
            const RowText rows[] = {
                { 84, 52, 198, L"Display", L"Monitor captured via DXGI", kColMuted },
                { 136, 52, 372, L"Frame rate", L"Capture and encode cadence", kColMuted },
                { 188, 52, 396, L"Replay window", ramText_.c_str(), ramColor_ },
                { 240, 52, 396, L"Video bitrate", L"NVENC low-latency VBR target", kColMuted },
                { 328, 46, 470, L"Desktop audio", L"WASAPI system loopback", kColMuted },
                { 374, 46, 470, L"Microphone", L"Mixed into the replay audio track", kColMuted },
                { 420, 46, 470, L"Save chime", L"Plays a tone when a clip saves", kColMuted },
                { 504, 51, 244, L"Save replay hotkey", L"Click keycaps to bind", kColMuted },
                { 555, 51, 182, L"Clips folder", L"Fast-start MP4 destination", kColMuted },
            };

            for (const auto& r : rows) {
                int midY = r.yTop + r.rowH / 2;
                SelectObject(hdc, fontBold_);
                SetTextColor(hdc, kColText);
                RECT tRc{ S(36), S(r.yTop + 6), S(r.rightLimit), S(midY + 2) };
                DrawTextW(hdc, r.title, -1, &tRc,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

                SelectObject(hdc, fontDesc_);
                SetTextColor(hdc, r.descColor);
                RECT dRc{ S(36), S(midY + 2), S(r.rightLimit), S(r.yTop + r.rowH - 5) };
                DrawTextW(hdc, r.desc, -1, &dRc,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            RECT editBox{ S(190), S(567), S(406), S(595) };
            DrawFillRoundRect(hdc, editBox, S(6), kColInput, kColInputBorder);

            SelectObject(hdc, fontDesc_);
            if (!feedbackText_.empty()) {
                SelectObject(hdc, fontBold_);
                SetTextColor(hdc, feedbackColor_);
                RECT fbRc{ S(22), S(612), S(538), S(634) };
                DrawTextW(hdc, feedbackText_.c_str(), -1, &fbRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            } else {
                SetTextColor(hdc, kColMuted);
                RECT hintRc{ S(22), S(612), S(538), S(634) };
                DrawTextW(hdc, L"Closing this window keeps Clipper running in your system tray.", -1, &hintRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            SelectObject(hdc, oldFont);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, kColText);
            SetBkColor(hdc, kColInput);
            return reinterpret_cast<LRESULT>(brushInput_);
        }

        case WM_MEASUREITEM: {
            auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mis->CtlType == ODT_COMBOBOX) {
                mis->itemHeight = static_cast<UINT>(S(22));
                return TRUE;
            }
            break;
        }

        case WM_DRAWITEM: {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (dis->CtlType == ODT_COMBOBOX) {
                DrawComboItem(dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_BUTTON) {
                switch (dis->CtlID) {
                    case kIdSegFps:
                        DrawSegFpsItem(dis);
                        return TRUE;
                    case kIdBtnHotkey:
                        DrawHotkeyItem(dis);
                        return TRUE;
                    case kIdToggleDesktopAudio:
                        DrawToggleItem(dis, desktopAudio_);
                        return TRUE;
                    case kIdToggleMicAudio:
                        DrawToggleItem(dis, micAudio_);
                        return TRUE;
                    case kIdToggleChime:
                        DrawToggleItem(dis, saveChime_);
                        return TRUE;
                    default:
                        DrawButtonItem(dis);
                        return TRUE;
                }
            }
            break;
        }

        case WM_TIMER:
            if (wParam == kStatusTimerId) {
                RefreshStatus();
                return 0;
            }
            break;

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if ((id == kIdComboSeconds || id == kIdComboBitrate) && code == CBN_SELCHANGE) {
                UpdateRamEstimate();
                return 0;
            }
            if (code == BN_CLICKED) {
                switch (id) {
                    case kIdBtnRefreshMonitors: {
                        uint32_t cur = SelectedComboData(comboMonitor_, 0);
                        PopulateMonitors(cur);
                        ShowFeedback(L"Display list refreshed.", false);
                        return 0;
                    }
                    case kIdSegFps: {
                        POINT pt{};
                        if (GetCursorPos(&pt)) {
                            ScreenToClient(btnSegFps_, &pt);
                            RECT rc{};
                            GetClientRect(btnSegFps_, &rc);
                            if (PtInRect(&rc, pt)) {
                                selectedFps_ = (pt.x < rc.right / 2) ? 30u : 60u;
                            } else {
                                selectedFps_ = (selectedFps_ == 30u) ? 60u : 30u;
                            }
                        } else {
                            selectedFps_ = (selectedFps_ == 30u) ? 60u : 30u;
                        }
                        InvalidateRect(btnSegFps_, nullptr, FALSE);
                        UpdateRamEstimate();
                        return 0;
                    }
                    case kIdToggleDesktopAudio:
                        desktopAudio_ = !desktopAudio_;
                        InvalidateRect(btnDesktopAudio_, nullptr, FALSE);
                        return 0;
                    case kIdToggleMicAudio:
                        micAudio_ = !micAudio_;
                        InvalidateRect(btnMicAudio_, nullptr, FALSE);
                        return 0;
                    case kIdToggleChime:
                        saveChime_ = !saveChime_;
                        InvalidateRect(btnSaveChime_, nullptr, FALSE);
                        return 0;
                    case kIdBtnHotkey:
                        if (recordingHotkey_) EndHotkeyCapture(true);
                        else BeginHotkeyCapture();
                        return 0;
                    case kIdBtnResetHotkey:
                        if (recordingHotkey_) EndHotkeyCapture(false);
                        pendingHotkey_ = MakeDefaultConfig().hotkeySave;
                        InvalidateRect(btnHotkey_, nullptr, FALSE);
                        ShowFeedback(L"Hotkey reset to " + Utf8ToWide(pendingHotkey_.display) + L". Click Apply to activate.", false);
                        return 0;
                    case kIdBtnBrowseDir:
                        BrowseForOutputFolder();
                        return 0;
                    case kIdBtnOpenDir: {
                        std::wstring dir = ReadExpandedEditFolder();
                        std::error_code ec;
                        std::filesystem::create_directories(dir, ec);
                        ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                    case kIdBtnSaveReplay:
                        if (callbacks_.onSaveReplay) callbacks_.onSaveReplay();
                        return 0;
                    case kIdBtnHideTray:
                        DestroyWindow(hwnd_);
                        return 0;
                    case kIdBtnQuit:
                        if (callbacks_.onQuit) callbacks_.onQuit();
                        return 0;
                    case kIdBtnApply: {
                        if (recordingHotkey_) EndHotkeyCapture(false);
                        AppConfig nextCfg = GatherEditedConfig();
                        std::wstring feedback;
                        bool isErr = false;
                        if (callbacks_.onApply) {
                            callbacks_.onApply(nextCfg, feedback, isErr);
                        }
                        if (callbacks_.getConfig) {
                            PopulateFromConfig(callbacks_.getConfig());
                        }
                        if (!feedback.empty()) ShowFeedback(feedback, isErr);
                        RefreshStatus();
                        return 0;
                    }
                    default:
                        break;
                }
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY: {
            if (recordingHotkey_) {
                recordingHotkey_ = false;
                if (callbacks_.onSuspendHotkey) callbacks_.onSuspendHotkey(false);
            }
            KillTimer(hwnd_, kStatusTimerId);
            DestroyGdiResources();
            hoveredBtn_ = nullptr;
            hoveredSeg_ = -1;
            hwnd_ = nullptr;
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void SettingsWindow::CreateControls() {
    auto createCombo = [&](int id, int x, int y, int w) -> HWND {
        HWND h = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
            S(x), S(y), S(w), S(220),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(fontRegular_), TRUE);
        SendMessageW(h, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(22));
        SendMessageW(h, CB_SETITEMHEIGHT, 0, S(24));
        SetWindowTheme(h, L"DarkMode_CFD", nullptr);
        SetWindowSubclass(h, &SettingsWindow::ComboBorderProc, 3, reinterpret_cast<DWORD_PTR>(this));
        return h;
    };

    auto createBtn = [&](int id, const wchar_t* text, int x, int y, int w, int h, bool trackHover = true) -> HWND {
        HWND btn = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            S(x), S(y), S(w), S(h),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(fontRegular_), TRUE);
        if (trackHover) {
            SetWindowSubclass(btn, &SettingsWindow::ButtonHoverProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
        return btn;
    };

    btnSaveReplay_ = createBtn(kIdBtnSaveReplay, L"Save Replay", 424, 15, 116, 30);

    comboMonitor_ = createCombo(kIdComboMonitor, 206, 96, 280);
    btnRefresh_ = createBtn(kIdBtnRefreshMonitors, L"\x21BB", 492, 96, 32, 28);

    btnSegFps_ = createBtn(kIdSegFps, L"", 384, 147, 140, 30);
    SetWindowSubclass(btnSegFps_, &SettingsWindow::SegFpsKeyProc, 4, reinterpret_cast<DWORD_PTR>(this));

    comboSeconds_ = createCombo(kIdComboSeconds, 408, 200, 116);
    comboBitrate_ = createCombo(kIdComboBitrate, 408, 252, 116);

    btnDesktopAudio_ = createBtn(kIdToggleDesktopAudio, L"", 486, 341, 38, 20, false);
    btnMicAudio_ = createBtn(kIdToggleMicAudio, L"", 486, 387, 38, 20, false);
    btnSaveChime_ = createBtn(kIdToggleChime, L"", 486, 433, 38, 20, false);

    btnHotkey_ = createBtn(kIdBtnHotkey, L"", 252, 514, 212, 30);
    SetWindowSubclass(btnHotkey_, &SettingsWindow::HotkeySubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
    btnResetHotkey_ = createBtn(kIdBtnResetHotkey, L"Reset", 470, 514, 54, 30);

    editOutputDir_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        S(196), S(572), S(204), S(18),
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditOutputDir)), nullptr, nullptr);
    SendMessageW(editOutputDir_, WM_SETFONT, reinterpret_cast<WPARAM>(fontRegular_), TRUE);
    SetWindowTheme(editOutputDir_, L"DarkMode_Explorer", nullptr);

    btnBrowse_ = createBtn(kIdBtnBrowseDir, L"Browse", 412, 567, 58, 28);
    btnOpen_ = createBtn(kIdBtnOpenDir, L"Open", 476, 567, 48, 28);

    btnHide_ = createBtn(kIdBtnHideTray, L"Hide to Tray", 20, 640, 108, 32);
    btnQuit_ = createBtn(kIdBtnQuit, L"Quit", 136, 640, 72, 32);
    btnApply_ = createBtn(kIdBtnApply, L"Apply", 432, 640, 108, 32);
}

void SettingsWindow::PopulateMonitors(uint32_t selectedIndex) {
    SendMessageW(comboMonitor_, CB_RESETCONTENT, 0, 0);
    monitors_ = EnumerateCaptureMonitors();

    if (monitors_.empty()) {
        int idx = static_cast<int>(SendMessageW(comboMonitor_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"1: Primary Display")));
        SendMessageW(comboMonitor_, CB_SETITEMDATA, idx, 0);
        SendMessageW(comboMonitor_, CB_SETCURSEL, 0, 0);
        return;
    }

    int selPos = 0;
    for (size_t i = 0; i < monitors_.size(); ++i) {
        const auto& m = monitors_[i];
        wchar_t label[256] = {};
        if (m.width > 0 && m.height > 0 && m.refreshHz > 0) {
            std::swprintf(label, sizeof(label) / sizeof(wchar_t), L"%u: %ls (%ux%u @ %uHz)%ls",
                m.index + 1, m.friendlyName.c_str(), m.width, m.height, m.refreshHz,
                m.isPrimary ? L" [Primary]" : L"");
        } else {
            std::swprintf(label, sizeof(label) / sizeof(wchar_t), L"%u: %ls%ls",
                m.index + 1, m.friendlyName.c_str(), m.isPrimary ? L" [Primary]" : L"");
        }
        int itemIdx = static_cast<int>(SendMessageW(comboMonitor_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)));
        SendMessageW(comboMonitor_, CB_SETITEMDATA, itemIdx, static_cast<LPARAM>(m.index));
        if (m.index == selectedIndex) selPos = itemIdx;
    }

    if (selectedIndex >= monitors_.size()) {
        wchar_t missing[96] = {};
        std::swprintf(missing, sizeof(missing) / sizeof(wchar_t), L"%u: Disconnected display", selectedIndex + 1);
        int itemIdx = static_cast<int>(SendMessageW(comboMonitor_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(missing)));
        SendMessageW(comboMonitor_, CB_SETITEMDATA, itemIdx, static_cast<LPARAM>(selectedIndex));
        selPos = itemIdx;
    }

    SendMessageW(comboMonitor_, CB_SETCURSEL, selPos, 0);
}

void SettingsWindow::PopulateNumericCombo(HWND combo, const std::vector<uint32_t>& presets,
                                          uint32_t currentVal, const wchar_t* suffix) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    std::vector<uint32_t> vals = presets;
    if (!ContainsValue(vals, currentVal)) {
        vals.push_back(currentVal);
        std::sort(vals.begin(), vals.end());
    }
    int sel = 0;
    for (size_t i = 0; i < vals.size(); ++i) {
        wchar_t label[48] = {};
        std::swprintf(label, sizeof(label) / sizeof(wchar_t), L"%u%ls", vals[i], suffix);
        int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)));
        SendMessageW(combo, CB_SETITEMDATA, idx, static_cast<LPARAM>(vals[i]));
        if (vals[i] == currentVal) sel = idx;
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

uint32_t SettingsWindow::SelectedComboData(HWND combo, uint32_t fallback) const {
    LRESULT idx = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return fallback;
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(idx), 0);
    if (data == CB_ERR) return fallback;
    return static_cast<uint32_t>(data);
}

void SettingsWindow::PopulateFromConfig(const AppConfig& cfg) {
    PopulateMonitors(cfg.monitorIndex);
    selectedFps_ = (cfg.fps <= 45) ? 30u : 60u;
    PopulateNumericCombo(comboSeconds_, { 15, 30, 45, 60, 90, 120, 180, 300 }, cfg.replaySeconds, L" s");
    PopulateNumericCombo(comboBitrate_, { 5, 10, 15, 20, 25, 30, 40, 50, 60, 80 }, cfg.bitrateMbps, L" Mbps");

    desktopAudio_ = cfg.recordDesktopAudio;
    micAudio_ = cfg.recordMicAudio;
    saveChime_ = cfg.saveChime;
    pendingHotkey_ = cfg.hotkeySave;

    std::wstring dirText = cfg.outputDirRaw.empty() ? cfg.outputDir : Utf8ToWide(cfg.outputDirRaw);
    SetWindowTextW(editOutputDir_, dirText.c_str());

    InvalidateRect(btnSegFps_, nullptr, FALSE);
    InvalidateRect(btnDesktopAudio_, nullptr, FALSE);
    InvalidateRect(btnMicAudio_, nullptr, FALSE);
    InvalidateRect(btnSaveChime_, nullptr, FALSE);
    InvalidateRect(btnHotkey_, nullptr, FALSE);
    UpdateRamEstimate();
}

std::wstring SettingsWindow::ReadExpandedEditFolder() const {
    int len = GetWindowTextLengthW(editOutputDir_);
    std::wstring buf(static_cast<size_t>(len + 1), L'\0');
    GetWindowTextW(editOutputDir_, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(len));
    std::string raw = WideToUtf8(buf);
    if (raw.empty()) raw = MakeDefaultConfig().outputDirRaw;
    return ExpandEnvPath(raw);
}

AppConfig SettingsWindow::GatherEditedConfig() const {
    AppConfig cfg = callbacks_.getConfig ? callbacks_.getConfig() : MakeDefaultConfig();
    cfg.monitorIndex = SelectedComboData(comboMonitor_, cfg.monitorIndex);
    cfg.fps = selectedFps_;
    cfg.replaySeconds = SelectedComboData(comboSeconds_, cfg.replaySeconds);
    cfg.bitrateMbps = SelectedComboData(comboBitrate_, cfg.bitrateMbps);
    cfg.recordDesktopAudio = desktopAudio_;
    cfg.recordMicAudio = micAudio_;
    cfg.saveChime = saveChime_;
    cfg.hotkeySave = pendingHotkey_;

    int len = GetWindowTextLengthW(editOutputDir_);
    std::wstring buf(static_cast<size_t>(len + 1), L'\0');
    GetWindowTextW(editOutputDir_, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(len));
    cfg.outputDirRaw = WideToUtf8(buf);
    return cfg;
}

void SettingsWindow::UpdateRamEstimate() {
    AppConfig probe = MakeDefaultConfig();
    probe.bitrateMbps = SelectedComboData(comboBitrate_, 20);
    probe.replaySeconds = SelectedComboData(comboSeconds_, 60);
    probe.fps = selectedFps_;

    bool overBudget = (static_cast<uint64_t>(probe.bitrateMbps) * probe.replaySeconds) > kMaxBitrateSecondsBudget;
    AppConfig clamped = probe;
    std::vector<std::string> warnings;
    ValidateAppConfig(clamped, warnings);
    uint64_t mb = (clamped.VideoArenaBytes() + clamped.AudioArenaBytes()) / (1024ULL * 1024ULL);

    wchar_t buf[96] = {};
    if (overBudget) {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
            L"RAM buffer ~%llu MB (capped at %us)", static_cast<unsigned long long>(mb), clamped.replaySeconds);
        ramColor_ = kColAmber;
    } else {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
            L"RAM buffer ~%llu MB", static_cast<unsigned long long>(mb));
        ramColor_ = kColMuted;
    }
    ramText_ = buf;
    if (hwnd_) {
        RECT rc{ S(34), S(214), S(396), S(238) };
        InvalidateRect(hwnd_, &rc, FALSE);
    }
}

void SettingsWindow::BrowseForOutputFolder() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);

    std::wstring curDir = ReadExpandedEditFolder();
    Microsoft::WRL::ComPtr<IShellItem> initFolder;
    if (SUCCEEDED(SHCreateItemFromParsingName(curDir.c_str(), nullptr, IID_PPV_ARGS(&initFolder)))) {
        dlg->SetFolder(initFolder.Get());
    }

    if (SUCCEEDED(dlg->Show(hwnd_))) {
        Microsoft::WRL::ComPtr<IShellItem> res;
        if (SUCCEEDED(dlg->GetResult(&res))) {
            PWSTR pathPtr = nullptr;
            if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &pathPtr)) && pathPtr) {
                SetWindowTextW(editOutputDir_, pathPtr);
                CoTaskMemFree(pathPtr);
            }
        }
    }
}

void SettingsWindow::BeginHotkeyCapture() {
    if (recordingHotkey_) return;
    recordingHotkey_ = true;
    if (callbacks_.onSuspendHotkey) callbacks_.onSuspendHotkey(true);
    SetFocus(btnHotkey_);
    InvalidateRect(btnHotkey_, nullptr, FALSE);
    ShowFeedback(L"Press your desired keybind now (Esc cancels).", false);
}

void SettingsWindow::EndHotkeyCapture(bool cancelMsg) {
    if (!recordingHotkey_) return;
    recordingHotkey_ = false;
    if (callbacks_.onSuspendHotkey) callbacks_.onSuspendHotkey(false);
    InvalidateRect(btnHotkey_, nullptr, FALSE);
    if (cancelMsg) {
        ShowFeedback(L"Hotkey edit canceled.", false);
    }
}

void SettingsWindow::DrawRowContainer(HDC hdc, RECT rc, const std::vector<int>& dividerY) const {
    DrawFillRoundRect(hdc, rc, S(10), kColCard, kColCardBorder);
    HPEN divPen = CreatePen(PS_SOLID, 1, kColDivider);
    HGDIOBJ oldPen = SelectObject(hdc, divPen);
    for (int y : dividerY) {
        MoveToEx(hdc, rc.left + S(16), y, nullptr);
        LineTo(hdc, rc.right - S(16), y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(divPen);
}

void SettingsWindow::DrawButtonItem(const DRAWITEMSTRUCT* dis) const {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool isPrimary = (dis->CtlID == kIdBtnApply);
    bool isQuit = (dis->CtlID == kIdBtnQuit);
    bool isHeaderOrFooter = (dis->CtlID == kIdBtnSaveReplay || dis->CtlID == kIdBtnHideTray ||
                             dis->CtlID == kIdBtnQuit || dis->CtlID == kIdBtnApply);
    bool isPressed = (dis->itemState & ODS_SELECTED) != 0;
    bool isHovered = (dis->hwndItem == hoveredBtn_);
    bool isFocus = (dis->itemState & ODS_FOCUS) != 0;

    COLORREF bg = kColInput;
    COLORREF border = kColInputBorder;
    COLORREF fg = kColText;

    if (isPrimary) {
        bg = isPressed ? kColAccentPressed : (isHovered ? kColAccentHover : kColAccent);
        border = bg;
    } else if (isQuit && isHovered) {
        bg = RGB(68, 30, 34);
        border = kColRed;
        fg = RGB(254, 202, 202);
    } else {
        bg = isPressed ? kColPressed : (isHovered ? kColHover : kColInput);
        if (isFocus) border = kColAccent;
    }

    FillRect(hdc, &rc, isHeaderOrFooter ? brushBg_ : brushCard_);
    DrawFillRoundRect(hdc, rc, S(7), bg, border);

    wchar_t buf[128] = {};
    GetWindowTextW(dis->hwndItem, buf, 128);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    HGDIOBJ oldFont = SelectObject(hdc, isPrimary ? fontBold_ : fontRegular_);
    DrawTextW(hdc, buf, -1, const_cast<RECT*>(&rc),
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

void SettingsWindow::DrawSegFpsItem(const DRAWITEMSTRUCT* dis) const {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    FillRect(hdc, &rc, brushCard_);

    COLORREF outerBorder = ((dis->itemState & ODS_FOCUS) != 0) ? kColAccent : kColInputBorder;
    DrawFillRoundRect(hdc, rc, S(8), kColSegTrack, outerBorder);

    int pad = S(3);
    int midX = (rc.left + rc.right) / 2;
    RECT leftRc{ rc.left + pad, rc.top + pad, midX - S(1), rc.bottom - pad };
    RECT rightRc{ midX + S(1), rc.top + pad, rc.right - pad, rc.bottom - pad };

    struct SegInfo { RECT rc; const wchar_t* label; bool active; bool hover; };
    const SegInfo segs[2] = {
        { leftRc, L"30 fps", selectedFps_ == 30u, hoveredBtn_ == btnSegFps_ && hoveredSeg_ == 0 },
        { rightRc, L"60 fps", selectedFps_ == 60u, hoveredBtn_ == btnSegFps_ && hoveredSeg_ == 1 },
    };

    SetBkMode(hdc, TRANSPARENT);
    for (const auto& s : segs) {
        if (s.active) {
            DrawFillRoundRect(hdc, s.rc, S(6), kColSegActive, kColSegActiveBorder);
        } else if (s.hover) {
            DrawFillRoundRect(hdc, s.rc, S(6), kColHover, kColHover);
        }
        HGDIOBJ oldFont = SelectObject(hdc, s.active ? fontBold_ : fontRegular_);
        SetTextColor(hdc, s.active ? kColText : kColMuted);
        DrawTextW(hdc, s.label, -1, const_cast<RECT*>(&s.rc),
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
    }
}

void SettingsWindow::DrawHotkeyItem(const DRAWITEMSTRUCT* dis) const {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    FillRect(hdc, &rc, brushCard_);

    bool hovered = (dis->hwndItem == hoveredBtn_);
    bool focus = (dis->itemState & ODS_FOCUS) != 0;

    if (recordingHotkey_) {
        DrawFillRoundRect(hdc, rc, S(7), RGB(28, 34, 52), kColAccent);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kColText);
        HGDIOBJ oldFont = SelectObject(hdc, fontDesc_);
        RECT promptRc{ rc.left + S(10), rc.top, rc.right - S(48), rc.bottom };
        DrawTextW(hdc, L"Press keybind...", -1, &promptRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT escRc{ rc.right - S(42), rc.top + S(4), rc.right - S(6), rc.bottom - S(4) };
        DrawFillRoundRect(hdc, escRc, S(5), kColKeycap, kColKeycapBorder);
        SelectObject(hdc, fontKeycap_);
        SetTextColor(hdc, kColMuted);
        DrawTextW(hdc, L"Esc", -1, &escRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
        return;
    }

    if (hovered || focus) {
        DrawFillRoundRect(hdc, rc, S(7), RGB(26, 28, 34), focus ? kColAccent : kColInputBorder);
    }

    auto tokens = SplitKeycaps(pendingHotkey_.display);
    if (tokens.empty()) return;

    HGDIOBJ oldFont = SelectObject(hdc, fontKeycap_);
    SetBkMode(hdc, TRANSPARENT);

    int cursorX = rc.right - S(2);
    int capH = S(22);
    int capY = rc.top + ((rc.bottom - rc.top) - capH) / 2;

    for (size_t idx = tokens.size(); idx > 0; --idx) {
        const std::wstring& tok = tokens[idx - 1];
        RECT calc{ 0, 0, 0, 0 };
        DrawTextW(hdc, tok.c_str(), -1, &calc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        int capW = std::max(S(24), static_cast<int>(calc.right - calc.left) + S(14));
        int leftX = cursorX - capW;
        if (leftX < rc.left + S(2)) break;

        RECT chipRc{ leftX, capY, cursorX, capY + capH };
        DrawFillRoundRect(hdc, chipRc, S(5), kColKeycap, hovered ? kColSegActiveBorder : kColKeycapBorder);
        SetTextColor(hdc, kColText);
        DrawTextW(hdc, tok.c_str(), -1, &chipRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        cursorX = leftX - S(5);
    }
    SelectObject(hdc, oldFont);
}

void SettingsWindow::DrawToggleItem(const DRAWITEMSTRUCT* dis, bool checked) const {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    FillRect(hdc, &rc, brushCard_);

    int trackW = S(36);
    int trackH = S(20);
    int trackX = rc.right - trackW;
    int trackY = rc.top + ((rc.bottom - rc.top) - trackH) / 2;

    COLORREF trackCol = checked ? kColAccent : kColTrackOff;
    RECT trackRc{ trackX, trackY, trackX + trackW, trackY + trackH };
    DrawFillRoundRect(hdc, trackRc, trackH, trackCol, trackCol);

    int pad = S(3);
    int thumbSz = trackH - 2 * pad;
    int thumbX = checked ? (trackX + trackW - pad - thumbSz) : (trackX + pad);
    int thumbY = trackY + pad;
    BlitSmoothCircle(hdc, thumbX, thumbY, thumbSz, kColText, trackCol);
}

void SettingsWindow::DrawComboItem(const DRAWITEMSTRUCT* dis) const {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool isSelected = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF bg = isSelected ? kColHover : kColInput;

    HBRUSH bgBr = CreateSolidBrush(bg);
    FillRect(hdc, &rc, bgBr);
    DeleteObject(bgBr);

    if (dis->itemID == static_cast<UINT>(-1)) return;

    LRESULT len = SendMessageW(dis->hwndItem, CB_GETLBTEXTLEN, dis->itemID, 0);
    if (len == CB_ERR || len < 0) return;
    std::wstring text(static_cast<size_t>(len + 1), L'\0');
    SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(len));

    RECT textRc = rc;
    textRc.left += S(8);
    textRc.right -= S(4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kColText);
    HGDIOBJ oldFont = SelectObject(hdc, fontRegular_);
    DrawTextW(hdc, text.c_str(), -1, &textRc,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

void SettingsWindow::DestroyGdiResources() {
    auto del = [](HGDIOBJ& obj) {
        if (obj) {
            DeleteObject(obj);
            obj = nullptr;
        }
    };
    del(reinterpret_cast<HGDIOBJ&>(fontRegular_));
    del(reinterpret_cast<HGDIOBJ&>(fontBold_));
    del(reinterpret_cast<HGDIOBJ&>(fontSection_));
    del(reinterpret_cast<HGDIOBJ&>(fontDesc_));
    del(reinterpret_cast<HGDIOBJ&>(fontKeycap_));
    del(reinterpret_cast<HGDIOBJ&>(brushBg_));
    del(reinterpret_cast<HGDIOBJ&>(brushCard_));
    del(reinterpret_cast<HGDIOBJ&>(brushInput_));
}

}
