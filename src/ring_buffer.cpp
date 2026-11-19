#include "ring_buffer.h"

#include <algorithm>
#include <cstring>

namespace clipper {

PacketRingBuffer::PacketRingBuffer(size_t arenaBytes, size_t maxSlots)
    : arena_(std::max<size_t>(arenaBytes, 1)),
      slots_(std::max<size_t>(maxSlots, 1)),
      arenaCapacity_(std::max<size_t>(arenaBytes, 1)),
      maxSlots_(std::max<size_t>(maxSlots, 1)) {}

bool PacketRingBuffer::Push(const uint8_t* data, uint32_t size, int64_t ptsUs, uint32_t durationUs, bool isKeyFrame) {
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
        usedPayloadBytes_ -= slots_[headIdx_].size;
        headIdx_ = (headIdx_ + 1) % maxSlots_;
        slotCount_--;
    }

    std::memcpy(arena_.data() + startPhys, data, size);
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
    return true;
}

void PacketRingBuffer::AdvanceGeneration() {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_++;
}

void PacketRingBuffer::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    headIdx_ = 0;
    tailIdx_ = 0;
    slotCount_ = 0;
    writeLogical_ = 0;
    usedPayloadBytes_ = 0;
    generation_++;
}

ClipStreamSnapshot PacketRingBuffer::SnapshotVideoWindow(int64_t durationUs, int64_t maxEndPtsUs) const {
    ClipStreamSnapshot snap;
    std::lock_guard<std::mutex> lock(mutex_);
    if (slotCount_ == 0) return snap;

    size_t endOffset = slotCount_;
    while (endOffset > 0) {
        size_t idx = (headIdx_ + endOffset - 1) % maxSlots_;
        if (slots_[idx].ptsUs <= maxEndPtsUs) break;
        endOffset--;
    }
    if (endOffset == 0) return snap;

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
        if (!hasFallbackKf) return snap;
        chosenRel = fallbackKfRel;
    }

    size_t count = endRel - chosenRel + 1;
    size_t totalBytes = 0;
    for (size_t i = 0; i < count; ++i) {
        totalBytes += slots_[(headIdx_ + chosenRel + i) % maxSlots_].size;
    }

    snap.bytes.resize(totalBytes);
    snap.packets.reserve(count);
    uint32_t outOff = 0;
    for (size_t i = 0; i < count; ++i) {
        const PacketSlot& s = slots_[(headIdx_ + chosenRel + i) % maxSlots_];
        std::memcpy(snap.bytes.data() + outOff, arena_.data() + s.physOffset, s.size);
        snap.packets.push_back(ClipPacket{ outOff, s.size, s.ptsUs, s.durationUs, s.isKeyFrame });
        outOff += s.size;
    }

    snap.generation = activeGen;
    snap.startPtsUs = snap.packets.front().ptsUs;
    snap.endPtsUs = snap.packets.back().ptsUs + snap.packets.back().durationUs;
    return snap;
}

ClipStreamSnapshot PacketRingBuffer::SnapshotAudioRange(int64_t startPtsUs, int64_t endPtsUs) const {
    ClipStreamSnapshot snap;
    std::lock_guard<std::mutex> lock(mutex_);
    if (slotCount_ == 0) return snap;

    int64_t minPtsUs = startPtsUs - 21334;
    size_t firstRel = slotCount_;
    size_t lastRel = 0;
    size_t totalBytes = 0;

    for (size_t rel = 0; rel < slotCount_; ++rel) {
        const PacketSlot& s = slots_[(headIdx_ + rel) % maxSlots_];
        if (s.ptsUs >= minPtsUs && s.ptsUs <= endPtsUs) {
            if (firstRel == slotCount_) firstRel = rel;
            lastRel = rel;
            totalBytes += s.size;
        }
    }

    if (firstRel == slotCount_) return snap;

    size_t count = lastRel - firstRel + 1;
    snap.bytes.resize(totalBytes);
    snap.packets.reserve(count);
    uint32_t outOff = 0;
    for (size_t i = 0; i < count; ++i) {
        const PacketSlot& s = slots_[(headIdx_ + firstRel + i) % maxSlots_];
        std::memcpy(snap.bytes.data() + outOff, arena_.data() + s.physOffset, s.size);
        snap.packets.push_back(ClipPacket{ outOff, s.size, s.ptsUs, s.durationUs, s.isKeyFrame });
        outOff += s.size;
    }

    snap.generation = slots_[(headIdx_ + lastRel) % maxSlots_].generation;
    snap.startPtsUs = snap.packets.front().ptsUs;
    snap.endPtsUs = snap.packets.back().ptsUs + snap.packets.back().durationUs;
    return snap;
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
