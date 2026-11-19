#pragma once

#include "config.h"
#include "recorder_engine.h"
#include "video_pipeline.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace clipper {

struct SettingsStatusInfo {
    EngineState state = EngineState::Stopped;
    VideoStats video{};
    std::string lastError;
};

struct SettingsCallbacks {
    std::function<AppConfig()> getConfig;
    std::function<SettingsStatusInfo()> getStatus;
    std::function<bool(const AppConfig&, std::wstring& feedbackOut, bool& isErrOut)> onApply;
    std::function<void()> onSaveReplay;
    std::function<void()> onQuit;
    std::function<void(bool suspend)> onSuspendHotkey;
};

class SettingsWindow {
public:
    explicit SettingsWindow(SettingsCallbacks callbacks);
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    void ShowOrFocus(HINSTANCE hInst, HICON icon);
    void CloseIfOpen();
    bool IsOpen() const { return hwnd_ != nullptr; }
    void RefreshStatus();
    void ShowFeedback(const std::wstring& text, bool isError);
    HWND Handle() const { return hwnd_; }

private:
    enum ControlId : int {
        kIdBtnSaveReplay = 2001,
        kIdComboMonitor = 2002,
        kIdBtnRefreshMonitors = 2003,
        kIdSegFps = 2004,
        kIdComboSeconds = 2005,
        kIdComboBitrate = 2006,
        kIdToggleDesktopAudio = 2007,
        kIdToggleMicAudio = 2008,
        kIdToggleChime = 2009,
        kIdBtnHotkey = 2010,
        kIdBtnResetHotkey = 2011,
        kIdEditOutputDir = 2012,
        kIdBtnBrowseDir = 2013,
        kIdBtnOpenDir = 2014,
        kIdBtnHideTray = 2015,
        kIdBtnQuit = 2016,
        kIdBtnApply = 2017,
    };

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK HotkeySubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR subId, DWORD_PTR refData);
    static LRESULT CALLBACK ButtonHoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR subId, DWORD_PTR refData);
    static LRESULT CALLBACK ComboBorderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR subId, DWORD_PTR refData);
    static LRESULT CALLBACK SegFpsKeyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR subId, DWORD_PTR refData);

    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);
    int S(int logicalPx) const;

    void CreateControls();
    void PopulateFromConfig(const AppConfig& cfg);
    void PopulateMonitors(uint32_t selectedIndex);
    void PopulateNumericCombo(HWND combo, const std::vector<uint32_t>& presets,
                              uint32_t currentVal, const wchar_t* suffix);
    uint32_t SelectedComboData(HWND combo, uint32_t fallback) const;
    std::wstring ReadExpandedEditFolder() const;
    AppConfig GatherEditedConfig() const;

    void UpdateRamEstimate();
    void BrowseForOutputFolder();
    void BeginHotkeyCapture();
    void EndHotkeyCapture(bool cancelMsg);

    void DrawRowContainer(HDC hdc, RECT rc, const std::vector<int>& dividerY) const;
    void DrawButtonItem(const DRAWITEMSTRUCT* dis) const;
    void DrawSegFpsItem(const DRAWITEMSTRUCT* dis) const;
    void DrawHotkeyItem(const DRAWITEMSTRUCT* dis) const;
    void DrawToggleItem(const DRAWITEMSTRUCT* dis, bool checked) const;
    void DrawComboItem(const DRAWITEMSTRUCT* dis) const;
    void DestroyGdiResources();

    SettingsCallbacks callbacks_;
    HWND hwnd_ = nullptr;
    int dpi_ = 96;

    HFONT fontRegular_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontSection_ = nullptr;
    HFONT fontDesc_ = nullptr;
    HFONT fontKeycap_ = nullptr;
    HBRUSH brushBg_ = nullptr;
    HBRUSH brushCard_ = nullptr;
    HBRUSH brushInput_ = nullptr;

    HWND comboMonitor_ = nullptr;
    HWND btnRefresh_ = nullptr;
    HWND btnSegFps_ = nullptr;
    HWND comboSeconds_ = nullptr;
    HWND comboBitrate_ = nullptr;
    HWND btnDesktopAudio_ = nullptr;
    HWND btnMicAudio_ = nullptr;
    HWND btnSaveChime_ = nullptr;
    HWND btnHotkey_ = nullptr;
    HWND btnResetHotkey_ = nullptr;
    HWND editOutputDir_ = nullptr;
    HWND btnBrowse_ = nullptr;
    HWND btnOpen_ = nullptr;
    HWND btnSaveReplay_ = nullptr;
    HWND btnHide_ = nullptr;
    HWND btnQuit_ = nullptr;
    HWND btnApply_ = nullptr;

    std::vector<MonitorInfo> monitors_;
    HotkeySpec pendingHotkey_{};
    uint32_t selectedFps_ = 60;
    bool desktopAudio_ = true;
    bool micAudio_ = true;
    bool saveChime_ = true;
    bool recordingHotkey_ = false;
    HWND hoveredBtn_ = nullptr;
    int hoveredSeg_ = -1;

    std::wstring statusText_ = L"Starting...";
    COLORREF statusColor_ = RGB(245, 158, 11);
    std::wstring ramText_;
    COLORREF ramColor_ = RGB(138, 143, 152);
    std::wstring feedbackText_;
    COLORREF feedbackColor_ = RGB(138, 143, 152);
};

}
