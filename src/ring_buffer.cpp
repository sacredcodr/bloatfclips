#include "ring_buffer.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>

namespace clipper {

namespace {

constexpr size_t kPageBytes = 4096;

}

PacketRingBuffer::PacketRingBuffer(size_t arenaBytes, size_t maxSlots, int64_t retainUs)
    : slots_(std::max<size_t>(maxSlots, 1)),
      arenaCapacity_(std::max<size_t>(arenaBytes, 1)),
      maxSlots_(std::max<size_t>(maxSlots, 1)),
      retainUs_(retainUs) {
    // Committed (so writes can never fail) but untouched: pages join the working set only
    // when the first packet lands on them. std::vector would zero-fill, making all of it resident.
    arena_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, arenaCapacity_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!arena_) throw std::bad_alloc();
}

PacketRingBuffer::~PacketRingBuffer() {
    if (arena_) VirtualFree(arena_, 0, MEM_RELEASE);
}

void PacketRingBuffer::DiscardLogicalRange(uint64_t fromLogical, uint64_t toLogical) {
    if (toLogical <= fromLogical) return;
    uint64_t len = std::min<uint64_t>(toLogical - fromLogical, arenaCapacity_);
    size_t start = static_cast<size_t>(fromLogical % arenaCapacity_);
    size_t firstLen = static_cast<size_t>(std::min<uint64_t>(len, arenaCapacity_ - start));
    auto discard = [&](size_t begin, size_t end) {
        size_t a = (begin + kPageBytes - 1) / kPageBytes * kPageBytes;
        size_t b = end / kPageBytes * kPageBytes;
        if (b > a) DiscardVirtualMemory(arena_ + a, b - a);
    };
    discard(start, start + firstLen);
    if (len > firstLen) discard(0, static_cast<size_t>(len - firstLen));
}

// Discards logical [discardedTo_, head). The start is clamped to writeLogical_ - capacity so the
// range never aliases live bytes, and the watermark stops on a physical page boundary so the
// page straddling the head is picked up by a later call instead of leaking.
void PacketRingBuffer::ReleaseFreedPagesLocked() {
    uint64_t head = HeadLogicalLocked();
    uint64_t from = std::max<uint64_t>(discardedTo_, writeLogical_ > arenaCapacity_ ? writeLogical_ - arenaCapacity_ : 0);
    uint64_t to = head - (head % arenaCapacity_) % kPageBytes;
    if (to <= from) return;
    DiscardLogicalRange(from, to);
    discardedTo_ = to;
}

void PacketRingBuffer::PopHeadLocked(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        usedPayloadBytes_ -= slots_[headIdx_].size;
        headIdx_ = (headIdx_ + 1) % maxSlots_;
        slotCount_--;
    }
}

void PacketRingBuffer::EvictExpiredLocked() {
    if (retainUs_ <= 0 || latestPtsUs_ == INT64_MIN) return;
    const int64_t cutoff = latestPtsUs_ - retainUs_;
    // Drop whole GOPs: the head goes only once a later keyframe is itself at or before the
    // cutoff, so the oldest retained packet is always a keyframe no newer than the cutoff.
    while (slotCount_ > 1 && slots_[headIdx_].ptsUs < cutoff) {
        size_t k = 1;
        bool found = false;
        for (; k < slotCount_; ++k) {
            const PacketSlot& s = slots_[(headIdx_ + k) % maxSlots_];
            if (s.ptsUs > cutoff) break;
            if (s.isKeyFrame) {
                found = true;
                break;
            }
        }
        if (!found) break;
        PopHeadLocked(k);
    }
}

bool PacketRingBuffer::Push(const uint8_t* data, uint32_t size, int64_t ptsUs, uint32_t durationUs, bool isKeyFrame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!data || size == 0 || size > arenaCapacity_) {
            droppedOversize_++;
            return false;
        }

        size_t phys = static_cast<size_t>(writeLogical_ % arenaCapacity_);
        size_t tailSpace = arenaCapacity_ - phys;
        uint64_t pad = (tailSpace < size) ? static_cast<uint64_t>(tailSpace) : 0;
        uint64_t startLogical = writeLogical_ + pad;
        uint64_t endLogical = startLogical + size;
        uint32_t startPhys = static_cast<uint32_t>(startLogical % arenaCapacity_);

        while (slotCount_ == maxSlots_ ||
               (slotCount_ > 0 && (endLogical - slots_[headIdx_].logicalOffset) > arenaCapacity_)) {
            PopHeadLocked(1);
        }

        std::memcpy(arena_ + startPhys, data, size);
        slots_[tailIdx_] = PacketSlot{
            startLogical,
            startPhys,
            size,
            ptsUs,
            durationUs,
            generation_,
            isKeyFrame,
        };
        tailIdx_ = (tailIdx_ + 1) % maxSlots_;
        slotCount_++;
        writeLogical_ = endLogical;
        usedPayloadBytes_ += size;
        if (ptsUs > latestPtsUs_) latestPtsUs_ = ptsUs;

        EvictExpiredLocked();
        ReleaseFreedPagesLocked();
    }
    cv_.notify_all();
    return true;
}

void PacketRingBuffer::AdvanceGeneration() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_++;
    }
    cv_.notify_all();
}

void PacketRingBuffer::Clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // writeLogical_ stays monotonic so an in-flight unlocked snapshot copy can detect eviction.
        headIdx_ = 0;
        tailIdx_ = 0;
        slotCount_ = 0;
        usedPayloadBytes_ = 0;
        latestPtsUs_ = INT64_MIN;
        generation_++;
        if (arenaCapacity_ >= kPageBytes) DiscardVirtualMemory(arena_, arenaCapacity_ / kPageBytes * kPageBytes);
        discardedTo_ = writeLogical_;
    }
    cv_.notify_all();
}

bool PacketRingBuffer::WaitForPts(int64_t targetPtsUs, const std::atomic<bool>& abortFlag, uint32_t timeoutMs) const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
        return abortFlag.load(std::memory_order_acquire) || latestPtsUs_ >= targetPtsUs;
    });
    return !abortFlag.load(std::memory_order_acquire) && latestPtsUs_ >= targetPtsUs;
}

void PacketRingBuffer::NotifyAll() const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_all();
}

// Fills packet metadata under the lock, then copies payloads with the lock released so a
// multi-megabyte save never stalls Push() on the capture threads. Packets are only overwritten
// after eviction moves the head past them, so if the head has not passed the first copied
// packet once the copy ends, every copied byte was intact. Otherwise `stale` is set.
ClipStreamSnapshot PacketRingBuffer::CopyOut(std::unique_lock<std::mutex>& lock, const std::vector<size_t>& rels, bool& stale) const {
    stale = false;
    ClipStreamSnapshot snap;
    if (rels.empty()) return snap;

    std::vector<uint32_t> physOffsets;
    physOffsets.reserve(rels.size());
    snap.packets.reserve(rels.size());
    uint64_t totalBytes = 0;
    for (size_t rel : rels) {
        const PacketSlot& s = slots_[(headIdx_ + rel) % maxSlots_];
        snap.packets.push_back(ClipPacket{ static_cast<uint32_t>(totalBytes), s.size, s.ptsUs, s.durationUs, s.isKeyFrame });
        physOffsets.push_back(s.physOffset);
        totalBytes += s.size;
    }
    if (totalBytes > UINT32_MAX) return {};
    const uint64_t firstLogical = slots_[(headIdx_ + rels.front()) % maxSlots_].logicalOffset;
    snap.generation = slots_[(headIdx_ + rels.back()) % maxSlots_].generation;
    snap.startPtsUs = snap.packets.front().ptsUs;
    snap.endPtsUs = snap.packets.back().ptsUs + snap.packets.back().durationUs;

    lock.unlock();
    snap.bytes.resize(static_cast<size_t>(totalBytes));
    for (size_t i = 0; i < snap.packets.size(); ++i) {
        std::memcpy(snap.bytes.data() + snap.packets[i].offset, arena_ + physOffsets[i], snap.packets[i].size);
    }
    lock.lock();
    stale = HeadLogicalLocked() > firstLogical || slotCount_ == 0;
    return snap;
}

bool PacketRingBuffer::SelectVideoLocked(int64_t durationUs, int64_t maxEndPtsUs, size_t& firstRel, size_t& count) const {
    if (slotCount_ == 0) return false;

    size_t endOffset = slotCount_;
    while (endOffset > 0) {
        size_t idx = (headIdx_ + endOffset - 1) % maxSlots_;
        if (slots_[idx].ptsUs <= maxEndPtsUs) break;
        endOffset--;
    }
    if (endOffset == 0) return false;

    size_t endRel = endOffset - 1;
    const PacketSlot& endSlot = slots_[(headIdx_ + endRel) % maxSlots_];
    uint32_t activeGen = endSlot.generation;
    int64_t targetStartPtsUs = endSlot.ptsUs - durationUs;

    bool found = false;
    size_t chosenRel = 0;
    bool hasFallbackKf = false;
    size_t fallbackKfRel = 0;

    for (size_t back = 0; back <= endRel; ++back) {
        size_t rel = endRel - back;
        const PacketSlot& s = slots_[(headIdx_ + rel) % maxSlots_];
        if (s.generation != activeGen) break;
        if (s.isKeyFrame) {
            if (s.ptsUs <= targetStartPtsUs) {
                chosenRel = rel;
                found = true;
                break;
            }
            fallbackKfRel = rel;
            hasFallbackKf = true;
        }
    }

    if (!found) {
        if (!hasFallbackKf) return false;
        chosenRel = fallbackKfRel;
    }
    firstRel = chosenRel;
    count = endRel - chosenRel + 1;
    return true;
}

ClipStreamSnapshot PacketRingBuffer::SnapshotVideoWindow(int64_t durationUs, int64_t maxEndPtsUs) const {
    std::unique_lock<std::mutex> lock(mutex_);
    for (int attempt = 0; attempt < 3; ++attempt) {
        size_t firstRel = 0, count = 0;
        if (!SelectVideoLocked(durationUs, maxEndPtsUs, firstRel, count)) return {};
        std::vector<size_t> rels(count);
        for (size_t i = 0; i < count; ++i) rels[i] = firstRel + i;
        bool stale = false;
        ClipStreamSnapshot snap = CopyOut(lock, rels, stale);
        if (!stale) return snap;
    }
    return {};
}

void PacketRingBuffer::SelectAudioLocked(int64_t startPtsUs, int64_t endPtsUs, std::vector<size_t>& rels) const {
    rels.clear();
    if (slotCount_ == 0) return;

    size_t endOffset = slotCount_;
    while (endOffset > 0) {
        const PacketSlot& s = slots_[(headIdx_ + endOffset - 1) % maxSlots_];
        if (s.ptsUs <= endPtsUs) break;
        endOffset--;
    }
    if (endOffset == 0) return;

    size_t endRel = endOffset - 1;
    uint32_t activeGen = slots_[(headIdx_ + endRel) % maxSlots_].generation;

    for (size_t back = 0; back <= endRel; ++back) {
        size_t rel = endRel - back;
        const PacketSlot& s = slots_[(headIdx_ + rel) % maxSlots_];
        if (s.generation != activeGen) break;
        int64_t pktEndUs = s.ptsUs + static_cast<int64_t>(s.durationUs);
        bool overlaps = (s.durationUs > 0)
            ? (pktEndUs > startPtsUs && s.ptsUs <= endPtsUs)
            : (s.ptsUs >= startPtsUs && s.ptsUs <= endPtsUs);
        if (overlaps) rels.push_back(rel);
    }
    std::reverse(rels.begin(), rels.end());
}

ClipStreamSnapshot PacketRingBuffer::SnapshotAudioRange(int64_t startPtsUs, int64_t endPtsUs) const {
    std::unique_lock<std::mutex> lock(mutex_);
    std::vector<size_t> rels;
    for (int attempt = 0; attempt < 3; ++attempt) {
        SelectAudioLocked(startPtsUs, endPtsUs, rels);
        bool stale = false;
        ClipStreamSnapshot snap = CopyOut(lock, rels, stale);
        if (!stale) return snap;
    }
    return {};
}

size_t PacketRingBuffer::PacketCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slotCount_;
}

size_t PacketRingBuffer::UsedPayloadBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(usedPayloadBytes_);
}

uint64_t PacketRingBuffer::DroppedOversizeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return droppedOversize_;
}

}
