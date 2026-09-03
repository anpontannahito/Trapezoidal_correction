#include "FrameProcessor.h"

#include <chrono>
#include <utility>

namespace wpc {

FrameProcessor::FrameProcessor(
    CameraManager& camera,
    PerspectiveCorrector& corrector,
    OutputManager* outputManager)
    : camera_(camera),
      corrector_(corrector),
      outputManager_(outputManager) {}

FrameProcessor::~FrameProcessor() {
    stop();
}

bool FrameProcessor::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    if (!camera_.isRunning()) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    processingThread_ = std::thread(&FrameProcessor::processingLoop, this);
    return true;
}

void FrameProcessor::stop() {
    running_.store(false, std::memory_order_release);
    if (processingThread_.joinable()) {
        processingThread_.join();
    }
}

bool FrameProcessor::takeLatest(
    cv::Mat& source,
    cv::Mat& corrected,
    std::uint64_t& lastSequence,
    ProcessingStats& stats) {
    std::lock_guard<std::mutex> lock(resultMutex_);
    if (latestSequence_ == lastSequence || latestSource_.empty() || latestCorrected_.empty()) {
        return false;
    }

    std::swap(source, latestSource_);
    std::swap(corrected, latestCorrected_);
    lastSequence = latestSequence_;
    stats = latestStats_;
    return true;
}

bool FrameProcessor::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void FrameProcessor::processingLoop() {
    using Clock = std::chrono::steady_clock;

    cv::Mat sourceBuffer;
    cv::Mat correctedBuffer;
    cv::Size lastInputSize;
    std::uint64_t cameraSequence = 0;
    std::uint64_t processedFrames = 0;
    std::uint64_t framesInWindow = 0;
    double measuredFps = 0.0;
    auto statsWindowStart = Clock::now();

    while (running_.load(std::memory_order_acquire)) {
        if (!camera_.waitForLatest(sourceBuffer, cameraSequence, std::chrono::milliseconds(50))) {
            continue;
        }

        if (sourceBuffer.size() != lastInputSize) {
            corrector_.updateInputSize(sourceBuffer.size());
            lastInputSize = sourceBuffer.size();
        }

        corrector_.apply(sourceBuffer, correctedBuffer);
        if (outputManager_ != nullptr && !correctedBuffer.empty()) {
            outputManager_->setInputConnected(camera_.isConnected());
            outputManager_->submitFrame(correctedBuffer);
        }
        ++processedFrames;
        ++framesInWindow;

        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - statsWindowStart).count();
        if (elapsed >= 0.5) {
            measuredFps = static_cast<double>(framesInWindow) / elapsed;
            framesInWindow = 0;
            statsWindowStart = now;
        }

        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            std::swap(latestSource_, sourceBuffer);
            std::swap(latestCorrected_, correctedBuffer);
            ++latestSequence_;
            latestStats_ = {measuredFps, processedFrames};
        }
    }
}

} // namespace wpc
