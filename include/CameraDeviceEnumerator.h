#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wpc {

struct CameraMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frameRateNumerator = 0;
    std::uint32_t frameRateDenominator = 1;
    std::string pixelFormat;

    [[nodiscard]] double framesPerSecond() const noexcept;
    [[nodiscard]] std::string displayName() const;
};

struct CameraDeviceInfo {
    int captureIndex = 0;
    std::string friendlyName;
    std::wstring symbolicLink;
    std::vector<CameraMode> modes;
};

class CameraDeviceEnumerator {
public:
    [[nodiscard]] static std::vector<CameraDeviceInfo> enumerate();
};

} // namespace wpc
