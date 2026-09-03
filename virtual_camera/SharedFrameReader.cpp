#include "SharedFrameReader.h"

#include <algorithm>
#include <cstring>

namespace wpc::virtual_camera {

SharedFrameReader::~SharedFrameReader() {
    close();
}

bool SharedFrameReader::open() {
    if (header_ != nullptr) {
        return true;
    }
    mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shared_frame::MappingName);
    if (mapping_ == nullptr) {
        return false;
    }
    header_ = static_cast<shared_frame::Header*>(
        MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, shared_frame::MappingBytes));
    if (header_ == nullptr || header_->magic != shared_frame::Magic ||
        header_->version != shared_frame::Version) {
        close();
        return false;
    }
    return true;
}

void SharedFrameReader::close() {
    if (header_ != nullptr) {
        UnmapViewOfFile(header_);
        header_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

bool SharedFrameReader::readFormat(
    std::uint32_t& width,
    std::uint32_t& height,
    std::uint32_t& frameRateNumerator,
    std::uint32_t& frameRateDenominator,
    std::uint32_t& dataBytes) {
    if (!open()) {
        return false;
    }
    const LONG currentWidth = InterlockedCompareExchange(&header_->width, 0, 0);
    const LONG currentHeight = InterlockedCompareExchange(&header_->height, 0, 0);
    const LONG currentNumerator = InterlockedCompareExchange(&header_->frameRateNumerator, 0, 0);
    const LONG currentDenominator = InterlockedCompareExchange(&header_->frameRateDenominator, 0, 0);
    const LONG currentDataBytes = InterlockedCompareExchange(&header_->dataBytes, 0, 0);
    if (currentWidth <= 0 || currentHeight <= 0 || currentNumerator <= 0 ||
        currentDenominator <= 0 || currentDataBytes <= 0 ||
        static_cast<std::uint32_t>(currentDataBytes) > shared_frame::MaxNv12Bytes) {
        return false;
    }
    width = static_cast<std::uint32_t>(currentWidth);
    height = static_cast<std::uint32_t>(currentHeight);
    frameRateNumerator = static_cast<std::uint32_t>(currentNumerator);
    frameRateDenominator = static_cast<std::uint32_t>(currentDenominator);
    dataBytes = static_cast<std::uint32_t>(currentDataBytes);
    return true;
}

bool SharedFrameReader::copyLatest(void* destination, const std::size_t destinationBytes) {
    if (destination == nullptr || !open()) {
        return false;
    }
    const LONG dataBytes = InterlockedCompareExchange(&header_->dataBytes, 0, 0);
    if (dataBytes <= 0 || static_cast<std::size_t>(dataBytes) > destinationBytes ||
        static_cast<std::uint32_t>(dataBytes) > shared_frame::MaxNv12Bytes) {
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG slot = InterlockedCompareExchange(&header_->activeSlot, 0, 0);
        if (slot < 0 || slot >= static_cast<LONG>(shared_frame::SlotCount)) {
            return false;
        }
        const LONG64 sequenceBefore = InterlockedCompareExchange64(
            &header_->slotSequence[slot], 0, 0);
        if ((sequenceBefore & 1) != 0) {
            YieldProcessor();
            continue;
        }
        std::memcpy(
            destination,
            shared_frame::slotData(header_, static_cast<std::size_t>(slot)),
            static_cast<std::size_t>(dataBytes));
        MemoryBarrier();
        const LONG64 sequenceAfter = InterlockedCompareExchange64(
            &header_->slotSequence[slot], 0, 0);
        if (sequenceBefore == sequenceAfter && (sequenceAfter & 1) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace wpc::virtual_camera

