#include "clip_library.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace clipper {

namespace {

uint32_t ReadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t ReadBe64(const uint8_t* p) {
    return (static_cast<uint64_t>(ReadBe32(p)) << 32) |
           static_cast<uint64_t>(ReadBe32(p + 4));
}

bool ReadAt(HANDLE file, uint64_t offset, void* dst, DWORD bytes) {
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN)) return false;
    DWORD readBytes = 0;
    return ReadFile(file, dst, bytes, &readBytes, nullptr) != FALSE && readBytes == bytes;
}

bool ReadBoxHeader(HANDLE file, uint64_t offset, uint64_t limitEnd,
                   uint64_t& boxSizeOut, uint64_t& headerSizeOut, char typeOut[4]) {
    if (offset + 8 > limitEnd) return false;
    uint8_t hdr[16] = {};
    if (!ReadAt(file, offset, hdr, 8)) return false;
    uint32_t size32 = ReadBe32(hdr);
    std::memcpy(typeOut, hdr + 4, 4);

    if (size32 == 1) {
        if (offset + 16 > limitEnd) return false;
        if (!ReadAt(file, offset + 8, hdr + 8, 8)) return false;
        boxSizeOut = ReadBe64(hdr + 8);
        headerSizeOut = 16;
    } else if (size32 == 0) {
        boxSizeOut = limitEnd - offset;
        headerSizeOut = 8;
    } else {
        boxSizeOut = size32;
        headerSizeOut = 8;
    }

    if (boxSizeOut < headerSizeOut) return false;
    if (boxSizeOut > limitEnd - offset) return false;
    return true;
}

double ParseMvhdDuration(HANDLE file, uint64_t bodyOffset, uint64_t bodySize) {
    if (bodySize < 20) return 0.0;
    uint8_t buf[32] = {};
    DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(bodySize, sizeof(buf)));
    if (!ReadAt(file, bodyOffset, buf, toRead)) return 0.0;

    uint8_t version = buf[0];
    if (version == 0 && toRead >= 20) {
        uint32_t timescale = ReadBe32(buf + 12);
        uint32_t duration = ReadBe32(buf + 16);
        return timescale > 0 ? static_cast<double>(duration) / static_cast<double>(timescale) : 0.0;
    }
    if (version == 1 && toRead >= 32) {
        uint32_t timescale = ReadBe32(buf + 20);
        uint64_t duration = ReadBe64(buf + 24);
        return timescale > 0 ? static_cast<double>(duration) / static_cast<double>(timescale) : 0.0;
    }
    return 0.0;
}

std::wstring FormatFileTimeLabel(const FILETIME& ft) {
    static constexpr const wchar_t* kMonths[] = {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
    };
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&ft, &utc)) return L"";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        local = utc;
    }
    const wchar_t* mon = (local.wMonth >= 1 && local.wMonth <= 12) ? kMonths[local.wMonth - 1] : L"---";
    wchar_t buf[64] = {};
    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%ls %u, %02u:%02u",
        mon, static_cast<unsigned>(local.wDay),
        static_cast<unsigned>(local.wHour), static_cast<unsigned>(local.wMinute));
    return buf;
}

bool EndsWithCaseInsensitive(std::wstring_view text, std::wstring_view suffix) {
    if (text.size() < suffix.size()) return false;
    size_t offset = text.size() - suffix.size();
    return _wcsnicmp(text.data() + offset, suffix.data(), suffix.size()) == 0;
}

}

double ProbeMp4DurationSeconds(const std::wstring& path) {
    if (path.empty()) return 0.0;
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0.0;

    LARGE_INTEGER fileSizeLi{};
    double result = 0.0;
    if (GetFileSizeEx(file, &fileSizeLi) && fileSizeLi.QuadPart >= 8) {
        uint64_t fileEnd = static_cast<uint64_t>(fileSizeLi.QuadPart);
        uint64_t offset = 0;
        for (int iter = 0; iter < 64 && offset < fileEnd; ++iter) {
            uint64_t boxSize = 0, headerSize = 0;
            char boxType[4] = {};
            if (!ReadBoxHeader(file, offset, fileEnd, boxSize, headerSize, boxType)) break;

            if (std::memcmp(boxType, "moov", 4) == 0) {
                uint64_t childOff = offset + headerSize;
                uint64_t moovEnd = offset + boxSize;
                for (int cIter = 0; cIter < 64 && childOff < moovEnd; ++cIter) {
                    uint64_t cSize = 0, cHdr = 0;
                    char cType[4] = {};
                    if (!ReadBoxHeader(file, childOff, moovEnd, cSize, cHdr, cType)) break;
                    if (std::memcmp(cType, "mvhd", 4) == 0) {
                        result = ParseMvhdDuration(file, childOff + cHdr, cSize - cHdr);
                        break;
                    }
                    childOff += cSize;
                }
                break;
            }
            offset += boxSize;
        }
    }

    CloseHandle(file);
    return result;
}

std::vector<ClipEntry> ScanClipsDirectory(const std::wstring& dir, size_t maxClips) {
    std::vector<ClipEntry> entries;
    if (dir.empty() || maxClips == 0) return entries;

    std::wstring baseDir = dir;
    while (baseDir.size() > 3 && (baseDir.back() == L'\\' || baseDir.back() == L'/')) {
        baseDir.pop_back();
    }

    std::wstring pattern = baseDir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern += L"*.mp4";

    WIN32_FIND_DATAW fd{};
    HANDLE findHandle = FindFirstFileW(pattern.c_str(), &fd);
    if (findHandle == INVALID_HANDLE_VALUE) return entries;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (!EndsWithCaseInsensitive(name, L".mp4")) continue;

        ClipEntry entry;
        entry.fileName = name;
        entry.fullPath = baseDir;
        if (!entry.fullPath.empty() && entry.fullPath.back() != L'\\' && entry.fullPath.back() != L'/') {
            entry.fullPath.push_back(L'\\');
        }
        entry.fullPath += name;
        entry.displayTitle = name.size() > 4 ? name.substr(0, name.size() - 4) : name;
        entry.sizeBytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        entry.lastWriteTicks = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                               fd.ftLastWriteTime.dwLowDateTime;
        entry.dateLabel = FormatFileTimeLabel(fd.ftLastWriteTime);
        entries.push_back(std::move(entry));
    } while (FindNextFileW(findHandle, &fd) != FALSE);

    FindClose(findHandle);

    std::sort(entries.begin(), entries.end(), [](const ClipEntry& a, const ClipEntry& b) {
        if (a.lastWriteTicks != b.lastWriteTicks) return a.lastWriteTicks > b.lastWriteTicks;
        return a.fileName > b.fileName;
    });

    if (entries.size() > maxClips) {
        entries.resize(maxClips);
    }

    for (auto& e : entries) {
        e.durationSec = ProbeMp4DurationSeconds(e.fullPath);
    }

    return entries;
}

std::wstring FormatBytesHuman(uint64_t bytes) {
    wchar_t buf[64] = {};
    if (bytes < 1024ULL) {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%llu B",
            static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ULL * 1024ULL) {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%.1f KB",
            static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%.1f MB",
            static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%.2f GB",
            static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

std::wstring FormatDurationShort(double seconds) {
    if (seconds < 0.05) return L"";
    uint64_t total = static_cast<uint64_t>(std::llround(seconds));
    if (total == 0) total = 1;
    uint64_t hrs = total / 3600ULL;
    uint64_t mins = (total % 3600ULL) / 60ULL;
    uint64_t secs = total % 60ULL;
    wchar_t buf[32] = {};
    if (hrs > 0) {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%llu:%02llu:%02llu",
            static_cast<unsigned long long>(hrs),
            static_cast<unsigned long long>(mins),
            static_cast<unsigned long long>(secs));
    } else {
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%llu:%02llu",
            static_cast<unsigned long long>(mins),
            static_cast<unsigned long long>(secs));
    }
    return buf;
}

}
