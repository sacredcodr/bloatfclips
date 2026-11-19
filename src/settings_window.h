#pragma once

#include "clip_library.h"
#include "config.h"
#include "recorder_engine.h"
#include "video_pipeline.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace clipper {

enum class SettingsTab : int {
    Clips = 0,
    Settings = 1
};

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
    static constexpr UINT kWmSelectTab = WM_APP + 50;

    explicit SettingsWindow(SettingsCallbacks callbacks);
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    void ShowOrFocus(HINSTANCE hInst, HICON icon, SettingsTab initialTab = SettingsTab::Clips);
    void SetActiveTab(SettingsTab tab);
    void NotifyClipsChanged();
    void CloseIfOpen();
    bool IsOpen() const { return hwnd_ != nullptr; }
    void RefreshStatus();
    void ShowFeedback(const std::wstring& text, bool isError);

private:
    enum class InputMode {
        None = 0,
        EditFps,
        EditReplay,
        EditBitrate,
        CaptureHotkey
    };

    enum class HitAction {
        None = 0,
        WinMinimize,
        WinClose,
        TitleSaveClip,
        SelectTab,
        RailHide,
        RailQuit,
        ClipsRefresh,
        ClipsOpenFolder,
        ClipPlay,
        ClipShowInFolder,
        ClipDelete,
        EmptyGoSettings,
        RefreshMonitors,
        SelectMonitor,
        SelectFpsPreset,
        EditFpsBox,
        SelectReplayPreset,
        EditReplayBox,
        ToggleReplayUnit,
        BitrateSlider,
        EditBitrateBox,
        ToggleDesktopAudio,
        ToggleMicAudio,
        ToggleChime,
        ToggleStartWithWindows,
        HotkeyCaptureBtn,
        HotkeyResetBtn,
        BrowseFolder,
        OpenFolderBtn,
        ApplyChanges,
        DiscardChanges,
    };

    struct HitKey {
        HitAction action = HitAction::None;
        int param = 0;

        bool operator==(const HitKey&) const = default;
    };

    struct HitTarget {
        D2D1_RECT_F rect{};
        HitKey key{};
        bool inViewport = false;
    };

    struct ThumbEntry {
        bool attempted = false;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
    };

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    bool EnsureDeviceIndependentResources();
    bool EnsureDeviceResources();
    bool EnsureBackbuffer(int w, int h);
    void DestroyBackbuffer();
    void DiscardDeviceResources();
    void DestroyAllResources();

    void ReloadConfigState();
    void ReloadMonitors();
    void ReloadClips();
    void StepThumbnailBatch(size_t maxCount, bool invalidateOnLoad);
    void InvalidateWindow();

    void RenderToDC(HDC targetHdc);
    void DrawMicaBackdrop();
    void DrawTitleBar();
    void DrawNavRail();
    void DrawPageHeader();
    void DrawClipsTab(float contentX, float contentW, float& yCursor);
    void DrawSettingsTab(float contentX, float contentW, float& yCursor);
    void DrawBottomBar();

    void DrawTextLine(std::wstring_view text, IDWriteTextFormat* fmt,
                      D2D1_RECT_F rect, D2D1_COLOR_F color,
                      DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void DrawGlyph(const wchar_t* glyph, float fontSize, D2D1_RECT_F rect, D2D1_COLOR_F color);
    void DrawFluentSurface(D2D1_RECT_F rect, float radius, bool hovered, bool active = false);
    void DrawFluentButton(D2D1_RECT_F rect, const wchar_t* label, const wchar_t* glyph,
                          HitKey key, bool primary, bool subtleDisabled = false, bool inViewport = false);
    void DrawEditBox(D2D1_RECT_F rect, std::wstring_view displayValue, InputMode mode,
                     HitKey key, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER);
    void DrawToggleSwitch(D2D1_RECT_F rect, bool on, bool hovered);
    void DrawKeycapGroupRight(float rightX, float centerY, const std::string& display, float minLeft);

    const HitTarget* HitTestDip(float dipX, float dipY) const;
    void ExecuteHit(const HitKey& key, float dipX);
    void EnterInputMode(InputMode nextMode);
    void CommitOrCancelInput(bool commit, bool showCancelFeedback = false);
    bool HandleInputChar(wchar_t ch);
    bool HandleInputKeyDown(UINT vk, LPARAM lParam);
    void UpdateBitrateFromDip(float dipX);
    void BrowseForOutputFolder();
    void OpenFolderInExplorer(const std::wstring& dir);
    void RevealFileInExplorer(const std::wstring& fullPath);
    void DeleteClipToRecycleBin(size_t clipIndex);
    bool IsDirty() const;
    void ComputeRamEstimate(double& outMb, bool& outOverCap, uint32_t& outClampedSecs) const;

    SettingsCallbacks callbacks_;
    HWND hwnd_ = nullptr;
    int dpi_ = 96;
    SettingsTab activeTab_ = SettingsTab::Clips;
    InputMode inputMode_ = InputMode::None;
    std::wstring editBuffer_;
    bool replaceOnFirstChar_ = false;
    bool replayUnitMinutes_ = true;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBrand_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtPageTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtRowTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBody_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBodySemi_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtSmall_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtMono_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtKeycap_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtGlyph_;
    std::wstring glyphFontName_ = L"Segoe Fluent Icons";

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    HDC memDC_ = nullptr;
    HBITMAP memBmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int memW_ = 0;
    int memH_ = 0;

    AppConfig appliedCfg_{};
    AppConfig editedCfg_{};
    SettingsStatusInfo status_{};
    std::vector<MonitorInfo> monitors_;
    std::vector<ClipEntry> clips_;
    uint64_t totalClipsBytes_ = 0;
    std::unordered_map<std::wstring, ThumbEntry> thumbCache_;

    std::vector<HitTarget> hitTargets_;
    HitKey hoveredHit_{};
    bool trackingMouse_ = false;
    bool draggingSlider_ = false;
    D2D1_RECT_F sliderTrackRect_{};
    float scrollY_ = 0.0f;
    float maxScroll_ = 0.0f;

    std::wstring feedbackText_;
    bool feedbackIsError_ = false;
    uint64_t feedbackExpiresMs_ = 0;
};

}
