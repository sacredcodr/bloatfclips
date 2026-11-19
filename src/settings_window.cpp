#include "settings_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace clipper {

namespace {

constexpr wchar_t kSettingsWndClass[] = L"ClipperSettingsWindow";
constexpr UINT_PTR kStatusTimerId = 1;

constexpr COLORREF kColBg = RGB(24, 26, 31);
constexpr COLORREF kColCard = RGB(33, 36, 43);
constexpr COLORREF kColCardBorder = RGB(48, 53, 64);
constexpr COLORREF kColInput = RGB(42, 46, 55);
constexpr COLORREF kColInputBorder = RGB(62, 68, 82);
constexpr COLORREF kColHover = RGB(54, 60, 72);
constexpr COLORREF kColPressed = RGB(35, 39, 47);
constexpr COLORREF kColAccent = RGB(59, 130, 246);
constexpr COLORREF kColAccentHover = RGB(79, 145, 252);
constexpr COLORREF kColAccentPressed = RGB(37, 99, 235);
constexpr COLORREF kColText = RGB(237, 240, 245);
constexpr COLORREF kColMuted = RGB(154, 163, 178);
constexpr COLORREF kColGreen = RGB(34, 197, 94);
constexpr COLORREF kColAmber = RGB(245, 158, 11);
constexpr COLORREF kColRed = RGB(239, 68, 68);
constexpr COLORREF kColTrackOff = RGB(60, 66, 78);

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
    RECT wr{ 0, 0, S(540), S(508) };
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
        uint32_t fpsVal = SelectedComboData(comboFps_, 60);
        wchar_t buf[128] = {};
        if (st.video.width > 0 && st.video.height > 0) {
            std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
                L"Recording  %ux%u @ %u fps", st.video.width, st.video.height, fpsVal);
        } else {
            std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"Recording");
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
        RECT rc{ 0, 0, S(400), S(52) };
        InvalidateRect(hwnd_, &rc, FALSE);
    }
}

void SettingsWindow::ShowFeedback(const std::wstring& text, bool isError) {
    feedbackText_ = text;
    feedbackColor_ = isError ? kColRed : kColGreen;
    if (hwnd_) {
        RECT rc{ 0, S(412), S(540), S(450) };
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
            if (self->hoveredBtn_ != hwnd) {
                self->hoveredBtn_ = hwnd;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (msg == WM_MOUSELEAVE) {
            if (self->hoveredBtn_ == hwnd) {
                self->hoveredBtn_ = nullptr;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            fontRegular_ = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            fontBold_ = CreateFontW(-S(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            fontSmall_ = CreateFontW(-S(11), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
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

            DrawCard(hdc, RECT{ S(16), S(54), S(524), S(246) });
            DrawCard(hdc, RECT{ S(16), S(260), S(524), S(406) });

            SetBkMode(hdc, TRANSPARENT);

            HGDIOBJ dotBrush = CreateSolidBrush(statusColor_);
            HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
            HGDIOBJ oldBrush = SelectObject(hdc, dotBrush);
            Ellipse(hdc, S(18), S(22), S(28), S(32));
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(dotBrush);

            HGDIOBJ oldFont = SelectObject(hdc, fontBold_);
            SetTextColor(hdc, kColText);
            RECT statusRc{ S(36), S(16), S(398), S(40) };
            DrawTextW(hdc, statusText_.c_str(), -1, &statusRc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(hdc, fontSmall_);
            SetTextColor(hdc, kColMuted);
            RECT h1{ S(30), S(64), S(400), S(82) };
            DrawTextW(hdc, L"CAPTURE & VIDEO", -1, &h1, DT_LEFT | DT_SINGLELINE);
            RECT h2{ S(30), S(270), S(400), S(288) };
            DrawTextW(hdc, L"HOTKEY & CLIPS FOLDER", -1, &h2, DT_LEFT | DT_SINGLELINE);

            SelectObject(hdc, fontRegular_);
            SetTextColor(hdc, kColMuted);
            struct LabelPos { int x; int y; int w; int h; const wchar_t* text; };
            const LabelPos labels[] = {
                { 30, 90, 96, 28, L"Screen" },
                { 30, 128, 96, 28, L"Frame Rate" },
                { 266, 128, 64, 28, L"Replay" },
                { 30, 166, 96, 28, L"Bitrate" },
                { 30, 296, 96, 30, L"Save Hotkey" },
                { 30, 338, 96, 28, L"Clips Folder" },
            };
            for (const auto& lbl : labels) {
                RECT lrc{ S(lbl.x), S(lbl.y), S(lbl.x + lbl.w), S(lbl.y + lbl.h) };
                DrawTextW(hdc, lbl.text, -1, &lrc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }

            SetTextColor(hdc, ramColor_);
            RECT ramRc{ S(266), S(166), S(510), S(194) };
            DrawTextW(hdc, ramText_.c_str(), -1, &ramRc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SetTextColor(hdc, kColMuted);
            RECT hintRc{ S(30), S(374), S(510), S(396) };
            DrawTextW(hdc, L"Closing this window keeps Clipper active in your system tray.", -1, &hintRc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            if (!feedbackText_.empty()) {
                SelectObject(hdc, fontBold_);
                SetTextColor(hdc, feedbackColor_);
                RECT fbRc{ S(18), S(416), S(522), S(444) };
                DrawTextW(hdc, feedbackText_.c_str(), -1, &fbRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            HPEN editPen = CreatePen(PS_SOLID, 1, kColInputBorder);
            oldPen = SelectObject(hdc, editPen);
            oldBrush = SelectObject(hdc, brushInput_);
            RoundRect(hdc, S(130), S(338), S(358), S(366), S(6), S(6));
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(editPen);

            SelectObject(hdc, oldFont);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, kColText);
            SetBkColor(hdc, kColInput);
            return reinterpret_cast<LRESULT>(brushInput_);
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, kColText);
            SetBkColor(hdc, kColInput);
            return reinterpret_cast<LRESULT>(brushInput_);
        }

        case WM_MEASUREITEM: {
            auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mis->CtlType == ODT_COMBOBOX) {
                mis->itemHeight = static_cast<UINT>(S(24));
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
                    case kIdToggleDesktopAudio:
                        DrawToggleItem(dis, desktopAudio_, L"Desktop Audio");
                        return TRUE;
                    case kIdToggleMicAudio:
                        DrawToggleItem(dis, micAudio_, L"Microphone");
                        return TRUE;
                    case kIdToggleChime:
                        DrawToggleItem(dis, saveChime_, L"Save Chime");
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
        SendMessageW(h, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(24));
        SendMessageW(h, CB_SETITEMHEIGHT, 0, S(24));
        SetWindowTheme(h, L"DarkMode_CFD", nullptr);
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

    btnSaveReplay_ = createBtn(kIdBtnSaveReplay, L"Save Replay", 408, 14, 116, 30);

    comboMonitor_ = createCombo(kIdComboMonitor, 130, 89, 310);
    btnRefresh_ = createBtn(kIdBtnRefreshMonitors, L"Refresh", 448, 89, 62, 28);

    comboFps_ = createCombo(kIdComboFps, 130, 127, 110);
    comboSeconds_ = createCombo(kIdComboSeconds, 334, 127, 176);
    comboBitrate_ = createCombo(kIdComboBitrate, 130, 165, 110);

    btnDesktopAudio_ = createBtn(kIdToggleDesktopAudio, L"", 30, 204, 152, 26, false);
    btnMicAudio_ = createBtn(kIdToggleMicAudio, L"", 198, 204, 148, 26, false);
    btnSaveChime_ = createBtn(kIdToggleChime, L"", 362, 204, 148, 26, false);

    btnHotkey_ = createBtn(kIdBtnHotkey, L"", 130, 296, 296, 30);
    SetWindowSubclass(btnHotkey_, &SettingsWindow::HotkeySubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
    btnResetHotkey_ = createBtn(kIdBtnResetHotkey, L"Reset", 434, 296, 76, 30);

    editOutputDir_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        S(136), S(343), S(216), S(18),
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditOutputDir)), nullptr, nullptr);
    SendMessageW(editOutputDir_, WM_SETFONT, reinterpret_cast<WPARAM>(fontRegular_), TRUE);
    SetWindowTheme(editOutputDir_, L"DarkMode_Explorer", nullptr);

    btnBrowse_ = createBtn(kIdBtnBrowseDir, L"Browse...", 364, 338, 78, 28);
    btnOpen_ = createBtn(kIdBtnOpenDir, L"Open", 448, 338, 62, 28);

    btnHide_ = createBtn(kIdBtnHideTray, L"Hide to Tray", 16, 458, 114, 34);
    btnQuit_ = createBtn(kIdBtnQuit, L"Quit", 138, 458, 80, 34);
    btnApply_ = createBtn(kIdBtnApply, L"Apply", 408, 458, 116, 34);
}

void SettingsWindow::PopulateMonitors(uint32_t selectedIndex) {
    SendMessageW(comboMonitor_, CB_RESETCONTENT, 0, 0);
    monitors_ = EnumerateCaptureMonitors();

    if (monitors_.empty()) {
        int idx = static_cast<int>(SendMessageW(comboMonitor_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"0: Primary Display (fallback)")));
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
    PopulateNumericCombo(comboFps_, { 30, 60 }, cfg.fps, L" fps");
    PopulateNumericCombo(comboSeconds_, { 15, 30, 45, 60, 90, 120, 180, 300 }, cfg.replaySeconds, L" s");
    PopulateNumericCombo(comboBitrate_, { 5, 10, 15, 20, 25, 30, 40, 50, 60, 80 }, cfg.bitrateMbps, L" Mbps");

    desktopAudio_ = cfg.recordDesktopAudio;
    micAudio_ = cfg.recordMicAudio;
    saveChime_ = cfg.saveChime;
    pendingHotkey_ = cfg.hotkeySave;

    std::wstring dirText = cfg.outputDirRaw.empty() ? cfg.outputDir : Utf8ToWide(cfg.outputDirRaw);
    SetWindowTextW(editOutputDir_, dirText.c_str());

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
    cfg.fps = SelectedComboData(comboFps_, cfg.fps);
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
    probe.fps = SelectedComboData(comboFps_, 60);

    bool overBudget = (static_cast<uint64_t>(probe.bitrateMbps) * probe.replaySeconds) > kMaxBitrateSecondsBudget;
    AppConfig clamped = probe;
    std::vector<std::string> warnings;
    ValidateAppConfig(clamped, warnings);
    uint64_t mb = (clamped.VideoArenaBytes() + clamped.AudioArenaBytes()) / (1024ULL * 1024ULL);

    wchar_t buf[96] = {};
    if (overBudget) {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
            L"Est. RAM: ~%llu MB (capped at %us)", static_cast<unsigned long long>(mb), clamped.replaySeconds);
        ramColor_ = kColAmber;
    } else {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
            L"Est. RAM buffer: ~%llu MB", static_cast<unsigned long long>(mb));
        ramColor_ = kColMuted;
    }
    ramText_ = buf;
    if (hwnd_) {
        RECT rc{ S(264), S(162), S(514), S(198) };
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

void SettingsWindow::DrawCard(HDC hdc, RECT rc) const {
    HPEN pen = CreatePen(PS_SOLID, 1, kColCardBorder);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brushCard_);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, S(10), S(10));
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void SettingsWindow::DrawButtonItem(const DRAWITEMSTRUCT* dis) const {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool isPrimary = (dis->CtlID == kIdBtnApply);
    bool isQuit = (dis->CtlID == kIdBtnQuit);
    bool isHotkey = (dis->CtlID == kIdBtnHotkey);
    bool isPressed = (dis->itemState & ODS_SELECTED) != 0;
    bool isHovered = (dis->hwndItem == hoveredBtn_);
    bool isFocus = (dis->itemState & ODS_FOCUS) != 0;

    COLORREF bg = kColInput;
    COLORREF border = kColInputBorder;
    COLORREF fg = kColText;

    if (isPrimary) {
        bg = isPressed ? kColAccentPressed : (isHovered ? kColAccentHover : kColAccent);
        border = bg;
    } else if (isHotkey && recordingHotkey_) {
        bg = RGB(36, 48, 72);
        border = kColAccent;
        fg = kColText;
    } else if (isQuit && isHovered) {
        bg = RGB(74, 34, 38);
        border = kColRed;
        fg = RGB(254, 202, 202);
    } else {
        bg = isPressed ? kColPressed : (isHovered ? kColHover : kColInput);
        if (isFocus) border = kColAccent;
    }

    FillRect(hdc, &rc, (dis->CtlID == kIdBtnSaveReplay || dis->CtlID == kIdBtnHideTray ||
                       dis->CtlID == kIdBtnQuit || dis->CtlID == kIdBtnApply) ? brushBg_ : brushCard_);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH br = CreateSolidBrush(bg);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, S(8), S(8));
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);

    std::wstring text;
    if (isHotkey) {
        if (recordingHotkey_) {
            text = L"Press key combination... (Esc cancels)";
        } else {
            text = Utf8ToWide(pendingHotkey_.display) + L"  (Click to change)";
        }
    } else {
        wchar_t buf[128] = {};
        GetWindowTextW(dis->hwndItem, buf, 128);
        text = buf;
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    HGDIOBJ oldFont = SelectObject(hdc, isPrimary ? fontBold_ : fontRegular_);
    DrawTextW(hdc, text.c_str(), -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
}

void SettingsWindow::DrawToggleItem(const DRAWITEMSTRUCT* dis, bool checked, const wchar_t* label) const {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    FillRect(hdc, &rc, brushCard_);

    int trackW = S(34);
    int trackH = S(18);
    int trackX = rc.left;
    int trackY = rc.top + ((rc.bottom - rc.top) - trackH) / 2;

    COLORREF trackCol = checked ? kColAccent : kColTrackOff;
    HBRUSH trackBr = CreateSolidBrush(trackCol);
    HPEN trackPen = CreatePen(PS_SOLID, 1, trackCol);
    HGDIOBJ oldPen = SelectObject(hdc, trackPen);
    HGDIOBJ oldBr = SelectObject(hdc, trackBr);
    RoundRect(hdc, trackX, trackY, trackX + trackW, trackY + trackH, trackH, trackH);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(trackBr);
    DeleteObject(trackPen);

    int pad = S(3);
    int thumbSz = trackH - 2 * pad;
    int thumbX = checked ? (trackX + trackW - pad - thumbSz) : (trackX + pad);
    int thumbY = trackY + pad;

    HBRUSH thumbBr = CreateSolidBrush(kColText);
    oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    oldBr = SelectObject(hdc, thumbBr);
    Ellipse(hdc, thumbX, thumbY, thumbX + thumbSz, thumbY + thumbSz);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(thumbBr);

    RECT textRc{ trackX + trackW + S(8), rc.top, rc.right, rc.bottom };
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, checked ? kColText : kColMuted);
    HGDIOBJ oldFont = SelectObject(hdc, fontRegular_);
    DrawTextW(hdc, label, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

void SettingsWindow::DrawComboItem(const DRAWITEMSTRUCT* dis) const {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool isSelected = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF bg = isSelected ? RGB(54, 72, 108) : kColInput;

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
    textRc.left += S(6);
    textRc.right -= S(4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kColText);
    HGDIOBJ oldFont = SelectObject(hdc, fontRegular_);
    DrawTextW(hdc, text.c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
    del(reinterpret_cast<HGDIOBJ&>(fontSmall_));
    del(reinterpret_cast<HGDIOBJ&>(brushBg_));
    del(reinterpret_cast<HGDIOBJ&>(brushCard_));
    del(reinterpret_cast<HGDIOBJ&>(brushInput_));
}

}
