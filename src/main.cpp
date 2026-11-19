#include "config.h"
#include "recorder_engine.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\ClipperSingleInstanceMutex";
constexpr wchar_t kWndClass[] = L"ClipperTrayHiddenWindow";
constexpr UINT kTrayIconId = 1;
constexpr int kHotkeySaveId = 1;

constexpr UINT kWmTray = WM_APP + 1;
constexpr UINT kWmSaveDone = WM_APP + 2;

constexpr UINT kCmdSave = 1001;
constexpr UINT kCmdToggleMic = 1002;
constexpr UINT kCmdOpenDir = 1003;
constexpr UINT kCmdExit = 1004;

struct AppState {
    clipper::AppConfig config;
    clipper::RecorderEngine* engine = nullptr;
    HWND hwnd = nullptr;
    HICON trayIcon = nullptr;
    UINT taskbarCreatedMsg = 0;
    bool trayAdded = false;
};

HICON CreateRecorderTrayIcon() {
    constexpr int kSize = 16;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = kSize;
    bi.bV5Height = -kSize;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    uint32_t* pixels = nullptr;
    HBITMAP colorBmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
        reinterpret_cast<void**>(&pixels), nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!colorBmp || !pixels) return nullptr;

    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            double dx = (x + 0.5) - kSize * 0.5;
            double dy = (y + 0.5) - kSize * 0.5;
            double r = dx * dx + dy * dy;
            uint32_t col = 0x00000000;
            if (r <= 56.0) {
                if (r <= 14.0) col = 0xFFE53935;
                else if (r <= 36.0) col = 0xFF20242A;
                else col = 0xFFD0D6DC;
            }
            pixels[y * kSize + x] = col;
        }
    }

    HBITMAP maskBmp = CreateBitmap(kSize, kSize, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(maskBmp);
    DeleteObject(colorBmp);
    return icon;
}

void AddTrayIcon(AppState& app) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kWmTray;
    nid.hIcon = app.trayIcon;
    std::wstring tip = L"Clipper (" + clipper::Utf8ToWide(app.config.hotkeySave.display) + L")";
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (app.trayAdded) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void RemoveTrayIcon(AppState& app) {
    if (!app.trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    app.trayAdded = false;
}

void ShowTrayBalloon(const AppState& app, const wchar_t* title, const wchar_t* text, DWORD infoFlags) {
    if (!app.trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = infoFlags;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowContextMenu(AppState& app) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    std::wstring saveLabel = L"Save Replay (" + clipper::Utf8ToWide(app.config.hotkeySave.display) + L")";
    AppendMenuW(menu, MF_STRING, kCmdSave, saveLabel.c_str());

    UINT micFlags = MF_STRING;
    if (!app.config.recordMicAudio) {
        micFlags |= MF_GRAYED;
    } else if (app.engine && app.engine->IsMicEnabled()) {
        micFlags |= MF_CHECKED;
    }
    AppendMenuW(menu, micFlags, kCmdToggleMic, L"Microphone");
    AppendMenuW(menu, MF_STRING, kCmdOpenDir, L"Open Clips Folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(app.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, app.hwnd, nullptr);
    PostMessageW(app.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (app && app->taskbarCreatedMsg != 0 && msg == app->taskbarCreatedMsg) {
        AddTrayIcon(*app);
        return 0;
    }

    switch (msg) {
        case WM_HOTKEY:
            if (app && wParam == kHotkeySaveId && app->engine) {
                app->engine->RequestSaveClip();
            }
            return 0;

        case kWmTray: {
            UINT evt = LOWORD(lParam);
            if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
                if (app) ShowContextMenu(*app);
            } else if (evt == WM_LBUTTONDBLCLK) {
                if (app && app->engine) app->engine->RequestSaveClip();
            }
            return 0;
        }

        case kWmSaveDone: {
            auto* res = reinterpret_cast<clipper::SaveResult*>(lParam);
            if (res) {
                if (!res->ok && app) {
                    std::wstring msgW = L"Failed to save clip: " + clipper::Utf8ToWide(res->error);
                    ShowTrayBalloon(*app, L"Clipper", msgW.c_str(), NIIF_ERROR);
                }
                delete res;
            }
            return 0;
        }

        case WM_COMMAND:
            if (!app) break;
            switch (LOWORD(wParam)) {
                case kCmdSave:
                    if (app->engine) app->engine->RequestSaveClip();
                    return 0;
                case kCmdToggleMic:
                    if (app->engine && app->config.recordMicAudio) {
                        app->engine->SetMicEnabled(!app->engine->IsMicEnabled());
                    }
                    return 0;
                case kCmdOpenDir: {
                    std::error_code ec;
                    std::filesystem::create_directories(app->config.outputDir, ec);
                    ShellExecuteW(nullptr, L"open", app->config.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                case kCmdExit:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (app) {
                UnregisterHotKey(hwnd, kHotkeySaveId);
                RemoveTrayIcon(*app);
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE singleMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (singleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleMutex);
        return 0;
    }

    auto parsed = clipper::LoadOrCreateConfig(clipper::DefaultConfigPath());
    parsed = clipper::ApplyCliOverrides(__argc, __wargv, std::move(parsed.config));

    AppState app;
    app.config = parsed.config;
    app.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    app.trayIcon = CreateRecorderTrayIcon();
    if (!app.trayIcon) app.trayIcon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    app.hwnd = CreateWindowExW(0, kWndClass, L"Clipper", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    SetWindowLongPtrW(app.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    AddTrayIcon(app);

    clipper::RecorderEngine engine(app.config);
    app.engine = &engine;
    HWND targetHwnd = app.hwnd;
    engine.SetSaveCallback([targetHwnd](const clipper::SaveResult& r) {
        auto* copy = new clipper::SaveResult(r);
        if (!PostMessageW(targetHwnd, kWmSaveDone, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });

    if (!RegisterHotKey(app.hwnd, kHotkeySaveId, app.config.hotkeySave.modifiers | MOD_NOREPEAT, app.config.hotkeySave.vk)) {
        std::wstring warn = L"Could not register hotkey " + clipper::Utf8ToWide(app.config.hotkeySave.display) +
                            L" (collision with another app). Use the tray menu or change hotkey_save in clipper.ini.";
        ShowTrayBalloon(app, L"Clipper", warn.c_str(), NIIF_WARNING);
    }

    bool failFast = false;
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--fail-fast") == 0) failFast = true;
    }

    std::string startErr;
    if (!engine.Start(startErr)) {
        if (failFast) {
            DestroyWindow(app.hwnd);
            if (app.trayIcon) DestroyIcon(app.trayIcon);
            if (singleMutex) CloseHandle(singleMutex);
            return 2;
        }
        std::wstring errW = L"Capture start failed: " + clipper::Utf8ToWide(startErr);
        ShowTrayBalloon(app, L"Clipper", errW.c_str(), NIIF_ERROR);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    engine.Stop();
    if (app.trayIcon) DestroyIcon(app.trayIcon);
    if (singleMutex) CloseHandle(singleMutex);
    return 0;
}
