#pragma once

#include <opencv2/core.hpp>

#include <string>

namespace wpc {

struct VideoOutputConfig {
    cv::Size frameSize{1920, 1080};
    double framesPerSecond = 30.0;
};

class IVideoOutput {
public:
    virtual ~IVideoOutput() = default;

    virtual bool configure(const VideoOutputConfig& config) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void submitFrame(const cv::Mat& correctedBgrFrame) = 0;

    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string statusText() const = 0;
    [[nodiscard]] virtual VideoOutputConfig config() const = 0;
};

} // namespace wpc
