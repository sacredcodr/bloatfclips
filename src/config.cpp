#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace clipper {

namespace {

constexpr uint32_t kMinFps = 30;
constexpr uint32_t kMaxFps = 60;
constexpr uint32_t kMinBitrateMbps = 2;
constexpr uint32_t kMaxBitrateMbps = 80;
constexpr uint32_t kMinReplaySeconds = 5;
constexpr uint32_t kMaxReplaySeconds = 300;

struct NamedKeyDef {
    const char* tokenLower;
    uint32_t vk;
    const char* canonical;
    bool requiresModifier;
};

constexpr NamedKeyDef kNamedKeys[] = {
    {"insert", VK_INSERT, "Insert", false},
    {"ins", VK_INSERT, "Insert", false},
    {"delete", VK_DELETE, "Delete", false},
    {"del", VK_DELETE, "Delete", false},
    {"home", VK_HOME, "Home", false},
    {"end", VK_END, "End", false},
    {"pageup", VK_PRIOR, "PageUp", false},
    {"pgup", VK_PRIOR, "PageUp", false},
    {"pagedown", VK_NEXT, "PageDown", false},
    {"pgdn", VK_NEXT, "PageDown", false},
    {"printscreen", VK_SNAPSHOT, "PrintScreen", false},
    {"prtsc", VK_SNAPSHOT, "PrintScreen", false},
    {"pause", VK_PAUSE, "Pause", false},
    {"space", VK_SPACE, "Space", true},
    {"tab", VK_TAB, "Tab", true},
    {"nummul", VK_MULTIPLY, "NumMul", false},
    {"numadd", VK_ADD, "NumAdd", false},
    {"numsub", VK_SUBTRACT, "NumSub", false},
    {"numdiv", VK_DIVIDE, "NumDiv", false},
    {"numdot", VK_DECIMAL, "NumDot", false},
    {"backtick", VK_OEM_3, "Backtick", true},
    {"`", VK_OEM_3, "Backtick", true},
    {"minus", VK_OEM_MINUS, "Minus", true},
    {"equals", VK_OEM_PLUS, "Equals", true},
};

std::string FormatDisplayCombo(uint32_t mods, std::string_view keyCanon) {
    std::string disp;
    if (mods & MOD_CONTROL) disp += "Ctrl+";
    if (mods & MOD_ALT) disp += "Alt+";
    if (mods & MOD_SHIFT) disp += "Shift+";
    if (mods & MOD_WIN) disp += "Win+";
    disp.append(keyCanon.data(), keyCanon.size());
    return disp;
}

std::string Trim(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

bool ParseUint(std::string_view s, uint32_t& out) {
    std::string t = Trim(s);
    if (t.empty()) return false;
    uint64_t val = 0;
    for (char ch : t) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        val = val * 10 + static_cast<uint64_t>(ch - '0');
        if (val > UINT32_MAX) return false;
    }
    out = static_cast<uint32_t>(val);
    return true;
}

bool ParseBool(std::string_view s, bool& out) {
    std::string t = ToLower(Trim(s));
    if (t == "1" || t == "true" || t == "yes" || t == "on") {
        out = true;
        return true;
    }
    if (t == "0" || t == "false" || t == "no" || t == "off") {
        out = false;
        return true;
    }
    return false;
}

bool ParseKeyToken(const std::string& lower, uint32_t& vk, std::string& canonical) {
    if (lower.size() == 1 && lower[0] >= 'a' && lower[0] <= 'z') {
        char up = static_cast<char>(std::toupper(static_cast<unsigned char>(lower[0])));
        vk = static_cast<uint32_t>(up);
        canonical = std::string(1, up);
        return true;
    }
    if (lower.size() == 1 && lower[0] >= '0' && lower[0] <= '9') {
        vk = static_cast<uint32_t>(lower[0]);
        canonical = lower;
        return true;
    }
    if (lower.size() >= 2 && lower[0] == 'f') {
        uint32_t fn = 0;
        if (ParseUint(std::string_view(lower).substr(1), fn) && fn >= 1 && fn <= 24) {
            vk = VK_F1 + (fn - 1);
            canonical = "F" + std::to_string(fn);
            return true;
        }
    }
    if (lower.size() == 7 && lower.rfind("numpad", 0) == 0 && lower[6] >= '0' && lower[6] <= '9') {
        vk = VK_NUMPAD0 + static_cast<uint32_t>(lower[6] - '0');
        canonical = std::string("Numpad") + lower[6];
        return true;
    }
    for (const auto& entry : kNamedKeys) {
        if (lower == entry.tokenLower) {
            vk = entry.vk;
            canonical = entry.canonical;
            return true;
        }
    }
    return false;
}

bool VkToCanonicalToken(uint32_t vk, std::string& token, bool& needsModifier) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        token = std::string(1, static_cast<char>(vk));
        needsModifier = true;
        return true;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        token = "F" + std::to_string((vk - VK_F1) + 1);
        needsModifier = false;
        return true;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        token = "Numpad" + std::to_string(vk - VK_NUMPAD0);
        needsModifier = false;
        return true;
    }
    for (const auto& entry : kNamedKeys) {
        if (vk == entry.vk) {
            token = entry.canonical;
            needsModifier = entry.requiresModifier;
            return true;
        }
    }
    return false;
}

void ClampAndValidate(AppConfig& cfg, std::vector<std::string>& warnings) {
    if (cfg.fps < kMinFps || cfg.fps > kMaxFps) {
        uint32_t clamped = std::clamp(cfg.fps, kMinFps, kMaxFps);
        warnings.push_back("fps clamped from " + std::to_string(cfg.fps) + " to " + std::to_string(clamped));
        cfg.fps = clamped;
    }
    if (cfg.bitrateMbps < kMinBitrateMbps || cfg.bitrateMbps > kMaxBitrateMbps) {
        uint32_t clamped = std::clamp(cfg.bitrateMbps, kMinBitrateMbps, kMaxBitrateMbps);
        warnings.push_back("bitrate_mbps clamped from " + std::to_string(cfg.bitrateMbps) + " to " + std::to_string(clamped));
        cfg.bitrateMbps = clamped;
    }
    if (cfg.replaySeconds < kMinReplaySeconds || cfg.replaySeconds > kMaxReplaySeconds) {
        uint32_t clamped = std::clamp(cfg.replaySeconds, kMinReplaySeconds, kMaxReplaySeconds);
        warnings.push_back("replay_seconds clamped from " + std::to_string(cfg.replaySeconds) + " to " + std::to_string(clamped));
        cfg.replaySeconds = clamped;
    }
    if (static_cast<uint64_t>(cfg.bitrateMbps) * cfg.replaySeconds > kMaxBitrateSecondsBudget) {
        uint32_t maxAllowedSeconds = std::max(kMinReplaySeconds, kMaxBitrateSecondsBudget / cfg.bitrateMbps);
        warnings.push_back("replay_seconds reduced from " + std::to_string(cfg.replaySeconds) + " to " +
                           std::to_string(maxAllowedSeconds) + " to stay within 2GB ring RAM budget");
        cfg.replaySeconds = maxAllowedSeconds;
    }
    if (cfg.outputDirRaw.empty()) {
        cfg.outputDirRaw = "%USERPROFILE%\\Videos\\Clips";
        warnings.push_back("empty output_dir reset to default");
    }
    cfg.outputDir = ExpandEnvPath(cfg.outputDirRaw);
}

void ApplyKeyValue(const std::string& key, const std::string& val, AppConfig& cfg, std::vector<std::string>& warnings) {
    if (key == "monitor_index") {
        uint32_t v = 0;
        if (ParseUint(val, v)) cfg.monitorIndex = v;
        else warnings.push_back("invalid monitor_index: " + val);
    } else if (key == "fps") {
        uint32_t v = 0;
        if (ParseUint(val, v)) cfg.fps = v;
        else warnings.push_back("invalid fps: " + val);
    } else if (key == "bitrate_mbps") {
        uint32_t v = 0;
        if (ParseUint(val, v)) cfg.bitrateMbps = v;
        else warnings.push_back("invalid bitrate_mbps: " + val);
    } else if (key == "replay_seconds") {
        uint32_t v = 0;
        if (ParseUint(val, v)) cfg.replaySeconds = v;
        else warnings.push_back("invalid replay_seconds: " + val);
    } else if (key == "record_desktop_audio") {
        bool b = true;
        if (ParseBool(val, b)) cfg.recordDesktopAudio = b;
        else warnings.push_back("invalid record_desktop_audio: " + val);
    } else if (key == "record_mic_audio") {
        bool b = true;
        if (ParseBool(val, b)) cfg.recordMicAudio = b;
        else warnings.push_back("invalid record_mic_audio: " + val);
    } else if (key == "hotkey_save") {
        HotkeySpec hk;
        if (ParseHotkeyString(val, hk)) cfg.hotkeySave = hk;
        else warnings.push_back("invalid hotkey_save kept as " + cfg.hotkeySave.display + ": " + val);
    } else if (key == "output_dir") {
        cfg.outputDirRaw = val;
    } else if (key == "save_chime") {
        bool b = true;
        if (ParseBool(val, b)) cfg.saveChime = b;
        else warnings.push_back("invalid save_chime: " + val);
    } else {
        warnings.push_back("unknown config key ignored: " + key);
    }
}

}

size_t AppConfig::VideoArenaBytes() const {
    double bytesPerSec = std::ceil(static_cast<double>(bitrateMbps) * 1.35 * 1'000'000.0 / 8.0);
    return static_cast<size_t>(replaySeconds + 3) * static_cast<size_t>(bytesPerSec);
}

size_t AppConfig::VideoMaxSlots() const {
    return static_cast<size_t>(replaySeconds + 3) * static_cast<size_t>(fps) + 64;
}

size_t AppConfig::AudioArenaBytes() const {
    return static_cast<size_t>(replaySeconds + 5) * (256'000 / 8);
}

size_t AppConfig::AudioMaxSlots() const {
    return static_cast<size_t>(replaySeconds + 5) * 48 + 64;
}

AppConfig MakeDefaultConfig() {
    AppConfig cfg;
    cfg.outputDir = ExpandEnvPath(cfg.outputDirRaw);
    return cfg;
}

bool ParseHotkeyString(std::string_view text, HotkeySpec& out) {
    std::string trimmed = Trim(text);
    if (trimmed.empty()) return false;

    uint32_t mods = 0;
    uint32_t vk = 0;
    std::string keyCanon;
    bool hasKey = false;

    size_t start = 0;
    while (start <= trimmed.size()) {
        size_t plus = trimmed.find('+', start);
        std::string token = Trim(plus == std::string::npos
                                     ? std::string_view(trimmed).substr(start)
                                     : std::string_view(trimmed).substr(start, plus - start));
        if (token.empty()) return false;
        std::string lower = ToLower(token);

        if (lower == "ctrl" || lower == "control") {
            mods |= MOD_CONTROL;
        } else if (lower == "alt") {
            mods |= MOD_ALT;
        } else if (lower == "shift") {
            mods |= MOD_SHIFT;
        } else if (lower == "win" || lower == "windows") {
            mods |= MOD_WIN;
        } else {
            if (hasKey) return false;
            if (!ParseKeyToken(lower, vk, keyCanon)) return false;
            hasKey = true;
        }

        if (plus == std::string::npos) break;
        start = plus + 1;
    }

    if (!hasKey) return false;

    out.modifiers = mods;
    out.vk = vk;
    out.display = FormatDisplayCombo(mods, keyCanon);
    return true;
}

bool IsSafeHotkey(uint32_t modifiers, uint32_t vk) {
    uint32_t mods = modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    std::string token;
    bool needsMod = false;
    if (!VkToCanonicalToken(vk, token, needsMod)) return false;
    return !needsMod || mods != 0;
}

bool FormatHotkeyFromVk(uint32_t modifiers, uint32_t vk, HotkeySpec& out) {
    uint32_t mods = modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    std::string token;
    bool needsMod = false;
    if (!VkToCanonicalToken(vk, token, needsMod)) return false;
    if (needsMod && mods == 0) return false;

    out.modifiers = mods;
    out.vk = vk;
    out.display = FormatDisplayCombo(mods, token);
    return true;
}

void ValidateAppConfig(AppConfig& cfg, std::vector<std::string>& warnings) {
    ClampAndValidate(cfg, warnings);
}

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::wstring ExpandEnvPath(std::string_view utf8Path) {
    std::wstring wide = Utf8ToWide(utf8Path);
    if (wide.empty()) return {};
    DWORD len = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
    if (len == 0) return wide;
    std::wstring out(static_cast<size_t>(len), L'\0');
    DWORD written = ExpandEnvironmentStringsW(wide.c_str(), out.data(), len);
    if (written == 0) return wide;
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

ConfigParseResult ParseConfigText(std::string_view text, AppConfig base) {
    ConfigParseResult res;
    res.config = std::move(base);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string_view line = (eol == std::string_view::npos) ? text.substr(pos) : text.substr(pos, eol - pos);
        pos = (eol == std::string_view::npos) ? text.size() : eol + 1;

        std::string clean = Trim(line);
        if (clean.empty() || clean[0] == '#' || clean[0] == ';' || clean[0] == '[') continue;

        size_t eq = clean.find('=');
        if (eq == std::string::npos) {
            res.warnings.push_back("malformed config line ignored: " + clean);
            continue;
        }

        std::string key = ToLower(Trim(std::string_view(clean).substr(0, eq)));
        std::string val = Trim(std::string_view(clean).substr(eq + 1));
        ApplyKeyValue(key, val, res.config, res.warnings);
    }

    ClampAndValidate(res.config, res.warnings);
    return res;
}

std::string SerializeConfig(const AppConfig& cfg) {
    std::ostringstream os;
    os << "[clipper]\n";
    os << "monitor_index=" << cfg.monitorIndex << "\n";
    os << "fps=" << cfg.fps << "\n";
    os << "bitrate_mbps=" << cfg.bitrateMbps << "\n";
    os << "replay_seconds=" << cfg.replaySeconds << "\n";
    os << "record_desktop_audio=" << (cfg.recordDesktopAudio ? "true" : "false") << "\n";
    os << "record_mic_audio=" << (cfg.recordMicAudio ? "true" : "false") << "\n";
    os << "hotkey_save=" << cfg.hotkeySave.display << "\n";
    os << "output_dir=" << cfg.outputDirRaw << "\n";
    os << "save_chime=" << (cfg.saveChime ? "true" : "false") << "\n";
    return os.str();
}

ConfigParseResult LoadOrCreateConfig(const std::wstring& iniPath) {
    std::ifstream ifs(std::filesystem::path(iniPath), std::ios::binary);
    if (!ifs.is_open()) {
        AppConfig def = MakeDefaultConfig();
        SaveConfig(iniPath, def);
        return { def, {} };
    }
    std::ostringstream buf;
    buf << ifs.rdbuf();
    return ParseConfigText(buf.str(), MakeDefaultConfig());
}

bool SaveConfig(const std::wstring& iniPath, const AppConfig& cfg) {
    std::error_code ec;
    std::filesystem::path p(iniPath);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) return false;
    }
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    std::string data = SerializeConfig(cfg);
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return ofs.good();
}

std::wstring DefaultConfigPath() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"clipper.ini";
    std::filesystem::path p(exePath);
    return (p.parent_path() / L"clipper.ini").wstring();
}

ConfigParseResult ApplyCliOverrides(int argc, wchar_t** argv, AppConfig base) {
    ConfigParseResult res;
    res.config = std::move(base);
    for (int i = 1; i < argc; ++i) {
        std::string arg = WideToUtf8(argv[i]);
        if (arg.rfind("--", 0) != 0) continue;
        std::string body = arg.substr(2);
        if (body == "no-desktop-audio") {
            res.config.recordDesktopAudio = false;
        } else if (body == "no-mic-audio") {
            res.config.recordMicAudio = false;
        } else if (body == "no-chime") {
            res.config.saveChime = false;
        } else {
            size_t eq = body.find('=');
            if (eq == std::string::npos) continue;
            std::string key = ToLower(body.substr(0, eq));
            std::string val = body.substr(eq + 1);
            if (key == "ready-event" || key == "out" || key == "runs") continue;
            if (key == "monitor") key = "monitor_index";
            else if (key == "bitrate") key = "bitrate_mbps";
            else if (key == "seconds") key = "replay_seconds";
            else if (key == "output-dir") key = "output_dir";
            else if (key == "hotkey") key = "hotkey_save";
            ApplyKeyValue(key, val, res.config, res.warnings);
        }
    }
    ClampAndValidate(res.config, res.warnings);
    return res;
}

}
