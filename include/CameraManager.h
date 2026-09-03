#pragma once

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace wpc {

enum class CameraBackendPreference {
    DirectShow,
    MediaFoundation,
    Any
};

std::string toString(CameraBackendPreference backend);
CameraBackendPreference cameraBackendFromString(const std::string& value);

struct CameraConfig {
    int deviceIndex = 0;
    int requestedWidth = 1920;
    int requestedHeight = 1080;
    double requestedFps = 30.0;
    CameraBackendPreference preferredBackend = CameraBackendPreference::MediaFoundation;
    bool preferMjpeg = true;
    std::string requestedPixelFormat = "MJPG";
};

class CameraManager {
public:
    CameraManager() = default;
    ~CameraManager();

    CameraManager(const CameraManager&) = delete;
    CameraManager& operator=(const CameraManager&) = delete;

    bool open(const CameraConfig& config);
    bool start();
    void stop();
    void close();

    // destination と最新バッファの cv::Mat ヘッダーを交換するため、画素コピーは発生しない。
    bool waitForLatest(
        cv::Mat& destination,
        std::uint64_t& lastSequence,
        std::chrono::milliseconds timeout);

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool isConnected() const noexcept;
    [[nodiscard]] cv::Size actualFrameSize() const noexcept;
    [[nodiscard]] double actualFps() const noexcept;
    [[nodiscard]] const std::string& actualPixelFormat() const noexcept;
    [[nodiscard]] const std::string& activeBackendName() const noexcept;

private:
    bool tryOpen(int backendApi, const std::string& backendName, const CameraConfig& config);
    void captureLoop();

    cv::VideoCapture capture_;
    CameraConfig config_;
    cv::Size actualFrameSize_;
    double actualFps_ = 0.0;
    std::string actualPixelFormat_;
    std::string activeBackendName_;

    std::thread captureThread_;
    std::atomic<bool> opened_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    mutable std::mutex frameMutex_;
    std::condition_variable frameAvailable_;
    cv::Mat latestFrame_;
    std::uint64_t latestSequence_ = 0;
};

} // namespace wpc
