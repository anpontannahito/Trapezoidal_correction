#pragma once

#include "SharedFrameProtocol.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace wpc::virtual_camera {

class SharedFrameReader {
public:
    SharedFrameReader() = default;
    ~SharedFrameReader();

    SharedFrameReader(const SharedFrameReader&) = delete;
    SharedFrameReader& operator=(const SharedFrameReader&) = delete;

    bool open();
    void close();
    [[nodiscard]] bool readFormat(
        std::uint32_t& width,
        std::uint32_t& height,
        std::uint32_t& frameRateNumerator,
        std::uint32_t& frameRateDenominator,
        std::uint32_t& dataBytes);
    [[nodiscard]] bool copyLatest(void* destination, std::size_t destinationBytes);

private:
    HANDLE mapping_ = nullptr;
    shared_frame::Header* header_ = nullptr;
};

} // namespace wpc::virtual_camera

