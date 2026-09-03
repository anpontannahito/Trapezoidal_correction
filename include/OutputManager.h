#pragma once

#include "IVideoOutput.h"
#include "RecordingOutput.h"
#include "SpoutOutput.h"
#include "VirtualCameraOutput.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace wpc {

enum class ExternalOutputMethod {
    Spout,
    VirtualCamera
};

[[nodiscard]] std::string toString(ExternalOutputMethod method);
[[nodiscard]] ExternalOutputMethod externalOutputMethodFromString(const std::string& value);

class OutputManager {
public:
    OutputManager();
    ~OutputManager();

    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    bool configure(const VideoOutputConfig& config);
    bool startDispatcher();
    void shutdown();
    void submitFrame(const cv::Mat& correctedBgrFrame);
    void setInputConnected(bool connected) noexcept;

    void setSelectedMethod(ExternalOutputMethod method) noexcept;
    [[nodiscard]] ExternalOutputMethod selectedMethod() const noexcept;
    bool startExternalOutput();
    void stopExternalOutput();
    [[nodiscard]] bool isExternalOutputRunning() const noexcept;
    [[nodiscard]] std::string externalStatusText() const;

    bool startRecording(const std::filesystem::path& directory);
    void stopRecording();
    [[nodiscard]] bool isRecording() const noexcept;
    [[nodiscard]] std::string recordingStatusText() const;
    [[nodiscard]] std::filesystem::path recordingPath() const;

    [[nodiscard]] VideoOutputConfig config() const;
    [[nodiscard]] std::string spoutSenderName() const;

private:
    void outputLoop();
    void updateNeedsFrames() noexcept;
    IVideoOutput& selectedOutput() noexcept;
    const IVideoOutput& selectedOutput() const noexcept;

    mutable std::mutex controlMutex_;
    mutable std::mutex frameMutex_;
    std::condition_variable frameCondition_;
    std::thread outputThread_;
    cv::Mat latestFrame_;
    std::uint64_t latestSequence_ = 0;
    std::uint64_t consumedSequence_ = 0;
    VideoOutputConfig config_;
    SpoutOutput spoutOutput_;
    VirtualCameraOutput virtualCameraOutput_;
    RecordingOutput recordingOutput_;
    std::atomic<ExternalOutputMethod> selectedMethod_{ExternalOutputMethod::Spout};
    std::atomic<bool> dispatcherRunning_{false};
    std::atomic<bool> needsFrames_{false};
};

} // namespace wpc
