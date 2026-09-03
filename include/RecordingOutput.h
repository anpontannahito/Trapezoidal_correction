#pragma once

#include "IVideoOutput.h"

#include <filesystem>
#include <memory>
#include <string>

namespace wpc {

class RecordingOutput final : public IVideoOutput {
public:
    RecordingOutput();
    ~RecordingOutput() override;

    RecordingOutput(const RecordingOutput&) = delete;
    RecordingOutput& operator=(const RecordingOutput&) = delete;

    bool configure(const VideoOutputConfig& config) override;
    void setOutputPath(std::filesystem::path outputPath);
    bool start() override;
    void stop() override;
    void submitFrame(const cv::Mat& correctedBgrFrame) override;

    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string statusText() const override;
    [[nodiscard]] VideoOutputConfig config() const override;
    [[nodiscard]] std::filesystem::path outputPath() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wpc
