#include "SettingsManager.h"

#include <opencv2/core/persistence.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <system_error>
#include <utility>

namespace wpc {

SettingsManager::SettingsManager(std::filesystem::path settingsPath)
    : settingsPath_(std::move(settingsPath)) {}

bool SettingsManager::load(AppSettings& settings) const {
    std::error_code filesystemError;
    if (!std::filesystem::exists(settingsPath_, filesystemError) || filesystemError) {
        return false;
    }

    try {
        AppSettings candidate = settings;
        cv::FileStorage storage(settingsPath_.string(), cv::FileStorage::READ);
        if (!storage.isOpened()) {
            return false;
        }

        const cv::FileNode camera = storage["camera"];
        if (!camera.empty()) {
            camera["index"] >> candidate.camera.deviceIndex;
            camera["width"] >> candidate.camera.requestedWidth;
            camera["height"] >> candidate.camera.requestedHeight;
            camera["fps"] >> candidate.camera.requestedFps;
            int preferMjpeg = candidate.camera.preferMjpeg ? 1 : 0;
            camera["prefer_mjpeg"] >> preferMjpeg;
            candidate.camera.preferMjpeg = preferMjpeg != 0;
            std::string backend = toString(candidate.camera.preferredBackend);
            camera["backend"] >> backend;
            candidate.camera.preferredBackend = cameraBackendFromString(backend);
            camera["pixel_format"] >> candidate.camera.requestedPixelFormat;
        }

        const cv::FileNode output = storage["output"];
        if (!output.empty()) {
            output["width"] >> candidate.outputSize.width;
            output["height"] >> candidate.outputSize.height;
            output["fps"] >> candidate.outputFps;
            int followsCapture = candidate.outputFollowsCapture ? 1 : 0;
            output["follows_capture"] >> followsCapture;
            candidate.outputFollowsCapture = followsCapture != 0;
            output["method"] >> candidate.outputMethod;
            std::string recordingDirectory = candidate.recordingDirectory.string();
            output["recording_directory"] >> recordingDirectory;
            if (!recordingDirectory.empty()) {
                candidate.recordingDirectory = recordingDirectory;
            }
        }

        const cv::FileNode points = storage["points"];
        if (points.isSeq() && points.size() == candidate.normalizedPoints.size()) {
            std::size_t index = 0;
            for (const auto& pointNode : points) {
                if (pointNode.isSeq() && pointNode.size() == 2) {
                    pointNode[0] >> candidate.normalizedPoints[index].x;
                    pointNode[1] >> candidate.normalizedPoints[index].y;
                }
                ++index;
            }
        }

        sanitize(candidate);
        settings = candidate;
        return true;
    } catch (const cv::Exception& error) {
        std::cerr << "設定ファイルを読み込めませんでした: " << error.what() << '\n';
        return false;
    }
}

bool SettingsManager::save(const AppSettings& settings) const {
    try {
        const auto parent = settingsPath_.parent_path();
        if (!parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error) {
                std::cerr << "設定ディレクトリを作成できませんでした: " << error.message() << '\n';
                return false;
            }
        }

        cv::FileStorage storage(
            settingsPath_.string(),
            cv::FileStorage::WRITE | cv::FileStorage::FORMAT_JSON);
        if (!storage.isOpened()) {
            return false;
        }

        storage << "camera" << "{"
                << "index" << settings.camera.deviceIndex
                << "width" << settings.camera.requestedWidth
                << "height" << settings.camera.requestedHeight
                << "fps" << settings.camera.requestedFps
                << "backend" << toString(settings.camera.preferredBackend)
                << "prefer_mjpeg" << static_cast<int>(settings.camera.preferMjpeg)
                << "pixel_format" << settings.camera.requestedPixelFormat
                << "}";
        storage << "output" << "{"
                << "width" << settings.outputSize.width
                << "height" << settings.outputSize.height
                << "fps" << settings.outputFps
                << "follows_capture" << static_cast<int>(settings.outputFollowsCapture)
                << "method" << settings.outputMethod
                << "recording_directory" << settings.recordingDirectory.string()
                << "}";
        storage << "points" << "[";
        for (const auto& point : settings.normalizedPoints) {
            storage << "[" << point.x << point.y << "]";
        }
        storage << "]";
        return true;
    } catch (const cv::Exception& error) {
        std::cerr << "設定ファイルを保存できませんでした: " << error.what() << '\n';
        return false;
    }
}

const std::filesystem::path& SettingsManager::path() const noexcept {
    return settingsPath_;
}

void SettingsManager::sanitize(AppSettings& settings) {
    settings.camera.deviceIndex = std::clamp(settings.camera.deviceIndex, 0, 31);
    settings.camera.requestedWidth = std::clamp(settings.camera.requestedWidth, 160, 7680);
    settings.camera.requestedHeight = std::clamp(settings.camera.requestedHeight, 120, 4320);
    if (!std::isfinite(settings.camera.requestedFps)) {
        settings.camera.requestedFps = 60.0;
    }
    settings.camera.requestedFps = std::clamp(settings.camera.requestedFps, 1.0, 240.0);
    settings.outputSize.width = std::clamp(settings.outputSize.width, 1, 16384);
    settings.outputSize.height = std::clamp(settings.outputSize.height, 1, 16384);
    if (!std::isfinite(settings.outputFps)) {
        settings.outputFps = 30.0;
    }
    settings.outputFps = std::clamp(settings.outputFps, 1.0, 120.0);
    if (settings.outputMethod != "Spout" && settings.outputMethod != "VirtualCamera") {
        settings.outputMethod = "Spout";
    }
    if (settings.recordingDirectory.empty()) {
        settings.recordingDirectory = "recordings";
    }
    for (auto& point : settings.normalizedPoints) {
        point.x = std::isfinite(point.x) ? std::clamp(point.x, 0.0F, 1.0F) : 0.5F;
        point.y = std::isfinite(point.y) ? std::clamp(point.y, 0.0F, 1.0F) : 0.5F;
    }
}

} // namespace wpc
