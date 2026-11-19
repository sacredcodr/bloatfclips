#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace clipper {

inline constexpr uint32_t kMaxBitrateSecondsBudget = 12000;

struct HotkeySpec {
    uint32_t modifiers = 0x0002 | 0x0001;
    uint32_t vk = 'S';
    std::string display = "Ctrl+Alt+S";

    bool operator==(const HotkeySpec&) const = default;
};

struct AppConfig {
    uint32_t monitorIndex = 0;
    uint32_t fps = 60;
    uint32_t bitrateMbps = 20;
    uint32_t replaySeconds = 60;
    bool recordDesktopAudio = true;
    bool recordMicAudio = true;
    HotkeySpec hotkeySave{};
    std::string outputDirRaw = "%USERPROFILE%\\Videos\\Clips";
    std::wstring outputDir;
    bool saveChime = true;

    size_t VideoArenaBytes() const;
    size_t VideoMaxSlots() const;
    size_t AudioArenaBytes() const;
    size_t AudioMaxSlots() const;
};

struct ConfigParseResult {
    AppConfig config;
    std::vector<std::string> warnings;
};

AppConfig MakeDefaultConfig();
bool ParseHotkeyString(std::string_view text, HotkeySpec& out);
bool IsSafeHotkey(uint32_t modifiers, uint32_t vk);
bool FormatHotkeyFromVk(uint32_t modifiers, uint32_t vk, HotkeySpec& out);
void ValidateAppConfig(AppConfig& cfg, std::vector<std::string>& warnings);
std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);
std::wstring ExpandEnvPath(std::string_view utf8Path);
ConfigParseResult ParseConfigText(std::string_view text, AppConfig base = MakeDefaultConfig());
std::string SerializeConfig(const AppConfig& cfg);
ConfigParseResult LoadOrCreateConfig(const std::wstring& iniPath);
bool SaveConfig(const std::wstring& iniPath, const AppConfig& cfg);
std::wstring DefaultConfigPath();
ConfigParseResult ApplyCliOverrides(int argc, wchar_t** argv, AppConfig base);

}
