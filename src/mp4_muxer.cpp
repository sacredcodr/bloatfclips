#include "mp4_muxer.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace clipper {

namespace {

constexpr uint32_t kVideoTimescale = 90000;

void AppendU8(std::vector<uint8_t>& b, uint8_t v) {
    b.push_back(v);
}

void AppendBE16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendBE24(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendBE32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendBE64(std::vector<uint8_t>& b, uint64_t v) {
    AppendBE32(b, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
    AppendBE32(b, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
}

void AppendFourCC(std::vector<uint8_t>& b, const char cc[4]) {
    b.insert(b.end(), cc, cc + 4);
}

void AppendZeros(std::vector<uint8_t>& b, size_t n) {
    b.insert(b.end(), n, 0);
}

size_t BeginBox(std::vector<uint8_t>& b, const char cc[4]) {
    size_t pos = b.size();
    AppendBE32(b, 0);
    AppendFourCC(b, cc);
    return pos;
}

size_t BeginFullBox(std::vector<uint8_t>& b, const char cc[4], uint8_t version, uint32_t flags) {
    size_t pos = BeginBox(b, cc);
    AppendU8(b, version);
    AppendBE24(b, flags);
    return pos;
}

void EndBox(std::vector<uint8_t>& b, size_t pos) {
    uint32_t sz = static_cast<uint32_t>(b.size() - pos);
    b[pos] = static_cast<uint8_t>((sz >> 24) & 0xFF);
    b[pos + 1] = static_cast<uint8_t>((sz >> 16) & 0xFF);
    b[pos + 2] = static_cast<uint8_t>((sz >> 8) & 0xFF);
    b[pos + 3] = static_cast<uint8_t>(sz & 0xFF);
}

struct NalSpan {
    const uint8_t* data;
    size_t size;
};

std::vector<NalSpan> SplitAnnexBNals(const uint8_t* data, size_t len) {
    std::vector<NalSpan> nals;
    auto findStartCode = [&](size_t from, size_t& scPos, size_t& scLen) -> bool {
        for (size_t i = from; i + 2 < len; ++i) {
            if (data[i] == 0 && data[i + 1] == 0) {
                if (data[i + 2] == 1) {
                    scPos = i;
                    scLen = 3;
                    return true;
                }
                if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
                    scPos = i;
                    scLen = 4;
                    return true;
                }
            }
        }
        return false;
    };

    size_t scPos = 0, scLen = 0;
    if (!findStartCode(0, scPos, scLen)) {
        if (len > 0) nals.push_back({ data, len });
        return nals;
    }

    size_t nalStart = scPos + scLen;
    while (nalStart < len) {
        size_t nextPos = 0, nextLen = 0;
        if (!findStartCode(nalStart, nextPos, nextLen)) {
            if (nalStart < len) nals.push_back({ data + nalStart, len - nalStart });
            break;
        }
        if (nextPos > nalStart) {
            nals.push_back({ data + nalStart, nextPos - nalStart });
        }
        nalStart = nextPos + nextLen;
    }
    return nals;
}

class RbspBitReader {
public:
    explicit RbspBitReader(const uint8_t* nal, size_t len) {
        rbsp_.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            if (i >= 2 && nal[i] == 0x03 && nal[i - 1] == 0x00 && nal[i - 2] == 0x00) continue;
            rbsp_.push_back(nal[i]);
        }
    }

    uint32_t ReadBits(int n) {
        uint32_t val = 0;
        for (int i = 0; i < n; ++i) {
            val = (val << 1) | ReadBit();
        }
        return val;
    }

    uint32_t ReadBit() {
        if (bitPos_ >= rbsp_.size() * 8) return 0;
        uint32_t b = (rbsp_[bitPos_ >> 3] >> (7 - (bitPos_ & 7))) & 1u;
        bitPos_++;
        return b;
    }

    uint32_t ReadUE() {
        int zeros = 0;
        while (ReadBit() == 0 && zeros < 31) zeros++;
        if (zeros == 0) return 0;
        return ((1u << zeros) - 1u) + ReadBits(zeros);
    }

private:
    std::vector<uint8_t> rbsp_;
    size_t bitPos_ = 0;
};

bool IsHighProfileIdc(uint8_t profileIdc) {
    switch (profileIdc) {
        case 44:
        case 83:
        case 86:
        case 100:
        case 110:
        case 118:
        case 122:
        case 128:
        case 134:
        case 138:
        case 139:
        case 144:
        case 244:
            return true;
        default:
            return false;
    }
}

struct AvcSpsChromaInfo {
    uint8_t chromaFormat = 1;
    uint8_t bitDepthLumaMinus8 = 0;
    uint8_t bitDepthChromaMinus8 = 0;
};

AvcSpsChromaInfo ParseSpsChromaInfo(const std::vector<uint8_t>& sps) {
    AvcSpsChromaInfo info;
    if (sps.size() < 4 || !IsHighProfileIdc(sps[1])) return info;
    RbspBitReader br(sps.data() + 1, sps.size() - 1);
    uint32_t profileIdc = br.ReadBits(8);
    br.ReadBits(8);
    br.ReadBits(8);
    br.ReadUE();
    if (IsHighProfileIdc(static_cast<uint8_t>(profileIdc))) {
        uint32_t chroma = br.ReadUE();
        info.chromaFormat = static_cast<uint8_t>(chroma & 0x3);
        if (chroma == 3) br.ReadBit();
        info.bitDepthLumaMinus8 = static_cast<uint8_t>(br.ReadUE() & 0x7);
        info.bitDepthChromaMinus8 = static_cast<uint8_t>(br.ReadUE() & 0x7);
    }
    return info;
}

// AVCC sample = vclNals[nalFirst, nalFirst + nalCount), each written as a 4-byte BE length plus
// the NAL payload, which stays in the input snapshot so a save never holds a second clip copy.
struct PreparedVideoSample {
    uint32_t nalFirst = 0;
    uint32_t nalCount = 0;
    uint32_t avccSize = 0;
    uint32_t durationTicks = 1500;
    bool isKeyFrame = false;
};

struct PreparedChunk {
    bool isVideo = true;
    uint32_t firstSample = 0;
    uint32_t sampleCount = 0;
    uint64_t byteSize = 0;
};

struct SttsRun {
    uint32_t count = 0;
    uint32_t delta = 0;
};

struct StscRun {
    uint32_t firstChunk = 1;
    uint32_t samplesPerChunk = 0;
    uint32_t sampleDescIndex = 1;
};

bool SynthesizeAacLcAsc(uint32_t sampleRate, uint32_t channels, std::vector<uint8_t>& outAsc) {
    static constexpr uint32_t kRates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    uint32_t freqIdx = UINT32_MAX;
    for (uint32_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); ++i) {
        if (kRates[i] == sampleRate) {
            freqIdx = i;
            break;
        }
    }
    if (freqIdx == UINT32_MAX || channels == 0 || channels > 7) {
        return false;
    }
    uint16_t val = static_cast<uint16_t>((2u << 11) | (freqIdx << 7) | (channels << 3));
    outAsc = {
        static_cast<uint8_t>((val >> 8) & 0xFFu),
        static_cast<uint8_t>(val & 0xFFu)
    };
    return true;
}

struct PreparedMuxData {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<PreparedVideoSample> videoSamples;
    std::vector<NalSpan> vclNals;
    std::vector<uint32_t> keyFrameIndices;
    std::vector<SttsRun> videoStts;
    uint64_t videoTotalTicks = 0;

    bool hasAudio = false;
    std::vector<uint8_t> audioAsc;
    uint32_t audioSampleRate = 48000;
    uint32_t audioChannels = 2;
    uint32_t audioBitrateBps = 192000;
    std::vector<uint32_t> audioSampleSizes;
    uint64_t audioTotalSamples = 0;
    uint32_t audioDelayTicks = 0;
    uint32_t audioMediaStart = 0;
    uint64_t audioTrackDuration = 0;
    uint64_t movieDuration = 0;

    std::vector<PreparedChunk> chunks;
    std::vector<StscRun> videoStsc;
    std::vector<StscRun> audioStsc;
    std::vector<uint32_t> videoChunkIndices;
    std::vector<uint32_t> audioChunkIndices;
    std::vector<uint64_t> chunkByteSizes;
};

bool PrepareMuxData(const Mp4MuxInput& input, PreparedMuxData& out, std::string& errOut) {
    if (input.width == 0 || input.height == 0) {
        errOut = "invalid video dimensions";
        return false;
    }
    if (input.video.Empty()) {
        errOut = "empty video snapshot";
        return false;
    }

    const size_t nv = input.video.packets.size();
    out.videoSamples.resize(nv);

    for (size_t i = 0; i < nv; ++i) {
        const ClipPacket& pkt = input.video.packets[i];
        const uint8_t* ptr = input.video.bytes.data() + pkt.offset;
        auto nals = SplitAnnexBNals(ptr, pkt.size);
        PreparedVideoSample& sample = out.videoSamples[i];
        sample.nalFirst = static_cast<uint32_t>(out.vclNals.size());
        uint64_t avccSize = 0;
        for (const auto& nal : nals) {
            if (nal.size == 0) continue;
            uint8_t nalType = nal.data[0] & 0x1Fu;
            if (nalType == 7) {
                if (out.sps.empty()) out.sps.assign(nal.data, nal.data + nal.size);
                continue;
            }
            if (nalType == 8) {
                if (out.pps.empty()) out.pps.assign(nal.data, nal.data + nal.size);
                continue;
            }
            if (nalType == 9) {
                continue;
            }
            out.vclNals.push_back(nal);
            avccSize += 4 + nal.size;
        }
        sample.nalCount = static_cast<uint32_t>(out.vclNals.size() - sample.nalFirst);
        sample.avccSize = static_cast<uint32_t>(avccSize);
        if (sample.nalCount == 0) {
            errOut = "video packet produced zero VCL NAL units";
            return false;
        }
        out.videoSamples[i].isKeyFrame = pkt.isKeyFrame;
        if (pkt.isKeyFrame) {
            out.keyFrameIndices.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    if (out.sps.size() < 4 || out.pps.empty()) {
        errOut = "missing H.264 SPS or PPS parameter sets";
        return false;
    }

    int64_t basePtsUs = input.video.packets[0].ptsUs;
    int64_t prevTick = 0;
    for (size_t i = 0; i < nv; ++i) {
        uint32_t dur = 1500;
        if (i + 1 < nv) {
            double rawTick = static_cast<double>(input.video.packets[i + 1].ptsUs - basePtsUs) * kVideoTimescale / 1'000'000.0;
            int64_t nextTick = std::max(prevTick + 1, static_cast<int64_t>(std::llround(rawTick)));
            dur = static_cast<uint32_t>(nextTick - prevTick);
            prevTick = nextTick;
        } else {
            uint32_t lastUs = input.video.packets[i].durationUs > 0 ? input.video.packets[i].durationUs : 16667u;
            int64_t d = static_cast<int64_t>(std::llround(static_cast<double>(lastUs) * kVideoTimescale / 1'000'000.0));
            dur = static_cast<uint32_t>(std::max<int64_t>(1, d));
        }
        out.videoSamples[i].durationTicks = dur;
        out.videoTotalTicks += dur;
        if (out.videoStts.empty() || out.videoStts.back().delta != dur) {
            out.videoStts.push_back({ 1, dur });
        } else {
            out.videoStts.back().count++;
        }
    }

    out.hasAudio = !input.audio.Empty();
    if (out.hasAudio) {
        out.audioSampleRate = input.audioSampleRate ? input.audioSampleRate : 48000u;
        out.audioChannels = input.audioChannels ? input.audioChannels : 2u;
        out.audioBitrateBps = input.audioBitrateBps ? input.audioBitrateBps : 192000u;
        if (!input.audioSpecificConfig.empty()) {
            out.audioAsc = input.audioSpecificConfig;
        } else if (!SynthesizeAacLcAsc(out.audioSampleRate, out.audioChannels, out.audioAsc)) {
            errOut = "unsupported audio parameters for AudioSpecificConfig";
            return false;
        }
        if (out.audioAsc.size() > 64) {
            errOut = "AudioSpecificConfig exceeds maximum supported length";
            return false;
        }
        out.audioSampleSizes.reserve(input.audio.packets.size());
        for (const auto& ap : input.audio.packets) {
            out.audioSampleSizes.push_back(ap.size);
        }
        out.audioTotalSamples = static_cast<uint64_t>(out.audioSampleSizes.size()) * 1024ULL;

        int64_t audioDeltaUs = input.audio.packets.front().ptsUs - basePtsUs;
        if (audioDeltaUs >= 1000) {
            out.audioDelayTicks = static_cast<uint32_t>(std::clamp<int64_t>(
                std::llround(static_cast<double>(audioDeltaUs) * kVideoTimescale / 1'000'000.0),
                1LL, static_cast<int64_t>(UINT32_MAX / 2)));
        } else if (audioDeltaUs <= -1000 && out.audioTotalSamples > 1) {
            uint64_t trimSamples = static_cast<uint64_t>(
                std::llround(static_cast<double>(-audioDeltaUs) * out.audioSampleRate / 1'000'000.0));
            out.audioMediaStart = static_cast<uint32_t>(
                std::min<uint64_t>(trimSamples, out.audioTotalSamples - 1ULL));
        }

        uint64_t playedSamples = (out.audioTotalSamples > out.audioMediaStart)
            ? (out.audioTotalSamples - out.audioMediaStart)
            : 0ULL;
        uint64_t playedMovieDur = static_cast<uint64_t>(
            std::llround(static_cast<double>(playedSamples) * kVideoTimescale / out.audioSampleRate));
        out.audioTrackDuration = static_cast<uint64_t>(out.audioDelayTicks) + playedMovieDur;
    }

    out.movieDuration = std::max(out.videoTotalTicks, out.audioTrackDuration);

    size_t vIdx = 0;
    size_t aIdx = 0;
    const size_t na = input.audio.packets.size();
    int64_t bucketEndUs = basePtsUs + 1'000'000;

    while (vIdx < nv || aIdx < na) {
        if (vIdx < nv) {
            uint32_t vStart = static_cast<uint32_t>(vIdx);
            uint64_t vBytes = 0;
            while (vIdx < nv && (input.video.packets[vIdx].ptsUs < bucketEndUs || vIdx == vStart)) {
                vBytes += out.videoSamples[vIdx].avccSize;
                vIdx++;
            }
            if (vIdx > vStart && input.video.packets[vIdx - 1].ptsUs >= bucketEndUs) {
                bucketEndUs = input.video.packets[vIdx - 1].ptsUs + 1'000'000;
            }
            out.videoChunkIndices.push_back(static_cast<uint32_t>(out.chunks.size()));
            out.chunks.push_back(PreparedChunk{ true, vStart, static_cast<uint32_t>(vIdx - vStart), vBytes });
            out.chunkByteSizes.push_back(vBytes);
        }
        if (aIdx < na) {
            uint32_t aStart = static_cast<uint32_t>(aIdx);
            uint64_t aBytes = 0;
            bool drainRest = (vIdx == nv);
            while (aIdx < na && (drainRest || input.audio.packets[aIdx].ptsUs < bucketEndUs)) {
                aBytes += out.audioSampleSizes[aIdx];
                aIdx++;
            }
            if (aIdx > aStart) {
                out.audioChunkIndices.push_back(static_cast<uint32_t>(out.chunks.size()));
                out.chunks.push_back(PreparedChunk{ false, aStart, static_cast<uint32_t>(aIdx - aStart), aBytes });
                out.chunkByteSizes.push_back(aBytes);
            }
        }
        bucketEndUs += 1'000'000;
    }

    auto buildStsc = [&](const std::vector<uint32_t>& chunkIdxs, std::vector<StscRun>& stscOut) {
        for (size_t i = 0; i < chunkIdxs.size(); ++i) {
            uint32_t spc = out.chunks[chunkIdxs[i]].sampleCount;
            if (stscOut.empty() || stscOut.back().samplesPerChunk != spc) {
                stscOut.push_back(StscRun{ static_cast<uint32_t>(i + 1), spc, 1 });
            }
        }
    };
    buildStsc(out.videoChunkIndices, out.videoStsc);
    if (out.hasAudio) buildStsc(out.audioChunkIndices, out.audioStsc);

    return true;
}

std::vector<uint8_t> BuildFtypBox() {
    std::vector<uint8_t> b;
    size_t pos = BeginBox(b, "ftyp");
    AppendFourCC(b, "isom");
    AppendBE32(b, 0x00000200);
    AppendFourCC(b, "isom");
    AppendFourCC(b, "iso2");
    AppendFourCC(b, "avc1");
    AppendFourCC(b, "mp41");
    EndBox(b, pos);
    return b;
}

void AppendIdentityMatrix(std::vector<uint8_t>& b) {
    static constexpr uint32_t kMat[9] = {
        0x00010000, 0, 0,
        0, 0x00010000, 0,
        0, 0, 0x40000000
    };
    for (uint32_t v : kMat) AppendBE32(b, v);
}

void AppendDinfBox(std::vector<uint8_t>& b) {
    size_t dinf = BeginBox(b, "dinf");
    size_t dref = BeginFullBox(b, "dref", 0, 0);
    AppendBE32(b, 1);
    size_t url = BeginFullBox(b, "url ", 0, 1);
    EndBox(b, url);
    EndBox(b, dref);
    EndBox(b, dinf);
}

void AppendChunkOffsetsBox(std::vector<uint8_t>& b, const std::vector<uint32_t>& trackChunkIdxs,
                           const std::vector<uint64_t>* chunkOffsets, bool useCo64) {
    size_t box = BeginFullBox(b, useCo64 ? "co64" : "stco", 0, 0);
    AppendBE32(b, static_cast<uint32_t>(trackChunkIdxs.size()));
    for (size_t i = 0; i < trackChunkIdxs.size(); ++i) {
        uint64_t off = chunkOffsets ? (*chunkOffsets)[trackChunkIdxs[i]] : 0ULL;
        if (useCo64) AppendBE64(b, off);
        else AppendBE32(b, static_cast<uint32_t>(off));
    }
    EndBox(b, box);
}

std::vector<uint8_t> BuildMoovBox(const Mp4MuxInput& input, const PreparedMuxData& data,
                                  bool useCo64, const std::vector<uint64_t>* chunkOffsets) {
    std::vector<uint8_t> b;
    size_t moov = BeginBox(b, "moov");

    size_t mvhd = BeginFullBox(b, "mvhd", 0, 0);
    AppendBE32(b, 0);
    AppendBE32(b, 0);
    AppendBE32(b, kVideoTimescale);
    AppendBE32(b, static_cast<uint32_t>(std::min<uint64_t>(data.movieDuration, UINT32_MAX)));
    AppendBE32(b, 0x00010000);
    AppendBE16(b, 0x0100);
    AppendZeros(b, 10);
    AppendIdentityMatrix(b);
    AppendZeros(b, 24);
    AppendBE32(b, data.hasAudio ? 3u : 2u);
    EndBox(b, mvhd);

    {
        size_t trak = BeginBox(b, "trak");
        size_t tkhd = BeginFullBox(b, "tkhd", 0, 3);
        AppendBE32(b, 0);
        AppendBE32(b, 0);
        AppendBE32(b, 1);
        AppendBE32(b, 0);
        AppendBE32(b, static_cast<uint32_t>(std::min<uint64_t>(data.videoTotalTicks, UINT32_MAX)));
        AppendZeros(b, 8);
        AppendBE16(b, 0);
        AppendBE16(b, 0);
        AppendBE16(b, 0);
        AppendBE16(b, 0);
        AppendIdentityMatrix(b);
        AppendBE32(b, input.width << 16);
        AppendBE32(b, input.height << 16);
        EndBox(b, tkhd);

        size_t mdia = BeginBox(b, "mdia");
        size_t mdhd = BeginFullBox(b, "mdhd", 0, 0);
        AppendBE32(b, 0);
        AppendBE32(b, 0);
        AppendBE32(b, kVideoTimescale);
        AppendBE32(b, static_cast<uint32_t>(std::min<uint64_t>(data.videoTotalTicks, UINT32_MAX)));
        AppendBE16(b, 0x55C4);
        AppendBE16(b, 0);
        EndBox(b, mdhd);

        size_t hdlr = BeginFullBox(b, "hdlr", 0, 0);
        AppendBE32(b, 0);
        AppendFourCC(b, "vide");
        AppendZeros(b, 12);
        static const char kVideoName[] = "VideoHandler";
        b.insert(b.end(), kVideoName, kVideoName + sizeof(kVideoName));
        EndBox(b, hdlr);

        size_t minf = BeginBox(b, "minf");
        size_t vmhd = BeginFullBox(b, "vmhd", 0, 1);
        AppendZeros(b, 8);
        EndBox(b, vmhd);
        AppendDinfBox(b);

        size_t stbl = BeginBox(b, "stbl");

        size_t stsd = BeginFullBox(b, "stsd", 0, 0);
        AppendBE32(b, 1);
        {
            size_t avc1 = BeginBox(b, "avc1");
            AppendZeros(b, 6);
            AppendBE16(b, 1);
            AppendZeros(b, 16);
            AppendBE16(b, static_cast<uint16_t>(input.width));
            AppendBE16(b, static_cast<uint16_t>(input.height));
            AppendBE32(b, 0x00480000);
            AppendBE32(b, 0x00480000);
            AppendBE32(b, 0);
            AppendBE16(b, 1);
            AppendZeros(b, 32);
            AppendBE16(b, 0x0018);
            AppendBE16(b, 0xFFFF);

            size_t avcc = BeginBox(b, "avcC");
            AppendU8(b, 1);
            AppendU8(b, data.sps[1]);
            AppendU8(b, data.sps[2]);
            AppendU8(b, data.sps[3]);
            AppendU8(b, 0xFF);
            AppendU8(b, 0xE1);
            AppendBE16(b, static_cast<uint16_t>(data.sps.size()));
            b.insert(b.end(), data.sps.begin(), data.sps.end());
            AppendU8(b, 1);
            AppendBE16(b, static_cast<uint16_t>(data.pps.size()));
            b.insert(b.end(), data.pps.begin(), data.pps.end());
            if (IsHighProfileIdc(data.sps[1])) {
                AvcSpsChromaInfo ci = ParseSpsChromaInfo(data.sps);
                AppendU8(b, static_cast<uint8_t>(0xFCu | (ci.chromaFormat & 0x3u)));
                AppendU8(b, static_cast<uint8_t>(0xF8u | (ci.bitDepthLumaMinus8 & 0x7u)));
                AppendU8(b, static_cast<uint8_t>(0xF8u | (ci.bitDepthChromaMinus8 & 0x7u)));
                AppendU8(b, 0);
            }
            EndBox(b, avcc);

            size_t colr = BeginBox(b, "colr");
            AppendFourCC(b, "nclx");
            AppendBE16(b, 1);
            AppendBE16(b, 1);
            AppendBE16(b, 1);
            AppendU8(b, 0);
            EndBox(b, colr);

            EndBox(b, avc1);
        }
        EndBox(b, stsd);

        size_t stts = BeginFullBox(b, "stts", 0, 0);
        AppendBE32(b, static_cast<uint32_t>(data.videoStts.size()));
        for (const auto& run : data.videoStts) {
            AppendBE32(b, run.count);
            AppendBE32(b, run.delta);
        }
        EndBox(b, stts);

        if (!data.keyFrameIndices.empty()) {
            size_t stss = BeginFullBox(b, "stss", 0, 0);
            AppendBE32(b, static_cast<uint32_t>(data.keyFrameIndices.size()));
            for (uint32_t idx : data.keyFrameIndices) AppendBE32(b, idx);
            EndBox(b, stss);
        }

        size_t stsc = BeginFullBox(b, "stsc", 0, 0);
        AppendBE32(b, static_cast<uint32_t>(data.videoStsc.size()));
        for (const auto& run : data.videoStsc) {
            AppendBE32(b, run.firstChunk);
            AppendBE32(b, run.samplesPerChunk);
            AppendBE32(b, run.sampleDescIndex);
        }
        EndBox(b, stsc);

        size_t stsz = BeginFullBox(b, "stsz", 0, 0);
        AppendBE32(b, 0);
        AppendBE32(b, static_cast<uint32_t>(data.videoSamples.size()));
        for (const auto& s : data.videoSamples) {
            AppendBE32(b, s.avccSize);
        }
        EndBox(b, stsz);

        AppendChunkOffsetsBox(b, data.videoChunkIndices, chunkOffsets, useCo64);

        EndBox(b, stbl);
        EndBox(b, minf);
        EndBox(b, mdia);
        EndBox(b, trak);
    }

    if (data.hasAudio) {
        size_t trak = BeginBox(b, "trak");
        uint64_t audioMovieDur = data.audioTrackDuration;
        uint64_t playedSamples = (data.audioTotalSamples > data.audioMediaStart)
            ? (data.audioTotalSamples - data.audioMediaStart)
            : 0ULL;
        uint32_t playedDur = static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(std::llround(static_cast<double>(playedSamples) * kVideoTimescale / data.audioSampleRate)),
            UINT32_MAX));

        size_t tkhd = BeginFullBox(b, "tkhd", 0, 3);
        AppendBE32(b, 0);
        AppendBE32(b, 0);
        AppendBE32(b, 2);
        AppendBE32(b, 0);
        AppendBE32(b, static_cast<uint32_t>(std::min<uint64_t>(audioMovieDur, UINT32_MAX)));
        AppendZeros(b, 8);
        AppendBE16(b, 0);
        AppendBE16(b, 0);
        AppendBE16(b, 0x0100);
        AppendBE16(b, 0);
        AppendIdentityMatrix(b);
        AppendBE32(b, 0);
        AppendBE32(b, 0);
        EndBox(b, tkhd);

        if (data.audioDelayTicks > 0 || data.audioMediaStart > 0) {
            size_t edts = BeginBox(b, "edts");
            size_t elst = BeginFullBox(b, "elst", 0, 0);
            if (data.audioDelayTicks > 0) {
                AppendBE32(b, 2);
                AppendBE32(b, data.audioDelayTicks);
                AppendBE32(b, 0xFFFFFFFFu);
                AppendBE16(b, 1);
                AppendBE16(b, 0);
                AppendBE32(b, playedDur);
                AppendBE32(b, 0);
                AppendBE16(b, 1);
                AppendBE16(b, 0);
            } else {
                AppendBE32(b, 1);
                AppendBE32(b, playedDur);
                AppendBE32(b, data.audioMediaStart);
                AppendBE16(b, 1);
                AppendBE16(b, 0);
            }
            EndBox(b, elst);
            EndBox(b, edts);
        }

        size_t mdia = BeginBox(b, "mdia");
        size_t mdhd = BeginFullBox(b, "mdhd", 0, 0);
        AppendBE32(b, 0);
        AppendBE32(b, 0);
        AppendBE32(b, data.audioSampleRate);
        AppendBE32(b, static_cast<uint32_t>(std::min<uint64_t>(data.audioTotalSamples, UINT32_MAX)));
        AppendBE16(b, 0x55C4);
        AppendBE16(b, 0);
        EndBox(b, mdhd);

        size_t hdlr = BeginFullBox(b, "hdlr", 0, 0);
        AppendBE32(b, 0);
        AppendFourCC(b, "soun");
        AppendZeros(b, 12);
        static const char kSoundName[] = "SoundHandler";
        b.insert(b.end(), kSoundName, kSoundName + sizeof(kSoundName));
        EndBox(b, hdlr);

        size_t minf = BeginBox(b, "minf");
        size_t smhd = BeginFullBox(b, "smhd", 0, 0);
        AppendZeros(b, 4);
        EndBox(b, smhd);
        AppendDinfBox(b);

        size_t stbl = BeginBox(b, "stbl");

        size_t stsd = BeginFullBox(b, "stsd", 0, 0);
        AppendBE32(b, 1);
        {
            size_t mp4a = BeginBox(b, "mp4a");
            AppendZeros(b, 6);
            AppendBE16(b, 1);
            AppendZeros(b, 8);
            AppendBE16(b, static_cast<uint16_t>(data.audioChannels));
            AppendBE16(b, 16);
            AppendBE32(b, 0);
            AppendBE32(b, data.audioSampleRate << 16);

            size_t esds = BeginFullBox(b, "esds", 0, 0);
            uint32_t decSpecLen = static_cast<uint32_t>(data.audioAsc.size());
            uint32_t decCfgLen = 13u + 2u + decSpecLen;
            uint32_t esLen = 3u + 2u + decCfgLen + 3u;
            AppendU8(b, 0x03);
            AppendU8(b, static_cast<uint8_t>(esLen));
            AppendBE16(b, 0x0002);
            AppendU8(b, 0x00);

            AppendU8(b, 0x04);
            AppendU8(b, static_cast<uint8_t>(decCfgLen));
            AppendU8(b, 0x40);
            AppendU8(b, 0x15);
            AppendBE24(b, 0);
            AppendBE32(b, data.audioBitrateBps);
            AppendBE32(b, data.audioBitrateBps);

            AppendU8(b, 0x05);
            AppendU8(b, static_cast<uint8_t>(decSpecLen));
            b.insert(b.end(), data.audioAsc.begin(), data.audioAsc.end());

            AppendU8(b, 0x06);
            AppendU8(b, 0x01);
            AppendU8(b, 0x02);
            EndBox(b, esds);

            EndBox(b, mp4a);
        }
        EndBox(b, stsd);

        size_t stts = BeginFullBox(b, "stts", 0, 0);
        AppendBE32(b, 1);
        AppendBE32(b, static_cast<uint32_t>(data.audioSampleSizes.size()));
        AppendBE32(b, 1024);
        EndBox(b, stts);

        size_t stsc = BeginFullBox(b, "stsc", 0, 0);
        AppendBE32(b, static_cast<uint32_t>(data.audioStsc.size()));
        for (const auto& run : data.audioStsc) {
            AppendBE32(b, run.firstChunk);
            AppendBE32(b, run.samplesPerChunk);
            AppendBE32(b, run.sampleDescIndex);
        }
        EndBox(b, stsc);

        size_t stsz = BeginFullBox(b, "stsz", 0, 0);
        AppendBE32(b, 0);
        AppendBE32(b, static_cast<uint32_t>(data.audioSampleSizes.size()));
        for (uint32_t sz : data.audioSampleSizes) AppendBE32(b, sz);
        EndBox(b, stsz);

        AppendChunkOffsetsBox(b, data.audioChunkIndices, chunkOffsets, useCo64);

        EndBox(b, stbl);
        EndBox(b, minf);
        EndBox(b, mdia);
        EndBox(b, trak);
    }

    EndBox(b, moov);
    return b;
}

struct SerializedHeaders {
    std::vector<uint8_t> ftyp;
    std::vector<uint8_t> moov;
    std::vector<uint8_t> mdatHeader;
    MdatOffsetPlan plan;
};

SerializedHeaders BuildSerializedHeaders(const Mp4MuxInput& input, const PreparedMuxData& data) {
    SerializedHeaders h;
    h.ftyp = BuildFtypBox();
    std::vector<uint8_t> stcoProbe = BuildMoovBox(input, data, false, nullptr);
    std::vector<uint8_t> co64Probe = BuildMoovBox(input, data, true, nullptr);
    h.plan = ComputeMdatOffsetPlan(h.ftyp.size(), stcoProbe.size(), co64Probe.size(), data.chunkByteSizes);
    h.moov = BuildMoovBox(input, data, h.plan.useCo64, &h.plan.chunkFileOffsets);

    if (h.plan.useLargeMdat) {
        AppendBE32(h.mdatHeader, 1);
        AppendFourCC(h.mdatHeader, "mdat");
        AppendBE64(h.mdatHeader, h.plan.mdatHeaderBytes + h.plan.mdatPayloadBytes);
    } else {
        AppendBE32(h.mdatHeader, static_cast<uint32_t>(h.plan.mdatHeaderBytes + h.plan.mdatPayloadBytes));
        AppendFourCC(h.mdatHeader, "mdat");
    }
    return h;
}

bool WriteAllBytes(HANDLE hFile, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - sent, 1u << 26));
        DWORD written = 0;
        if (!WriteFile(hFile, data + sent, chunk, &written, nullptr) || written != chunk) {
            return false;
        }
        sent += written;
    }
    return true;
}

}

std::vector<uint8_t> ExtractAudioSpecificConfigFromMfUserData(const uint8_t* data, size_t len) {
    if (!data || len < 14) return {};
    uint16_t payloadType = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    uint16_t structType = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
    if (payloadType != 0 || structType != 0) return {};
    return std::vector<uint8_t>(data + 12, data + len);
}

MdatOffsetPlan ComputeMdatOffsetPlan(size_t ftypBytes, size_t moovBytesIfStco, size_t moovBytesIfCo64,
                                     const std::vector<uint64_t>& chunkByteSizes) {
    MdatOffsetPlan plan;
    for (uint64_t sz : chunkByteSizes) plan.mdatPayloadBytes += sz;

    plan.useLargeMdat = (8ULL + plan.mdatPayloadBytes) > 0xFFFFFFFFULL;
    plan.mdatHeaderBytes = plan.useLargeMdat ? 16ULL : 8ULL;

    auto lastOffsetFor = [&](size_t moovBytes) -> uint64_t {
        uint64_t off = ftypBytes + moovBytes + plan.mdatHeaderBytes;
        for (size_t i = 0; i + 1 < chunkByteSizes.size(); ++i) off += chunkByteSizes[i];
        return off;
    };

    plan.useCo64 = !chunkByteSizes.empty() && lastOffsetFor(moovBytesIfStco) > 0xFFFFFFFFULL;
    size_t moovBytes = plan.useCo64 ? moovBytesIfCo64 : moovBytesIfStco;

    plan.chunkFileOffsets.reserve(chunkByteSizes.size());
    uint64_t cursor = ftypBytes + moovBytes + plan.mdatHeaderBytes;
    for (uint64_t sz : chunkByteSizes) {
        plan.chunkFileOffsets.push_back(cursor);
        cursor += sz;
    }
    return plan;
}

bool BuildMp4Bytes(const Mp4MuxInput& input, std::vector<uint8_t>& outBytes, std::string& errOut) {
    PreparedMuxData data;
    if (!PrepareMuxData(input, data, errOut)) return false;

    SerializedHeaders hdr = BuildSerializedHeaders(input, data);
    uint64_t totalSize = hdr.ftyp.size() + hdr.moov.size() + hdr.mdatHeader.size() + hdr.plan.mdatPayloadBytes;
    outBytes.clear();
    outBytes.reserve(static_cast<size_t>(totalSize));
    outBytes.insert(outBytes.end(), hdr.ftyp.begin(), hdr.ftyp.end());
    outBytes.insert(outBytes.end(), hdr.moov.begin(), hdr.moov.end());
    outBytes.insert(outBytes.end(), hdr.mdatHeader.begin(), hdr.mdatHeader.end());

    for (const auto& chunk : data.chunks) {
        if (chunk.isVideo) {
            for (uint32_t i = 0; i < chunk.sampleCount; ++i) {
                const PreparedVideoSample& vs = data.videoSamples[chunk.firstSample + i];
                for (uint32_t n = 0; n < vs.nalCount; ++n) {
                    const NalSpan& nal = data.vclNals[vs.nalFirst + n];
                    AppendBE32(outBytes, static_cast<uint32_t>(nal.size));
                    outBytes.insert(outBytes.end(), nal.data, nal.data + nal.size);
                }
            }
        } else {
            for (uint32_t i = 0; i < chunk.sampleCount; ++i) {
                const ClipPacket& ap = input.audio.packets[chunk.firstSample + i];
                const uint8_t* ptr = input.audio.bytes.data() + ap.offset;
                outBytes.insert(outBytes.end(), ptr, ptr + ap.size);
            }
        }
    }
    return true;
}

bool WriteMp4File(const std::wstring& path, const Mp4MuxInput& input, std::string& errOut) {
    PreparedMuxData data;
    if (!PrepareMuxData(input, data, errOut)) return false;

    std::error_code ec;
    std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            errOut = "create_directories failed: " + ec.message();
            return false;
        }
    }

    SerializedHeaders hdr = BuildSerializedHeaders(input, data);

    std::wstring tmpPath = path + L".tmp";
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        errOut = "CreateFileW failed: " + std::to_string(GetLastError());
        return false;
    }

    constexpr size_t kStageCap = 4u * 1024u * 1024u;
    std::vector<uint8_t> stage;
    stage.reserve(kStageCap);

    auto flushStage = [&]() -> bool {
        if (stage.empty()) return true;
        bool ok = WriteAllBytes(hFile, stage.data(), stage.size());
        stage.clear();
        return ok;
    };

    auto pushStage = [&](const uint8_t* ptr, size_t len) -> bool {
        if (len >= kStageCap) {
            if (!flushStage()) return false;
            return WriteAllBytes(hFile, ptr, len);
        }
        if (stage.size() + len > kStageCap) {
            if (!flushStage()) return false;
        }
        stage.insert(stage.end(), ptr, ptr + len);
        return true;
    };

    bool writeOk = pushStage(hdr.ftyp.data(), hdr.ftyp.size()) &&
                   pushStage(hdr.moov.data(), hdr.moov.size()) &&
                   pushStage(hdr.mdatHeader.data(), hdr.mdatHeader.size());

    for (size_t ci = 0; writeOk && ci < data.chunks.size(); ++ci) {
        const auto& chunk = data.chunks[ci];
        if (chunk.isVideo) {
            for (uint32_t i = 0; writeOk && i < chunk.sampleCount; ++i) {
                const PreparedVideoSample& vs = data.videoSamples[chunk.firstSample + i];
                for (uint32_t n = 0; writeOk && n < vs.nalCount; ++n) {
                    const NalSpan& nal = data.vclNals[vs.nalFirst + n];
                    const uint32_t sz = static_cast<uint32_t>(nal.size);
                    const uint8_t lenBe[4] = {
                        static_cast<uint8_t>(sz >> 24), static_cast<uint8_t>(sz >> 16),
                        static_cast<uint8_t>(sz >> 8), static_cast<uint8_t>(sz) };
                    writeOk = pushStage(lenBe, 4) && pushStage(nal.data, nal.size);
                }
            }
        } else {
            for (uint32_t i = 0; writeOk && i < chunk.sampleCount; ++i) {
                const ClipPacket& ap = input.audio.packets[chunk.firstSample + i];
                writeOk = pushStage(input.audio.bytes.data() + ap.offset, ap.size);
            }
        }
    }

    if (writeOk) writeOk = flushStage();
    if (!writeOk) {
        errOut = "WriteFile failed: " + std::to_string(GetLastError());
        CloseHandle(hFile);
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    CloseHandle(hFile);
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        errOut = "MoveFileExW failed: " + std::to_string(GetLastError());
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

}
