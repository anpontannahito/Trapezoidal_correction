#pragma once

#include "CameraManager.h"
#include "OutputManager.h"
#include "PerspectiveCorrector.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace wpc {

struct ProcessingStats {
    double framesPerSecond = 0.0;
    std::uint64_t processedFrames = 0;
};

class FrameProcessor {
public:
    FrameProcessor(
        CameraManager& camera,
        PerspectiveCorrector& corrector,
        OutputManager* outputManager = nullptr);
    ~FrameProcessor();

    FrameProcessor(const FrameProcessor&) = delete;
    FrameProcessor& operator=(const FrameProcessor&) = delete;

    bool start();
    void stop();

    // UI所有のバッファと最新結果を swap する。古いフレームはキューに蓄積しない。
    bool takeLatest(
        cv::Mat& source,
        cv::Mat& corrected,
        std::uint64_t& lastSequence,
        ProcessingStats& stats);

    [[nodiscard]] bool isRunning() const noexcept;

private:
    void processingLoop();

    CameraManager& camera_;
    PerspectiveCorrector& corrector_;
    OutputManager* outputManager_;

    std::thread processingThread_;
    std::atomic<bool> running_{false};

    std::mutex resultMutex_;
    cv::Mat latestSource_;
    cv::Mat latestCorrected_;
    std::uint64_t latestSequence_ = 0;
    ProcessingStats latestStats_;
};

} // namespace wpc
