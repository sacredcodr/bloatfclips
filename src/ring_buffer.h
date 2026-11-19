#pragma once

#include <atomic>
#include <condition_variable>
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

// Arena pages are committed up front but only become resident when written. Packets older than
// retainUs (whole GOPs, so a keyframe at or before the cutoff always survives) are evicted and
// their pages discarded, so resident memory tracks the real bitrate, not the worst-case arena.
class PacketRingBuffer {
public:
    PacketRingBuffer(size_t arenaBytes, size_t maxSlots, int64_t retainUs = 0);
    ~PacketRingBuffer();
    PacketRingBuffer(const PacketRingBuffer&) = delete;
    PacketRingBuffer& operator=(const PacketRingBuffer&) = delete;

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
    bool WaitForPts(int64_t targetPtsUs, const std::atomic<bool>& abortFlag, uint32_t timeoutMs) const;
    void NotifyAll() const;

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

    void PopHeadLocked(size_t count);
    void EvictExpiredLocked();
    void DiscardLogicalRange(uint64_t fromLogical, uint64_t toLogical);
    void ReleaseFreedPagesLocked();
    uint64_t HeadLogicalLocked() const { return slotCount_ ? slots_[headIdx_].logicalOffset : writeLogical_; }
    bool SelectVideoLocked(int64_t durationUs, int64_t maxEndPtsUs, size_t& firstRel, size_t& count) const;
    void SelectAudioLocked(int64_t startPtsUs, int64_t endPtsUs, std::vector<size_t>& rels) const;
    ClipStreamSnapshot CopyOut(std::unique_lock<std::mutex>& lock, const std::vector<size_t>& rels, bool& stale) const;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    uint8_t* arena_ = nullptr;
    std::vector<PacketSlot> slots_;
    size_t arenaCapacity_ = 0;
    size_t maxSlots_ = 0;
    int64_t retainUs_ = 0;
    size_t headIdx_ = 0;
    size_t tailIdx_ = 0;
    size_t slotCount_ = 0;
    uint64_t writeLogical_ = 0;
    uint64_t discardedTo_ = 0;
    uint64_t usedPayloadBytes_ = 0;
    uint64_t droppedOversize_ = 0;
    int64_t latestPtsUs_ = INT64_MIN;
    uint32_t generation_ = 1;
};

}
