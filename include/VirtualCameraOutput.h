#pragma once

#include "IVideoOutput.h"

#include <opencv2/core.hpp>

#include <memory>
#include <string>

namespace wpc {

class VirtualCameraOutput final : public IVideoOutput {
public:
    VirtualCameraOutput();
    ~VirtualCameraOutput();

    VirtualCameraOutput(const VirtualCameraOutput&) = delete;
    VirtualCameraOutput& operator=(const VirtualCameraOutput&) = delete;

    bool configure(const VideoOutputConfig& config) override;
    bool configure(cv::Size outputSize, double framesPerSecond) {
        return configure({outputSize, framesPerSecond});
    }
    bool start() override;
    void stop() override;
    void submitFrame(const cv::Mat& correctedBgrFrame) override;
    void publish(const cv::Mat& correctedBgrFrame);
    void setInputConnected(bool connected) noexcept;

    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string statusText() const override;
    [[nodiscard]] VideoOutputConfig config() const noexcept override;
    [[nodiscard]] std::string mediaSourceDiagnosticText() const;
    [[nodiscard]] cv::Size outputSize() const noexcept;
    [[nodiscard]] double outputFramesPerSecond() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wpc
