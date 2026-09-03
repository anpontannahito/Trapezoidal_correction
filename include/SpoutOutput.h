#pragma once

#include "IVideoOutput.h"

#include <memory>
#include <string>

namespace wpc {

class SpoutOutput final : public IVideoOutput {
public:
    static constexpr const char* DefaultSenderName = "Perspective Camera";

    explicit SpoutOutput(std::string senderName = DefaultSenderName);
    ~SpoutOutput() override;

    SpoutOutput(const SpoutOutput&) = delete;
    SpoutOutput& operator=(const SpoutOutput&) = delete;

    bool configure(const VideoOutputConfig& config) override;
    bool start() override;
    void stop() override;
    void submitFrame(const cv::Mat& correctedBgrFrame) override;

    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string statusText() const override;
    [[nodiscard]] VideoOutputConfig config() const override;
    [[nodiscard]] std::string senderName() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wpc
