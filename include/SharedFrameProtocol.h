#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace wpc::shared_frame {

inline constexpr wchar_t MappingName[] = L"Global\\PerspectiveCamera.Frame.v2";
inline constexpr std::uint32_t Magic = 0x5043414DU; // "PCAM"
inline constexpr std::uint32_t Version = 2;
inline constexpr std::uint32_t MaxWidth = 4096;
inline constexpr std::uint32_t MaxHeight = 4096;
inline constexpr std::uint32_t MaxNv12Bytes = MaxWidth * MaxHeight * 3U / 2U;
inline constexpr std::size_t SlotCount = 2;

struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t headerBytes;
    std::uint32_t maxFrameBytes;
    volatile LONG activeSlot;
    volatile LONG width;
    volatile LONG height;
    volatile LONG frameRateNumerator;
    volatile LONG frameRateDenominator;
    volatile LONG dataBytes;
    volatile LONG connected;
    volatile LONG lastFailureStage;
    volatile LONG lastFailureHresult;
    alignas(8) volatile LONG64 frameSequence;
    alignas(8) volatile LONG64 slotSequence[SlotCount];
};

inline void reportFailure(const LONG stage, const HRESULT result) noexcept {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, MappingName);
    if (mapping == nullptr) {
        return;
    }
    auto* header = static_cast<Header*>(
        MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Header)));
    if (header != nullptr) {
        if (header->magic == Magic && header->version == Version) {
            InterlockedExchange(&header->lastFailureStage, stage);
            InterlockedExchange(&header->lastFailureHresult, result);
        }
        UnmapViewOfFile(header);
    }
    CloseHandle(mapping);
}

inline constexpr std::size_t MappingBytes = sizeof(Header) + SlotCount * MaxNv12Bytes;

inline std::byte* slotData(Header* header, const std::size_t slot) noexcept {
    return reinterpret_cast<std::byte*>(header) + sizeof(Header) + slot * MaxNv12Bytes;
}

inline const std::byte* slotData(const Header* header, const std::size_t slot) noexcept {
    return reinterpret_cast<const std::byte*>(header) + sizeof(Header) + slot * MaxNv12Bytes;
}

} // namespace wpc::shared_frame
