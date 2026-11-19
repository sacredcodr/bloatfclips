#pragma once

#include "ring_buffer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clipper {

struct Mp4MuxInput {
    uint32_t width = 0;
    uint32_t height = 0;
    ClipStreamSnapshot video;
    ClipStreamSnapshot audio;
    std::vector<uint8_t> audioSpecificConfig;
    uint32_t audioSampleRate = 48000;
    uint32_t audioChannels = 2;
    uint32_t audioBitrateBps = 192000;
};

struct MdatOffsetPlan {
    bool useCo64 = false;
    bool useLargeMdat = false;
    uint64_t mdatPayloadBytes = 0;
    uint64_t mdatHeaderBytes = 8;
    std::vector<uint64_t> chunkFileOffsets;
};

std::vector<uint8_t> ExtractAudioSpecificConfigFromMfUserData(const uint8_t* data, size_t len);
MdatOffsetPlan ComputeMdatOffsetPlan(size_t ftypBytes, size_t moovBytesIfStco, size_t moovBytesIfCo64, const std::vector<uint64_t>& chunkByteSizes);

bool BuildMp4Bytes(const Mp4MuxInput& input, std::vector<uint8_t>& outBytes, std::string& errOut);
bool WriteMp4File(const std::wstring& path, const Mp4MuxInput& input, std::string& errOut);

}
