#pragma once

#include "CameraManager.h"
#include "PerspectiveCorrector.h"

#include <opencv2/core.hpp>

#include <filesystem>
#include <string>

namespace wpc {

struct AppSettings {
    CameraConfig camera;
    cv::Size outputSize{1920, 1080};
    double outputFps = 30.0;
    bool outputFollowsCapture = false;
    std::string outputMethod = "Spout";
    std::filesystem::path recordingDirectory = "recordings";
    PerspectiveCorrector::PointArray normalizedPoints{
        cv::Point2f{0.10F, 0.10F},
        cv::Point2f{0.90F, 0.10F},
        cv::Point2f{0.90F, 0.90F},
        cv::Point2f{0.10F, 0.90F}};
};

class SettingsManager {
public:
    explicit SettingsManager(std::filesystem::path settingsPath);

    [[nodiscard]] bool load(AppSettings& settings) const;
    [[nodiscard]] bool save(const AppSettings& settings) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    static void sanitize(AppSettings& settings);

    std::filesystem::path settingsPath_;
};

} // namespace wpc
