#include "audio_engine.h"
#include "clip_library.h"
#include "clock.h"
#include "config.h"
#include "mp4_muxer.h"
#include "recorder_engine.h"
#include "ring_buffer.h"
#include "video_pipeline.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "nvEncodeAPI.h"

using Microsoft::WRL::ComPtr;
using namespace clipper;

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::printf("FAIL [line %d]: %s\n", line, expr);
        g_failures++;
    }
}

#define CHECK(x) Check((x), #x, __LINE__)

uint32_t ReadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool FindBox(const uint8_t* buf, size_t len, const char type[4], size_t& outBodyOff, size_t& outBodyLen) {
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint64_t boxSz = ReadBE32(buf + pos);
        size_t hdr = 8;
        if (boxSz == 1 && pos + 16 <= len) {
            boxSz = (static_cast<uint64_t>(ReadBE32(buf + pos + 8)) << 32) | ReadBE32(buf + pos + 12);
            hdr = 16;
        }
        if (boxSz < hdr || pos + boxSz > len) return false;
        if (std::memcmp(buf + pos + 4, type, 4) == 0) {
            outBodyOff = pos + hdr;
            outBodyLen = static_cast<size_t>(boxSz - hdr);
            return true;
        }
        pos += static_cast<size_t>(boxSz);
    }
    return false;
}

bool FindBoxRecursive(const uint8_t* buf, size_t len, const char type[4], size_t& outBodyOff, size_t& outBodyLen) {
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint64_t boxSz = ReadBE32(buf + pos);
        size_t hdr = 8;
        if (boxSz == 1 && pos + 16 <= len) {
            boxSz = (static_cast<uint64_t>(ReadBE32(buf + pos + 8)) << 32) | ReadBE32(buf + pos + 12);
            hdr = 16;
        }
        if (boxSz < hdr || pos + boxSz > len) return false;
        if (std::memcmp(buf + pos + 4, type, 4) == 0) {
            outBodyOff = pos + hdr;
            outBodyLen = static_cast<size_t>(boxSz - hdr);
            return true;
        }
        const char* cc = reinterpret_cast<const char*>(buf + pos + 4);
        bool isContainer = std::memcmp(cc, "moov", 4) == 0 || std::memcmp(cc, "trak", 4) == 0 ||
                           std::memcmp(cc, "mdia", 4) == 0 || std::memcmp(cc, "minf", 4) == 0 ||
                           std::memcmp(cc, "stbl", 4) == 0 || std::memcmp(cc, "dinf", 4) == 0 ||
                           std::memcmp(cc, "edts", 4) == 0;
        if (isContainer) {
            size_t subOff = 0, subLen = 0;
            if (FindBoxRecursive(buf + pos + hdr, static_cast<size_t>(boxSz - hdr), type, subOff, subLen)) {
                outBodyOff = pos + hdr + subOff;
                outBodyLen = subLen;
                return true;
            }
        }
        if (std::memcmp(cc, "stsd", 4) == 0 && boxSz > 16) {
            size_t entryStart = pos + hdr + 8;
            size_t subOff = 0, subLen = 0;
            if (FindBoxRecursive(buf + entryStart, pos + static_cast<size_t>(boxSz) - entryStart, type, subOff, subLen)) {
                outBodyOff = entryStart + subOff;
                outBodyLen = subLen;
                return true;
            }
        }
        if (std::memcmp(cc, "avc1", 4) == 0 && boxSz > 86) {
            size_t subStart = pos + hdr + 78;
            size_t subOff = 0, subLen = 0;
            if (FindBoxRecursive(buf + subStart, pos + static_cast<size_t>(boxSz) - subStart, type, subOff, subLen)) {
                outBodyOff = subStart + subOff;
                outBodyLen = subLen;
                return true;
            }
        }
        if (std::memcmp(cc, "mp4a", 4) == 0 && boxSz > 36) {
            size_t subStart = pos + hdr + 28;
            size_t subOff = 0, subLen = 0;
            if (FindBoxRecursive(buf + subStart, pos + static_cast<size_t>(boxSz) - subStart, type, subOff, subLen)) {
                outBodyOff = subStart + subOff;
                outBodyLen = subLen;
                return true;
            }
        }
        pos += static_cast<size_t>(boxSz);
    }
    return false;
}

void TestConfig() {
    AppConfig def = MakeDefaultConfig();
    CHECK(def.monitorIndex == 0);
    CHECK(def.fps == 60);
    CHECK(def.bitrateMbps == 20);
    CHECK(def.replaySeconds == 60);
    CHECK(def.hotkeySave.display == "Ctrl+Alt+S");
    CHECK(def.recordDesktopAudio);
    CHECK(def.recordMicAudio);
    CHECK(def.saveChime);

    std::string serialized = SerializeConfig(def);
    ConfigParseResult roundTrip = ParseConfigText(serialized);
    CHECK(roundTrip.warnings.empty());
    CHECK(roundTrip.config.fps == 60);
    CHECK(roundTrip.config.bitrateMbps == 20);
    CHECK(roundTrip.config.replaySeconds == 60);
    CHECK(roundTrip.config.hotkeySave == def.hotkeySave);

    ConfigParseResult clamped = ParseConfigText(
        "fps=300\nbitrate_mbps=999\nreplay_seconds=1200\nhotkey_save=Alt+F10\n");
    CHECK(clamped.config.fps == 240);
    CHECK(clamped.config.bitrateMbps == 80);
    CHECK(clamped.config.replaySeconds == 600);
    CHECK(clamped.config.hotkeySave.display == "Alt+F10");
    CHECK(clamped.warnings.size() >= 3);

    ConfigParseResult lowFps = ParseConfigText("fps=10\nbitrate_mbps=10\nreplay_seconds=5000\n");
    CHECK(lowFps.config.fps == 15);
    CHECK(lowFps.config.replaySeconds == 3600);

    ConfigParseResult customFps = ParseConfigText("fps=120\nbitrate_mbps=20\nreplay_seconds=900\n");
    CHECK(customFps.config.fps == 120);
    CHECK(customFps.config.replaySeconds == 900);
    CHECK(customFps.warnings.empty());

    uint32_t parsedSec = 0;
    CHECK(ParseDurationInput(L"45", false, parsedSec) && parsedSec == 45);
    CHECK(ParseDurationInput(L"5", true, parsedSec) && parsedSec == 300);
    CHECK(ParseDurationInput(L"2.5", true, parsedSec) && parsedSec == 150);
    CHECK(ParseDurationInput(L"15m", false, parsedSec) && parsedSec == 900);
    CHECK(ParseDurationInput(L"90s", true, parsedSec) && parsedSec == 90);
    CHECK(ParseDurationInput(L"1:30", true, parsedSec) && parsedSec == 90);
    CHECK(ParseDurationInput(L"120m", true, parsedSec) && parsedSec == 3600);
    CHECK(!ParseDurationInput(L"0", true, parsedSec));
    CHECK(!ParseDurationInput(L"abc", false, parsedSec));
    CHECK(!ParseDurationInput(L"", false, parsedSec));

    CHECK(FormatReplayLengthShort(45) == L"45s");
    CHECK(FormatReplayLengthShort(60) == L"1m");
    CHECK(FormatReplayLengthShort(90) == L"1m 30s");
    CHECK(FormatReplayLengthShort(900) == L"15m");

    HotkeySpec hk;
    CHECK(ParseHotkeyString("ctrl+shift+f9", hk) && hk.display == "Ctrl+Shift+F9" && hk.vk == VK_F9);
    CHECK(ParseHotkeyString("F8", hk) && hk.modifiers == 0 && hk.vk == VK_F8);
    CHECK(ParseHotkeyString("Numpad0", hk) && hk.vk == VK_NUMPAD0 && hk.display == "Numpad0");
    CHECK(ParseHotkeyString("Ctrl+NumMul", hk) && hk.vk == VK_MULTIPLY && hk.modifiers == MOD_CONTROL);
    CHECK(ParseHotkeyString("Alt+Backtick", hk) && hk.vk == VK_OEM_3 && hk.modifiers == MOD_ALT);
    CHECK(!ParseHotkeyString("", hk));
    CHECK(!ParseHotkeyString("Ctrl+Alt", hk));
    CHECK(!ParseHotkeyString("Ctrl+NotAKey", hk));

    CHECK(!IsSafeHotkey(0, 'S'));
    CHECK(!IsSafeHotkey(0, '5'));
    CHECK(!IsSafeHotkey(0, VK_SPACE));
    CHECK(!IsSafeHotkey(0, VK_ESCAPE));
    CHECK(!IsSafeHotkey(MOD_CONTROL, VK_CONTROL));
    CHECK(IsSafeHotkey(MOD_CONTROL | MOD_ALT, 'S'));
    CHECK(IsSafeHotkey(0, VK_F9));
    CHECK(IsSafeHotkey(0, VK_INSERT));
    CHECK(IsSafeHotkey(0, VK_ADD));

    HotkeySpec formatted;
    CHECK(!FormatHotkeyFromVk(0, 'S', formatted));
    CHECK(FormatHotkeyFromVk(MOD_CONTROL | MOD_SHIFT, VK_F10, formatted) &&
          formatted.display == "Ctrl+Shift+F10");
    CHECK(FormatHotkeyFromVk(0, VK_NUMPAD3, formatted) && formatted.display == "Numpad3");
    CHECK(FormatHotkeyFromVk(MOD_ALT, VK_OEM_3, formatted) && formatted.display == "Alt+Backtick");

    AppConfig valCfg = MakeDefaultConfig();
    valCfg.fps = 300;
    valCfg.bitrateMbps = 80;
    valCfg.replaySeconds = 1200;
    std::vector<std::string> valWarns;
    ValidateAppConfig(valCfg, valWarns);
    CHECK(valCfg.fps == 240);
    CHECK(valCfg.replaySeconds == 600);
    CHECK(valWarns.size() >= 2);

    wchar_t arg0[] = L"clipper.exe";
    wchar_t arg1[] = L"--fps=30";
    wchar_t arg2[] = L"--bitrate=10";
    wchar_t arg3[] = L"--no-mic-audio";
    wchar_t arg4[] = L"--hotkey=F8";
    wchar_t* argv[] = { arg0, arg1, arg2, arg3, arg4 };
    ConfigParseResult cli = ApplyCliOverrides(5, argv, MakeDefaultConfig());
    CHECK(cli.config.fps == 30);
    CHECK(cli.config.bitrateMbps == 10);
    CHECK(!cli.config.recordMicAudio);
    CHECK(cli.config.hotkeySave.display == "F8");
    CHECK(def.VideoArenaBytes() > 200'000'000ULL);
    std::printf("PASS TestConfig\n");
}

void TestRingBuffer() {
    {
        PacketRingBuffer rb(100, 16);
        std::vector<uint8_t> p1(40, 0x11);
        std::vector<uint8_t> p2(40, 0x22);
        std::vector<uint8_t> p3(35, 0x33);
        CHECK(rb.Push(p1.data(), 40, 0, 16667, true));
        CHECK(rb.Push(p2.data(), 40, 16667, 16667, true));
        CHECK(rb.Push(p3.data(), 35, 33334, 16667, true));

        CHECK(rb.PacketCount() == 2);
        ClipStreamSnapshot snap = rb.SnapshotVideoWindow(100'000);
        CHECK(snap.packets.size() == 2);
        CHECK(snap.packets[0].size == 40);
        CHECK(snap.packets[1].size == 35);
        CHECK(std::all_of(snap.bytes.begin(), snap.bytes.begin() + 40, [](uint8_t v) { return v == 0x22; }));
        CHECK(std::all_of(snap.bytes.begin() + 40, snap.bytes.end(), [](uint8_t v) { return v == 0x33; }));

        PacketRingBuffer rbSkip(100, 16);
        CHECK(rbSkip.Push(p1.data(), 40, 0, 16667, false));
        CHECK(rbSkip.Push(p2.data(), 40, 16667, 16667, true));
        ClipStreamSnapshot skipSnap = rbSkip.SnapshotVideoWindow(100'000);
        CHECK(skipSnap.packets.size() == 1);
        CHECK(std::all_of(skipSnap.bytes.begin(), skipSnap.bytes.end(), [](uint8_t v) { return v == 0x22; }));

        std::vector<uint8_t> huge(101, 0x44);
        CHECK(!rb.Push(huge.data(), 101, 50000, 16667, true));
        CHECK(!rb.Push(p1.data(), 0, 50000, 16667, true));
        CHECK(rb.DroppedOversizeCount() == 2);
        CHECK(rb.PacketCount() == 2);
    }

    {
        PacketRingBuffer rb(65536, 256);
        uint8_t b = 1;
        for (int i = 0; i < 30; ++i) {
            CHECK(rb.Push(&b, 1, i * 16667LL, 16667, (i % 10) == 0));
        }
        rb.AdvanceGeneration();
        uint8_t b2 = 2;
        for (int i = 0; i < 15; ++i) {
            CHECK(rb.Push(&b2, 1, (30 + i) * 16667LL, 16667, i == 0));
        }
        ClipStreamSnapshot snap = rb.SnapshotVideoWindow(60'000'000LL);
        CHECK(snap.packets.size() == 15);
        CHECK(std::all_of(snap.bytes.begin(), snap.bytes.end(), [](uint8_t v) { return v == 2; }));
    }

    {
        PacketRingBuffer rb(65536, 256);
        uint8_t b = 7;
        for (int i = 0; i < 120; ++i) {
            CHECK(rb.Push(&b, 1, i * 1'000'000LL, 1'000'000, (i % 10) == 0));
        }
        ClipStreamSnapshot snap = rb.SnapshotVideoWindow(25'000'000LL);
        CHECK(!snap.Empty());
        CHECK(snap.packets.front().isKeyFrame);
        CHECK(snap.packets.front().ptsUs == 90'000'000LL);
        CHECK(snap.packets.back().ptsUs == 119'000'000LL);
    }

    {
        PacketRingBuffer rb(65536, 256);
        uint8_t b = 9;
        for (int i = 0; i < 10; ++i) CHECK(rb.Push(&b, 1, i * 16667LL, 16667, false));
        CHECK(rb.SnapshotVideoWindow(10'000'000LL).Empty());
    }

    {
        PacketRingBuffer rb(128 * 1024, 2000);
        std::atomic<bool> done{ false };
        std::thread writer([&]() {
            std::vector<uint8_t> pkt(900);
            for (uint32_t i = 0; i < 10000; ++i) {
                uint32_t sz = 50 + (i * 73) % 850;
                uint8_t tag = static_cast<uint8_t>(i & 0xFF);
                std::fill_n(pkt.begin(), sz, tag);
                pkt[0] = static_cast<uint8_t>(i & 0xFF);
                pkt[sz - 1] = static_cast<uint8_t>((i >> 8) & 0xFF);
                rb.Push(pkt.data(), sz, static_cast<int64_t>(i) * 16667LL, 16667, (i % 30) == 0);
            }
            done.store(true);
        });

        int snapshotsChecked = 0;
        while (!done.load() || snapshotsChecked < 50) {
            ClipStreamSnapshot snap = rb.SnapshotVideoWindow(1'000'000LL);
            if (!snap.Empty()) {
                CHECK(snap.packets.front().isKeyFrame);
                for (size_t i = 0; i < snap.packets.size(); ++i) {
                    const auto& p = snap.packets[i];
                    if (i > 0) CHECK(p.ptsUs > snap.packets[i - 1].ptsUs);
                    uint8_t expectedTag = snap.bytes[p.offset];
                    for (uint32_t j = 1; j + 1 < p.size; ++j) {
                        if (snap.bytes[p.offset + j] != expectedTag) {
                            CHECK(false);
                            break;
                        }
                    }
                    CHECK(p.size >= 2);
                }
                snapshotsChecked++;
            }
            if (done.load() && snapshotsChecked >= 50) break;
        }
        writer.join();
        CHECK(snapshotsChecked >= 50);
    }

    {
        PacketRingBuffer arb(2048, 32);
        const uint8_t dOld[] = { 1, 2, 3, 4 };
        const uint8_t dA[] = { 5, 6, 7 };
        const uint8_t dOut[] = { 8, 9, 10, 11, 12 };
        const uint8_t dB[] = { 13, 14, 15, 16 };
        arb.Push(dOld, sizeof(dOld), 100'000, 21333, true);
        arb.AdvanceGeneration();
        arb.Push(dA, sizeof(dA), 85'000, 21333, true);
        arb.Push(dOut, sizeof(dOut), 1'500'000, 21333, true);
        arb.Push(dB, sizeof(dB), 106'333, 21333, true);

        ClipStreamSnapshot s = arb.SnapshotAudioRange(100'000, 200'000);
        CHECK(s.generation == 2);
        CHECK(s.packets.size() == 2);
        CHECK(s.bytes.size() == sizeof(dA) + sizeof(dB));
        CHECK(s.packets[0].ptsUs == 85'000 && s.packets[1].ptsUs == 106'333);

        arb.Clear();
        CHECK(arb.PacketCount() == 0);
        CHECK(arb.UsedPayloadBytes() == 0);
    }

    std::printf("PASS TestRingBuffer\n");
}

void TestMp4StructureAndCo64() {
    const uint8_t sps[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x2A, 0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0xC0 };
    const uint8_t pps[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0 };
    const uint8_t aud[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xF0 };
    const uint8_t idr[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xAA };
    const uint8_t pfr[] = { 0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x22, 0x33 };

    Mp4MuxInput in;
    in.width = 1920;
    in.height = 1080;
    for (int f = 0; f < 90; ++f) {
        bool kf = (f % 60 == 0);
        uint32_t off = static_cast<uint32_t>(in.video.bytes.size());
        in.video.bytes.insert(in.video.bytes.end(), aud, aud + sizeof(aud));
        if (kf) {
            in.video.bytes.insert(in.video.bytes.end(), sps, sps + sizeof(sps));
            in.video.bytes.insert(in.video.bytes.end(), pps, pps + sizeof(pps));
            in.video.bytes.insert(in.video.bytes.end(), idr, idr + sizeof(idr));
        } else {
            in.video.bytes.insert(in.video.bytes.end(), pfr, pfr + sizeof(pfr));
        }
        uint32_t sz = static_cast<uint32_t>(in.video.bytes.size() - off);
        in.video.packets.push_back(ClipPacket{ off, sz, f * 16667LL, 16667, kf });
    }

    const uint8_t aacDummy[] = { 0x21, 0x10, 0x04, 0x60, 0x8C, 0x1C };
    for (int a = 0; a < 70; ++a) {
        uint32_t off = static_cast<uint32_t>(in.audio.bytes.size());
        in.audio.bytes.insert(in.audio.bytes.end(), aacDummy, aacDummy + sizeof(aacDummy));
        in.audio.packets.push_back(ClipPacket{ off, sizeof(aacDummy), a * 21333LL, 21333, true });
    }

    std::vector<uint8_t> mp4;
    std::string err;
    CHECK(BuildMp4Bytes(in, mp4, err));
    CHECK(err.empty());

    size_t ftypOff = 0, ftypLen = 0, moovOff = 0, moovLen = 0, mdatOff = 0, mdatLen = 0;
    CHECK(FindBox(mp4.data(), mp4.size(), "ftyp", ftypOff, ftypLen));
    CHECK(FindBox(mp4.data(), mp4.size(), "moov", moovOff, moovLen));
    CHECK(FindBox(mp4.data(), mp4.size(), "mdat", mdatOff, mdatLen));
    CHECK(ftypOff < moovOff && moovOff < mdatOff);

    size_t avccOff = 0, avccLen = 0;
    CHECK(FindBoxRecursive(mp4.data() + moovOff, moovLen, "avcC", avccOff, avccLen));
    CHECK(avccLen >= 10);
    const uint8_t* avccPtr = mp4.data() + moovOff + avccOff;
    CHECK(avccPtr[0] == 1 && avccPtr[1] == 100);
    CHECK(avccPtr[avccLen - 1] == 0x00 && (avccPtr[avccLen - 4] & 0xFC) == 0xFC);

    size_t colrOff = 0, colrLen = 0;
    CHECK(FindBoxRecursive(mp4.data() + moovOff, moovLen, "colr", colrOff, colrLen));
    CHECK(colrLen == 11);
    const uint8_t* colrPtr = mp4.data() + moovOff + colrOff;
    CHECK(std::memcmp(colrPtr, "nclx", 4) == 0);

    size_t stcoOff = 0, stcoLen = 0;
    CHECK(FindBoxRecursive(mp4.data() + moovOff, moovLen, "stco", stcoOff, stcoLen));
    uint32_t vChunkCount = ReadBE32(mp4.data() + moovOff + stcoOff + 4);
    CHECK(vChunkCount == 2);

    size_t stszOff = 0, stszLen = 0;
    CHECK(FindBoxRecursive(mp4.data() + moovOff, moovLen, "stsz", stszOff, stszLen));
    uint32_t vSampleCount = ReadBE32(mp4.data() + moovOff + stszOff + 8);
    CHECK(vSampleCount == 90);

    uint32_t chunk0Off = ReadBE32(mp4.data() + moovOff + stcoOff + 8);
    size_t cursor = chunk0Off;
    for (uint32_t s = 0; s < 60; ++s) {
        uint32_t sampleSz = ReadBE32(mp4.data() + moovOff + stszOff + 12 + s * 4);
        size_t pos = 0;
        while (pos + 4 <= sampleSz) {
            uint32_t nalLen = ReadBE32(mp4.data() + cursor + pos);
            uint8_t nalType = mp4[cursor + pos + 4] & 0x1Fu;
            CHECK(nalType != 7 && nalType != 8 && nalType != 9);
            pos += 4 + nalLen;
        }
        CHECK(pos == sampleSz);
        cursor += sampleSz;
    }

    uint8_t fakeUserData[14] = { 0, 0, 0x29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x90 };
    auto asc = ExtractAudioSpecificConfigFromMfUserData(fakeUserData, sizeof(fakeUserData));
    CHECK(asc.size() == 2 && asc[0] == 0x11 && asc[1] == 0x90);

    uint8_t badUserData[14] = { 1, 0, 0x29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x90 };
    CHECK(ExtractAudioSpecificConfigFromMfUserData(nullptr, 0).empty());
    CHECK(ExtractAudioSpecificConfigFromMfUserData(fakeUserData, 10).empty());
    CHECK(ExtractAudioSpecificConfigFromMfUserData(badUserData, sizeof(badUserData)).empty());

    size_t trak1Off = 0, trak1Len = 0, trak2Off = 0, trak2Len = 0;
    CHECK(FindBox(mp4.data() + moovOff, moovLen, "trak", trak1Off, trak1Len));
    size_t scan = 0;
    int trakSeen = 0;
    while (scan + 8 <= moovLen) {
        uint32_t bsz = ReadBE32(mp4.data() + moovOff + scan);
        if (bsz < 8 || scan + bsz > moovLen) break;
        if (std::memcmp(mp4.data() + moovOff + scan + 4, "trak", 4) == 0) {
            trakSeen++;
            if (trakSeen == 2) {
                trak2Off = scan + 8;
                trak2Len = bsz - 8;
            }
        }
        scan += bsz;
    }
    CHECK(trakSeen == 2);
    size_t mp4aOff = 0, mp4aLen = 0, esdsOff = 0, esdsLen = 0, aStszOff = 0, aStszLen = 0;
    CHECK(FindBoxRecursive(mp4.data() + moovOff + trak2Off, trak2Len, "mp4a", mp4aOff, mp4aLen));
    CHECK(FindBoxRecursive(mp4.data() + moovOff + trak2Off, trak2Len, "esds", esdsOff, esdsLen));
    CHECK(FindBoxRecursive(mp4.data() + moovOff + trak2Off, trak2Len, "stsz", aStszOff, aStszLen));
    CHECK(ReadBE32(mp4.data() + moovOff + trak2Off + aStszOff + 8) == 70);

    {
        Mp4MuxInput delayed = in;
        for (auto& ap : delayed.audio.packets) ap.ptsUs += 30'000;
        std::vector<uint8_t> dmp4;
        std::string derr;
        CHECK(BuildMp4Bytes(delayed, dmp4, derr));
        size_t dmOff = 0, dmLen = 0, elstOff = 0, elstLen = 0;
        CHECK(FindBox(dmp4.data(), dmp4.size(), "moov", dmOff, dmLen));
        CHECK(FindBoxRecursive(dmp4.data() + dmOff, dmLen, "elst", elstOff, elstLen));
        CHECK(ReadBE32(dmp4.data() + dmOff + elstOff + 4) == 2);
    }

    std::vector<uint64_t> hugeChunks = { 2ULL << 30, 2ULL << 30, 1ULL << 30 };
    MdatOffsetPlan plan = ComputeMdatOffsetPlan(32, 500, 512, hugeChunks);
    CHECK(plan.useCo64);
    CHECK(plan.useLargeMdat);
    CHECK(plan.mdatHeaderBytes == 16);
    CHECK(plan.chunkFileOffsets.size() == 3);
    CHECK(plan.chunkFileOffsets[2] > 0xFFFFFFFFULL);

    std::printf("PASS TestMp4StructureAndCo64\n");
}

void TestRecorderComponentsAndLifecycle() {
    AppConfig cfg = MakeDefaultConfig();
    RecorderEngine eng(cfg);
    CHECK(eng.State() == EngineState::Stopped);
    SaveResult stoppedSave = eng.SaveClipSync(L"build\\test_out\\should_not_exist.mp4");
    CHECK(!stoppedSave.ok && !stoppedSave.error.empty());
    eng.UpdateSaveSettings(L"C:\\Temp\\ClipperOut", false);
    CHECK(eng.Config().outputDir == L"C:\\Temp\\ClipperOut");
    CHECK(!eng.Config().saveChime);
    eng.Stop();
    CHECK(eng.State() == EngineState::Stopped);

    auto monitors = EnumerateCaptureMonitors();
    CHECK(!monitors.empty());
    if (!monitors.empty()) {
        CHECK(!monitors[0].deviceName.empty());
        CHECK(monitors[0].width > 0 && monitors[0].height > 0);
        CHECK(monitors[0].refreshHz > 0);
    }

    AppConfig badMonitor = cfg;
    badMonitor.monitorIndex = 99;
    PacketRingBuffer vRing(4 * 1024 * 1024, 64);
    VideoPipeline vp;
    std::string vErr;
    CHECK(!vp.Start(badMonitor, NowQpcTicks(), vRing, vErr));
    CHECK(!vErr.empty());
    vp.Stop();

    PacketRingBuffer aRing(2 * 1024 * 1024, 256);
    AudioEngine ae;
    std::string aErr;
    if (ae.Start(cfg, NowQpcTicks(), aRing, aErr)) {
        Sleep(250);
        ae.SetMicEnabled(false);
        CHECK(!ae.IsMicEnabled());
        ae.Stop();
        auto asc = ae.AudioSpecificConfig();
        auto ast = ae.Stats();
        CHECK(asc.size() == 2);
        CHECK(ast.aacFramesEncoded >= 6);
        CHECK(ast.errors == 0);
        CHECK(aRing.PacketCount() >= 6);
    } else {
        std::printf("AudioEngine skipped in environment: %s\n", aErr.c_str());
    }

    std::printf("PASS TestRecorderComponentsAndLifecycle\n");
}

void TestEndToEndNvencAacAndSourceReader() {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1,
        D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) return;

    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoContext1> vctx1;
    CHECK(SUCCEEDED(dev.As(&vdev)));
    CHECK(SUCCEEDED(ctx.As(&vctx)));
    CHECK(SUCCEEDED(ctx.As(&vctx1)));

    constexpr uint32_t W = 1920, H = 1080, FPS = 60, NFRAMES = 60;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = W; cd.InputHeight = H;
    cd.OutputWidth = W; cd.OutputHeight = H;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    CHECK(SUCCEEDED(vdev->CreateVideoProcessorEnumerator(&cd, &vpEnum)));
    CHECK(SUCCEEDED(vdev->CreateVideoProcessor(vpEnum.Get(), 0, &vp)));
    vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> bgraTex;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &bgraTex)));

    D3D11_TEXTURE2D_DESC tdNv12 = td;
    tdNv12.Format = DXGI_FORMAT_NV12;
    ComPtr<ID3D11Texture2D> nv12Tex;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&tdNv12, nullptr, &nv12Tex)));

    ComPtr<ID3D11VideoProcessorInputView> piv;
    ComPtr<ID3D11VideoProcessorOutputView> pov;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    CHECK(SUCCEEDED(vdev->CreateVideoProcessorInputView(bgraTex.Get(), vpEnum.Get(), &ivd, &piv)));
    CHECK(SUCCEEDED(vdev->CreateVideoProcessorOutputView(nv12Tex.Get(), vpEnum.Get(), &ovd, &pov)));

    HMODULE nvDll = LoadLibraryW(L"nvEncodeAPI64.dll");
    CHECK(nvDll != nullptr);
    if (!nvDll) return;

    using PFN_Create = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto pCreate = reinterpret_cast<PFN_Create>(GetProcAddress(nvDll, "NvEncodeAPICreateInstance"));
    NV_ENCODE_API_FUNCTION_LIST nv = {};
    nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    CHECK(pCreate(&nv) == NV_ENC_SUCCESS);

    void* encoder = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {};
    op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.device = dev.Get();
    op.apiVersion = NVENCAPI_VERSION;
    CHECK(nv.nvEncOpenEncodeSessionEx(&op, &encoder) == NV_ENC_SUCCESS);

    NV_ENC_PRESET_CONFIG pcfg = {};
    pcfg.version = NV_ENC_PRESET_CONFIG_VER;
    pcfg.presetCfg.version = NV_ENC_CONFIG_VER;
    CHECK(nv.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
            NV_ENC_TUNING_INFO_LOW_LATENCY, &pcfg) == NV_ENC_SUCCESS);

    NV_ENC_CONFIG cfg = pcfg.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.gopLength = FPS;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    cfg.rcParams.averageBitRate = 20000000;
    cfg.rcParams.maxBitRate = 27000000;
    cfg.rcParams.vbvBufferSize = 27000000 / 2;
    cfg.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    cfg.rcParams.enableLookahead = 0;
    cfg.rcParams.enableAQ = 0;
    cfg.rcParams.enableTemporalAQ = 0;
    cfg.rcParams.zeroReorderDelay = 1;
    cfg.encodeCodecConfig.h264Config.idrPeriod = FPS;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
    cfg.encodeCodecConfig.h264Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    cfg.encodeCodecConfig.h264Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    auto& vui = cfg.encodeCodecConfig.h264Config.h264VUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

    NV_ENC_INITIALIZE_PARAMS ip = {};
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P4_GUID;
    ip.encodeWidth = W; ip.encodeHeight = H;
    ip.darWidth = W; ip.darHeight = H;
    ip.frameRateNum = FPS; ip.frameRateDen = 1;
    ip.enableEncodeAsync = 1;
    ip.enablePTD = 1;
    ip.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    ip.encodeConfig = &cfg;
    CHECK(nv.nvEncInitializeEncoder(encoder, &ip) == NV_ENC_SUCCESS);

    NV_ENC_REGISTER_RESOURCE rr = {};
    rr.version = NV_ENC_REGISTER_RESOURCE_VER;
    rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    rr.width = W; rr.height = H;
    rr.resourceToRegister = nv12Tex.Get();
    rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    rr.bufferUsage = NV_ENC_INPUT_IMAGE;
    CHECK(nv.nvEncRegisterResource(encoder, &rr) == NV_ENC_SUCCESS);

    NV_ENC_CREATE_BITSTREAM_BUFFER cbb = {};
    cbb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    CHECK(nv.nvEncCreateBitstreamBuffer(encoder, &cbb) == NV_ENC_SUCCESS);

    HANDLE encEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    NV_ENC_EVENT_PARAMS ep = {};
    ep.version = NV_ENC_EVENT_PARAMS_VER;
    ep.completionEvent = encEv;
    CHECK(nv.nvEncRegisterAsyncEvent(encoder, &ep) == NV_ENC_SUCCESS);

    PacketRingBuffer videoRing(8 * 1024 * 1024, 128);
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(SUCCEEDED(dev->CreateRenderTargetView(bgraTex.Get(), nullptr, &rtv)));

    for (uint32_t f = 0; f < NFRAMES; ++f) {
        float c[4] = { (f % 30) / 30.0f, 0.25f, 1.0f - (f % 30) / 30.0f, 1.0f };
        ctx->ClearRenderTargetView(rtv.Get(), c);
        D3D11_VIDEO_PROCESSOR_STREAM strm = {};
        strm.Enable = TRUE;
        strm.pInputSurface = piv.Get();
        CHECK(SUCCEEDED(vctx->VideoProcessorBlt(vp.Get(), pov.Get(), 0, 1, &strm)));

        NV_ENC_MAP_INPUT_RESOURCE mir = {};
        mir.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mir.registeredResource = rr.registeredResource;
        CHECK(nv.nvEncMapInputResource(encoder, &mir) == NV_ENC_SUCCESS);

        NV_ENC_PIC_PARAMS pic = {};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = W; pic.inputHeight = H; pic.inputPitch = W;
        pic.frameIdx = f;
        pic.inputTimeStamp = static_cast<uint64_t>(f) * 16667ULL;
        pic.inputDuration = 16667ULL;
        pic.inputBuffer = mir.mappedResource;
        pic.outputBitstream = cbb.bitstreamBuffer;
        pic.completionEvent = encEv;
        pic.bufferFmt = mir.mappedBufferFmt;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        if (f == 0) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;

        CHECK(nv.nvEncEncodePicture(encoder, &pic) == NV_ENC_SUCCESS);
        CHECK(WaitForSingleObject(encEv, 2000) == WAIT_OBJECT_0);
        NV_ENC_LOCK_BITSTREAM lb = {};
        lb.version = NV_ENC_LOCK_BITSTREAM_VER;
        lb.outputBitstream = cbb.bitstreamBuffer;
        CHECK(nv.nvEncLockBitstream(encoder, &lb) == NV_ENC_SUCCESS);
        bool isIdr = (lb.pictureType == NV_ENC_PIC_TYPE_IDR);
        CHECK(videoRing.Push(static_cast<const uint8_t*>(lb.bitstreamBufferPtr),
            lb.bitstreamSizeInBytes, static_cast<int64_t>(f) * 16667LL, 16667, isIdr));
        nv.nvEncUnlockBitstream(encoder, cbb.bitstreamBuffer);
        nv.nvEncUnmapInputResource(encoder, mir.mappedResource);
    }

    nv.nvEncUnregisterAsyncEvent(encoder, &ep);
    CloseHandle(encEv);
    nv.nvEncDestroyBitstreamBuffer(encoder, cbb.bitstreamBuffer);
    nv.nvEncUnregisterResource(encoder, rr.registeredResource);
    nv.nvEncDestroyEncoder(encoder);
    FreeLibrary(nvDll);

    ComPtr<IMFTransform> aacMft;
    CHECK(SUCCEEDED(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&aacMft))));
    ComPtr<IMFMediaType> inType, outType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);

    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);

    CHECK(SUCCEEDED(aacMft->SetInputType(0, inType.Get(), 0)));
    CHECK(SUCCEEDED(aacMft->SetOutputType(0, outType.Get(), 0)));
    CHECK(SUCCEEDED(aacMft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)));
    CHECK(SUCCEEDED(aacMft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)));

    ComPtr<IMFMediaType> actualOut;
    CHECK(SUCCEEDED(aacMft->GetOutputCurrentType(0, &actualOut)));
    std::vector<uint8_t> userData(64);
    UINT32 udLen = 0;
    std::vector<uint8_t> asc = { 0x11, 0x90 };
    if (SUCCEEDED(actualOut->GetBlob(MF_MT_USER_DATA, userData.data(), static_cast<UINT32>(userData.size()), &udLen))) {
        asc = ExtractAudioSpecificConfigFromMfUserData(userData.data(), udLen);
        std::printf("MF AAC userData bytes=%u asc=%02X %02X\n", udLen, asc[0], asc.size() > 1 ? asc[1] : 0);
    }

    constexpr uint32_t kSamplesTotal = 48000 + 2048;
    std::vector<int16_t> pcm(kSamplesTotal * 2);
    for (uint32_t i = 0; i < kSamplesTotal; ++i) {
        double v = std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / 48000.0) * 16000.0;
        int16_t s = static_cast<int16_t>(v);
        pcm[i * 2] = s;
        pcm[i * 2 + 1] = s;
    }

    PacketRingBuffer audioRing(1024 * 1024, 128);
    int64_t aacPtsUs = 0;
    auto drainAac = [&]() {
        MFT_OUTPUT_STREAM_INFO osi = {};
        aacMft->GetOutputStreamInfo(0, &osi);
        DWORD outBufSz = std::max<DWORD>(osi.cbSize, 4096);
        while (true) {
            ComPtr<IMFSample> outSample;
            ComPtr<IMFMediaBuffer> outBuf;
            MFCreateSample(&outSample);
            MFCreateMemoryBuffer(outBufSz, &outBuf);
            outSample->AddBuffer(outBuf.Get());
            MFT_OUTPUT_DATA_BUFFER odb = {};
            odb.pSample = outSample.Get();
            DWORD status = 0;
            HRESULT ohr = aacMft->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) odb.pEvents->Release();
            if (ohr == MF_E_TRANSFORM_NEED_MORE_INPUT || ohr == MF_E_TRANSFORM_STREAM_CHANGE) break;
            if (FAILED(ohr)) break;
            ComPtr<IMFMediaBuffer> locked;
            outSample->ConvertToContiguousBuffer(&locked);
            BYTE* ptr = nullptr;
            DWORD len = 0;
            locked->Lock(&ptr, nullptr, &len);
            if (len > 0) {
                audioRing.Push(ptr, len, aacPtsUs, 21333, true);
                aacPtsUs += 21333;
            }
            locked->Unlock();
        }
    };

    for (uint32_t off = 0; off < kSamplesTotal; off += 1024) {
        uint32_t cnt = std::min<uint32_t>(1024, kSamplesTotal - off);
        ComPtr<IMFSample> inSample;
        ComPtr<IMFMediaBuffer> inBuf;
        DWORD byteLen = cnt * 4;
        MFCreateSample(&inSample);
        MFCreateMemoryBuffer(byteLen, &inBuf);
        BYTE* dst = nullptr;
        inBuf->Lock(&dst, nullptr, nullptr);
        std::memcpy(dst, pcm.data() + off * 2, byteLen);
        inBuf->Unlock();
        inBuf->SetCurrentLength(byteLen);
        inSample->AddBuffer(inBuf.Get());
        LONGLONG sampleTimeHns = (static_cast<LONGLONG>(off) * 10'000'000LL) / 48000LL;
        LONGLONG sampleDurHns = (static_cast<LONGLONG>(cnt) * 10'000'000LL) / 48000LL;
        inSample->SetSampleTime(sampleTimeHns);
        inSample->SetSampleDuration(sampleDurHns);

        HRESULT phr = aacMft->ProcessInput(0, inSample.Get(), 0);
        if (phr == MF_E_NOTACCEPTING) {
            drainAac();
            phr = aacMft->ProcessInput(0, inSample.Get(), 0);
        }
        if (FAILED(phr)) {
            std::printf("aacMft->ProcessInput failed HR=0x%08lX\n", static_cast<unsigned long>(phr));
            CHECK(false);
            break;
        }
        drainAac();
    }
    aacMft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    drainAac();

    Mp4MuxInput muxIn;
    muxIn.width = W;
    muxIn.height = H;
    muxIn.video = videoRing.SnapshotVideoWindow(10'000'000LL);
    muxIn.audio = audioRing.SnapshotAudioRange(muxIn.video.startPtsUs, muxIn.video.endPtsUs);
    muxIn.audioSpecificConfig = asc;
    CHECK(!muxIn.video.Empty());
    CHECK(!muxIn.audio.Empty());
    if (muxIn.video.Empty() || muxIn.audio.Empty()) return;

    std::filesystem::path outDir = std::filesystem::current_path() / "build" / "test_out";
    std::filesystem::path outPath = outDir / "e2e_h264_aac.mp4";
    std::string muxErr;
    CHECK(WriteMp4File(outPath.wstring(), muxIn, muxErr));
    CHECK(muxErr.empty());
    CHECK(std::filesystem::exists(outPath));
    CHECK(!std::filesystem::exists(outPath.wstring() + L".tmp"));

    ComPtr<IMFSourceReader> reader;
    CHECK(SUCCEEDED(MFCreateSourceReaderFromURL(outPath.wstring().c_str(), nullptr, &reader)));
    if (!reader) return;

    PROPVARIANT varDur;
    PropVariantInit(&varDur);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &varDur))) {
        double durSec = static_cast<double>(varDur.uhVal.QuadPart) / 10'000'000.0;
        std::printf("IMFSourceReader duration=%.3fs\n", durSec);
        CHECK(durSec >= 0.90 && durSec <= 1.15);
    }
    PropVariantClear(&varDur);

    ComPtr<IMFMediaType> nat0, nat1;
    HRESULT hrNat0 = reader->GetNativeMediaType(0, 0, &nat0);
    HRESULT hrNat1 = reader->GetNativeMediaType(1, 0, &nat1);
    CHECK(SUCCEEDED(hrNat0));
    CHECK(SUCCEEDED(hrNat1));
    if (FAILED(hrNat0) || FAILED(hrNat1)) return;

    GUID maj0 = {}, maj1 = {}, sub0 = {}, sub1 = {};
    nat0->GetGUID(MF_MT_MAJOR_TYPE, &maj0);
    nat0->GetGUID(MF_MT_SUBTYPE, &sub0);
    nat1->GetGUID(MF_MT_MAJOR_TYPE, &maj1);
    nat1->GetGUID(MF_MT_SUBTYPE, &sub1);
    std::printf("Stream0 major=%08lX sub=%08lX | Stream1 major=%08lX sub=%08lX\n",
        maj0.Data1, sub0.Data1, maj1.Data1, sub1.Data1);

    DWORD videoStreamIdx = (maj0 == MFMediaType_Video) ? 0u : (maj1 == MFMediaType_Video ? 1u : UINT32_MAX);
    DWORD audioStreamIdx = (maj0 == MFMediaType_Audio) ? 0u : (maj1 == MFMediaType_Audio ? 1u : UINT32_MAX);
    CHECK(videoStreamIdx != UINT32_MAX);
    CHECK(audioStreamIdx != UINT32_MAX);
    if (videoStreamIdx == UINT32_MAX || audioStreamIdx == UINT32_MAX) return;

    ComPtr<IMFMediaType> natV = (videoStreamIdx == 0) ? nat0 : nat1;
    UINT32 rw = 0, rh = 0;
    MFGetAttributeSize(natV.Get(), MF_MT_FRAME_SIZE, &rw, &rh);
    std::printf("Native Video size=%ux%u\n", rw, rh);
    CHECK(rw == W && rh == H);

    ComPtr<IMFMediaType> decV, decA;
    MFCreateMediaType(&decV);
    decV->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decV->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    HRESULT hrSetV = reader->SetCurrentMediaType(videoStreamIdx, nullptr, decV.Get());
    if (FAILED(hrSetV)) std::printf("SetCurrentMediaType(video) HR=0x%08lX\n", static_cast<unsigned long>(hrSetV));
    CHECK(SUCCEEDED(hrSetV));

    MFCreateMediaType(&decA);
    decA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    decA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    HRESULT hrSetA = reader->SetCurrentMediaType(audioStreamIdx, nullptr, decA.Get());
    if (FAILED(hrSetA)) std::printf("SetCurrentMediaType(audio) HR=0x%08lX\n", static_cast<unsigned long>(hrSetA));
    CHECK(SUCCEEDED(hrSetA));

    int decodedVideoFrames = 0;
    bool videoHasColorVariance = false;
    size_t decodedPcmBytes = 0;
    double pcmSumSq = 0.0;
    size_t pcmSamples = 0;

    bool vDone = false, aDone = false;
    int reads = 0;
    constexpr int kMaxReads = 5000;
    while ((!vDone || !aDone) && reads < kMaxReads) {
        reads++;
        DWORD streamIdx = 0, streamFlags = 0;
        ComPtr<IMFSample> sample;
        HRESULT rhr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &streamIdx, &streamFlags, nullptr, &sample);
        CHECK(SUCCEEDED(rhr));
        if (FAILED(rhr)) break;
        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (streamIdx == videoStreamIdx) vDone = true;
            if (streamIdx == audioStreamIdx) aDone = true;
            continue;
        }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* ptr = nullptr;
        DWORD len = 0;
        buf->Lock(&ptr, nullptr, &len);
        if (streamIdx == videoStreamIdx) {
            decodedVideoFrames++;
            if (len >= W) {
                uint8_t mn = 255, mx = 0;
                for (DWORD k = 0; k < len; k += 4099) {
                    mn = std::min(mn, ptr[k]);
                    mx = std::max(mx, ptr[k]);
                }
                if (mx > mn) videoHasColorVariance = true;
            }
        } else if (streamIdx == audioStreamIdx) {
            decodedPcmBytes += len;
            const int16_t* sp = reinterpret_cast<const int16_t*>(ptr);
            size_t n = len / sizeof(int16_t);
            for (size_t k = 0; k < n; ++k) {
                pcmSumSq += static_cast<double>(sp[k]) * static_cast<double>(sp[k]);
            }
            pcmSamples += n;
        }
        buf->Unlock();
    }
    CHECK(reads < kMaxReads);

    double rms = pcmSamples > 0 ? std::sqrt(pcmSumSq / static_cast<double>(pcmSamples)) : 0.0;
    std::printf("IMFSourceReader decoded: videoFrames=%d (variance=%d), pcmBytes=%zu, rms=%.1f\n",
        decodedVideoFrames, videoHasColorVariance ? 1 : 0, decodedPcmBytes, rms);

    CHECK(decodedVideoFrames >= 58);
    CHECK(videoHasColorVariance);
    CHECK(decodedPcmBytes >= 160000);
    CHECK(rms > 1000.0);
    std::printf("PASS TestEndToEndNvencAacAndSourceReader\n");
}

void TestClipLibrary() {
    CHECK(FormatBytesHuman(0) == L"0 B");
    CHECK(FormatBytesHuman(512) == L"512 B");
    CHECK(FormatBytesHuman(1536) == L"1.5 KB");
    CHECK(FormatBytesHuman(19293798ULL) == L"18.4 MB");
    CHECK(FormatBytesHuman(1342177280ULL) == L"1.25 GB");

    CHECK(FormatDurationShort(0.0).empty());
    CHECK(FormatDurationShort(3.0) == L"0:03");
    CHECK(FormatDurationShort(65.2) == L"1:05");
    CHECK(FormatDurationShort(3661.0) == L"1:01:01");

    CHECK(ProbeMp4DurationSeconds(L"Z:\\nonexistent\\missing_clip.mp4") == 0.0);

    std::filesystem::path outDir = std::filesystem::current_path() / "build" / "test_out";
    std::filesystem::path mp4Path = outDir / "e2e_h264_aac.mp4";
    CHECK(std::filesystem::exists(mp4Path));

    double dur = ProbeMp4DurationSeconds(mp4Path.wstring());
    std::printf("ProbeMp4DurationSeconds=%.3fs\n", dur);
    CHECK(dur >= 0.90 && dur <= 1.15);

    std::filesystem::path tmpFile = outDir / "e2e_h264_aac.mp4.tmp";
    {
        HANDLE h = CreateFileW(tmpFile.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    auto clips = ScanClipsDirectory(outDir.wstring(), 16);
    std::error_code ec;
    std::filesystem::remove(tmpFile, ec);

    bool foundE2e = false;
    for (const auto& c : clips) {
        CHECK(c.fileName.find(L".tmp") == std::wstring::npos);
        if (c.fileName == L"e2e_h264_aac.mp4") {
            foundE2e = true;
            CHECK(c.displayTitle == L"e2e_h264_aac");
            CHECK(c.sizeBytes > 10000);
            CHECK(c.durationSec >= 0.90 && c.durationSec <= 1.15);
            CHECK(!c.dateLabel.empty());
        }
    }
    CHECK(foundE2e);
    std::printf("PASS TestClipLibrary\n");
}

}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    TestConfig();
    TestRingBuffer();
    TestMp4StructureAndCo64();
    TestRecorderComponentsAndLifecycle();
    TestEndToEndNvencAacAndSourceReader();
    TestClipLibrary();

    MFShutdown();
    CoUninitialize();
    std::printf("\nTotal failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
