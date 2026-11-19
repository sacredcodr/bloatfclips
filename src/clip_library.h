#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clipper {

struct ClipEntry {
    std::wstring fullPath;
    std::wstring fileName;
    std::wstring displayTitle;
    std::wstring dateLabel;
    uint64_t sizeBytes = 0;
    uint64_t lastWriteTicks = 0;
    double durationSec = 0.0;
};

double ProbeMp4DurationSeconds(const std::wstring& path);
std::vector<ClipEntry> ScanClipsDirectory(const std::wstring& dir, size_t maxClips = 48);
std::wstring FormatBytesHuman(uint64_t bytes);
std::wstring FormatDurationShort(double seconds);

}
