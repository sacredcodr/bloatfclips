#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace clipper {

struct ClipPacket {
    uint32_t offset = 0;
    uint32_t size = 0;
    int64_t ptsUs = 0;
    uint32_t durationUs = 0;
    bool isKeyFrame = false;
};

struct ClipStreamSnapshot {
    std::vector<uint8_t> bytes;
    std::vector<ClipPacket> packets;
    uint32_t generation = 0;
    int64_t startPtsUs = 0;
    int64_t endPtsUs = 0;

    bool Empty() const { return packets.empty(); }
};

class PacketRingBuffer {
public:
    PacketRingBuffer(size_t arenaBytes, size_t maxSlots);

    bool Push(const uint8_t* data, uint32_t size, int64_t ptsUs, uint32_t durationUs, bool isKeyFrame);
    void AdvanceGeneration();
    void Clear();

    ClipStreamSnapshot SnapshotVideoWindow(int64_t durationUs, int64_t maxEndPtsUs = INT64_MAX) const;
    ClipStreamSnapshot SnapshotAudioRange(int64_t startPtsUs, int64_t endPtsUs) const;

    size_t ArenaCapacity() const { return arenaCapacity_; }
    size_t MaxSlots() const { return maxSlots_; }
    size_t PacketCount() const;
    size_t UsedPayloadBytes() const;
    uint64_t DroppedOversizeCount() const;

private:
    struct PacketSlot {
        uint64_t logicalOffset = 0;
        uint32_t physOffset = 0;
        uint32_t size = 0;
        int64_t ptsUs = 0;
        uint32_t durationUs = 0;
        uint32_t generation = 0;
        bool isKeyFrame = false;
    };

    mutable std::mutex mutex_;
    std::vector<uint8_t> arena_;
    std::vector<PacketSlot> slots_;
    size_t arenaCapacity_ = 0;
    size_t maxSlots_ = 0;
    size_t headIdx_ = 0;
    size_t tailIdx_ = 0;
    size_t slotCount_ = 0;
    uint64_t writeLogical_ = 0;
    uint64_t usedPayloadBytes_ = 0;
    uint64_t droppedOversize_ = 0;
    uint32_t generation_ = 1;
};

}
