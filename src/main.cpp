#include "config.h"
#include "recorder_engine.h"
#include "settings_window.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\ClipperSingleInstanceMutex";
constexpr wchar_t kWndClass[] = L"ClipperTrayHiddenWindow";
constexpr UINT kTrayIconId = 1;
constexpr int kHotkeySaveId = 1;

constexpr UINT kWmTray = WM_APP + 1;
constexpr UINT kWmSaveDone = WM_APP + 2;

constexpr UINT kCmdOpenSettings = 1000;
constexpr UINT kCmdSave = 1001;
constexpr UINT kCmdToggleMic = 1002;
constexpr UINT kCmdOpenDir = 1003;
constexpr UINT kCmdExit = 1004;

struct AppState {
    HINSTANCE hInst = nullptr;
    clipper::AppConfig config;
    std::unique_ptr<clipper::RecorderEngine> engine;
    std::unique_ptr<clipper::SettingsWindow> settingsWin;
    std::string lastStartError;
    HWND hwnd = nullptr;
    HICON trayIcon = nullptr;
    bool ownsTrayIcon = false;
    UINT taskbarCreatedMsg = 0;
    UINT showSettingsMsg = 0;
    bool trayAdded = false;
    bool hotkeyRegistered = false;
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

void UpdateTrayTooltip(const AppState& app) {
    if (!app.trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;
    std::wstring tip = std::wstring(clipper::kAppName) + L" (" + clipper::Utf8ToWide(app.config.hotkeySave.display) + L")";
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AddTrayIcon(AppState& app) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kWmTray;
    nid.hIcon = app.trayIcon;
    std::wstring tip = std::wstring(clipper::kAppName) + L" (" + clipper::Utf8ToWide(app.config.hotkeySave.display) + L")";
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

bool RegisterSaveHotkey(AppState& app) {
    if (app.hotkeyRegistered) {
        UnregisterHotKey(app.hwnd, kHotkeySaveId);
        app.hotkeyRegistered = false;
    }
    app.hotkeyRegistered = RegisterHotKey(
        app.hwnd, kHotkeySaveId,
        app.config.hotkeySave.modifiers | MOD_NOREPEAT,
        app.config.hotkeySave.vk) != FALSE;
    return app.hotkeyRegistered;
}

void WireSaveCallback(AppState& app) {
    HWND targetHwnd = app.hwnd;
    app.engine->SetSaveCallback([targetHwnd](const clipper::SaveResult& r) {
        auto* copy = new clipper::SaveResult(r);
        if (!PostMessageW(targetHwnd, kWmSaveDone, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });
}

bool ApplyNewConfig(AppState& app, clipper::AppConfig nextCfg, std::wstring& feedbackOut, bool& isErrOut) {
    std::vector<std::string> warnings;
    clipper::ValidateAppConfig(nextCfg, warnings);

    bool needsRestart = !app.engine ||
        nextCfg.monitorIndex != app.config.monitorIndex ||
        nextCfg.fps != app.config.fps ||
        nextCfg.bitrateMbps != app.config.bitrateMbps ||
        nextCfg.replaySeconds != app.config.replaySeconds ||
        nextCfg.recordDesktopAudio != app.config.recordDesktopAudio ||
        nextCfg.recordMicAudio != app.config.recordMicAudio ||
        app.engine->State() == clipper::EngineState::Faulted ||
        app.engine->State() == clipper::EngineState::Stopped;

    app.config = nextCfg;
    bool hkOk = RegisterSaveHotkey(app);
    UpdateTrayTooltip(app);

    bool startOk = true;
    if (needsRestart) {
        if (app.engine) app.engine->Stop();
        app.engine = std::make_unique<clipper::RecorderEngine>(app.config);
        WireSaveCallback(app);
        std::string startErr;
        startOk = app.engine->Start(startErr);
        app.lastStartError = startOk ? std::string() : startErr;
    } else {
        app.engine->UpdateSaveSettings(app.config.outputDir, app.config.saveChime);
    }

    bool savedIni = (!needsRestart || startOk)
        ? clipper::SaveConfig(clipper::DefaultConfigPath(), app.config)
        : false;

    std::vector<std::wstring> errs;
    if (!startOk) {
        errs.push_back(L"Capture failed: " + clipper::Utf8ToWide(app.lastStartError));
    }
    if (!hkOk) {
        errs.push_back(L"Hotkey " + clipper::Utf8ToWide(app.config.hotkeySave.display) + L" collided with another app");
    }
    if ((!needsRestart || startOk) && !savedIni) {
        errs.push_back(L"Failed to write clipper.ini");
    }

    if (!errs.empty()) {
        isErrOut = true;
        feedbackOut.clear();
        for (size_t i = 0; i < errs.size(); ++i) {
            if (i > 0) feedbackOut += L" | ";
            feedbackOut += errs[i];
        }
        return false;
    }

    isErrOut = false;
    feedbackOut = needsRestart
        ? L"Settings saved and capture restarted."
        : L"Settings saved.";
    if (!warnings.empty()) {
        feedbackOut += L" (" + clipper::Utf8ToWide(warnings.front()) + L")";
    }
    return true;
}

void OpenSettings(AppState& app) {
    if (!app.settingsWin) return;
    app.settingsWin->ShowOrFocus(app.hInst, app.trayIcon);
}

void ShowContextMenu(AppState& app) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, kCmdOpenSettings, L"Open Settings");
    SetMenuDefaultItem(menu, kCmdOpenSettings, FALSE);

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
    if (app && app->showSettingsMsg != 0 && msg == app->showSettingsMsg) {
        OpenSettings(*app);
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
            } else if (evt == WM_LBUTTONUP || evt == WM_LBUTTONDBLCLK || evt == NIN_SELECT || evt == NIN_KEYSELECT) {
                if (app) OpenSettings(*app);
            }
            return 0;
        }

        case kWmSaveDone: {
            auto* res = reinterpret_cast<clipper::SaveResult*>(lParam);
            if (res && app) {
                if (res->ok) {
                    if (app->settingsWin && app->settingsWin->IsOpen()) {
                        std::wstring fileName = std::filesystem::path(res->path).filename().wstring();
                        wchar_t msgBuf[256] = {};
                        std::swprintf(msgBuf, sizeof(msgBuf) / sizeof(wchar_t),
                            L"Saved %ls (%.1f ms)", fileName.c_str(), res->saveMs);
                        app->settingsWin->NotifyClipsChanged();
                        app->settingsWin->ShowFeedback(msgBuf, false);
                    }
                } else {
                    std::wstring msgW = L"Failed to save clip: " + clipper::Utf8ToWide(res->error);
                    if (app->settingsWin && app->settingsWin->IsOpen()) {
                        app->settingsWin->ShowFeedback(msgW, true);
                    } else {
                        ShowTrayBalloon(*app, clipper::kAppName, msgW.c_str(), NIIF_ERROR);
                    }
                }
            }
            delete res;
            return 0;
        }

        case WM_COMMAND:
            if (!app) break;
            switch (LOWORD(wParam)) {
                case kCmdOpenSettings:
                    OpenSettings(*app);
                    return 0;
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
                if (app->settingsWin) app->settingsWin->CloseIfOpen();
                if (app->hotkeyRegistered) {
                    UnregisterHotKey(hwnd, kHotkeySaveId);
                    app->hotkeyRegistered = false;
                }
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
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    bool failFast = false;
    bool startInTray = false;
    clipper::SettingsTab initialTab = clipper::SettingsTab::Clips;
    std::wstring readyEventName;
    for (int i = 1; i < __argc; ++i) {
        std::wstring arg = __wargv[i];
        if (arg == L"--fail-fast") {
            failFast = true;
        } else if (arg == L"--tray" || arg == L"--minimized") {
            startInTray = true;
        } else if (arg.rfind(L"--ready-event=", 0) == 0) {
            readyEventName = arg.substr(14);
        } else if (arg.rfind(L"--page=", 0) == 0) {
            std::wstring p = arg.substr(7);
            if (_wcsicmp(p.c_str(), L"settings") == 0 ||
                _wcsicmp(p.c_str(), L"capture") == 0 ||
                _wcsicmp(p.c_str(), L"audio") == 0 ||
                _wcsicmp(p.c_str(), L"hotkeys") == 0) {
                initialTab = clipper::SettingsTab::Settings;
            } else {
                initialTab = clipper::SettingsTab::Clips;
            }
        }
    }

    UINT showSettingsMsg = RegisterWindowMessageW(L"ClipperShowSettingsWindow");
    HANDLE singleMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (singleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startInTray && !failFast && showSettingsMsg != 0) {
            if (HWND existing = FindWindowW(kWndClass, clipper::kAppName)) {
                PostMessageW(existing, showSettingsMsg, 0, 0);
            }
        }
        CloseHandle(singleMutex);
        if (SUCCEEDED(comHr)) CoUninitialize();
        return 0;
    }

    auto iniParsed = clipper::LoadOrCreateConfig(clipper::DefaultConfigPath());
    auto cliParsed = clipper::ApplyCliOverrides(__argc, __wargv, std::move(iniParsed.config));
    std::vector<std::string> configWarnings = std::move(iniParsed.warnings);
    configWarnings.insert(configWarnings.end(), cliParsed.warnings.begin(), cliParsed.warnings.end());

    AppState app;
    app.hInst = hInst;
    app.config = cliParsed.config;
    app.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    app.showSettingsMsg = showSettingsMsg;
    app.trayIcon = CreateRecorderTrayIcon();
    app.ownsTrayIcon = (app.trayIcon != nullptr);
    if (!app.trayIcon) app.trayIcon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    app.hwnd = CreateWindowExW(0, kWndClass, clipper::kAppName, WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    SetWindowLongPtrW(app.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    AddTrayIcon(app);

    clipper::SettingsCallbacks cbs;
    cbs.getConfig = [&app]() {
        return app.config;
    };
    cbs.getStatus = [&app]() {
        clipper::SettingsStatusInfo st;
        if (app.engine) {
            st.state = app.engine->State();
            st.video = app.engine->GetVideoStats();
        }
        st.lastError = app.lastStartError;
        return st;
    };
    cbs.onApply = [&app](const clipper::AppConfig& nextCfg, std::wstring& fb, bool& isErr) {
        return ApplyNewConfig(app, nextCfg, fb, isErr);
    };
    cbs.onSaveReplay = [&app]() {
        if (app.engine) app.engine->RequestSaveClip();
    };
    cbs.onQuit = [&app]() {
        if (app.hwnd) PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    };
    cbs.onSuspendHotkey = [&app](bool suspend) {
        if (suspend) {
            if (app.hotkeyRegistered) {
                UnregisterHotKey(app.hwnd, kHotkeySaveId);
                app.hotkeyRegistered = false;
            }
        } else {
            RegisterSaveHotkey(app);
        }
    };
    app.settingsWin = std::make_unique<clipper::SettingsWindow>(std::move(cbs));

    app.engine = std::make_unique<clipper::RecorderEngine>(app.config);
    WireSaveCallback(app);

    bool hkOk = RegisterSaveHotkey(app);
    if (!hkOk && !failFast) {
        std::wstring warn = L"Could not register hotkey " + clipper::Utf8ToWide(app.config.hotkeySave.display) +
                            L" (collision with another app). Change it in " + std::wstring(clipper::kAppName) + L" Settings.";
        ShowTrayBalloon(app, clipper::kAppName, warn.c_str(), NIIF_WARNING);
    }

    int exitCode = 0;
    std::string startErr;
    bool started = app.engine->Start(startErr);
    if (!started) {
        app.lastStartError = startErr;
        if (failFast) {
            exitCode = 2;
            DestroyWindow(app.hwnd);
        } else {
            std::wstring errW = L"Capture start failed: " + clipper::Utf8ToWide(startErr);
            ShowTrayBalloon(app, clipper::kAppName, errW.c_str(), NIIF_ERROR);
        }
    } else if (!readyEventName.empty()) {
        if (HANDLE readyEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName.c_str())) {
            SetEvent(readyEvt);
            CloseHandle(readyEvt);
        }
    }

    if (exitCode == 0) {
        if (!startInTray && !failFast) {
            if (app.settingsWin) app.settingsWin->ShowOrFocus(app.hInst, app.trayIcon, initialTab);
            if (!started && app.settingsWin) {
                app.settingsWin->ShowFeedback(L"Capture failed: " + clipper::Utf8ToWide(startErr), true);
            } else if (!hkOk && app.settingsWin) {
                app.settingsWin->ShowFeedback(
                    L"Hotkey " + clipper::Utf8ToWide(app.config.hotkeySave.display) + L" collided with another app.", true);
            } else if (!configWarnings.empty() && app.settingsWin) {
                app.settingsWin->ShowFeedback(clipper::Utf8ToWide(configWarnings.front()), true);
            }
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (app.settingsWin) {
        app.settingsWin->CloseIfOpen();
        app.settingsWin.reset();
    }
    if (app.engine) {
        app.engine->Stop();
        app.engine.reset();
    }
    if (app.trayIcon && app.ownsTrayIcon) DestroyIcon(app.trayIcon);
    if (singleMutex) CloseHandle(singleMutex);
    if (SUCCEEDED(comHr)) CoUninitialize();
    return exitCode;
}
