#include "SpoutOutput.h"

#include <SpoutDX.h>

#include <opencv2/imgproc.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>

namespace wpc {

class SpoutOutput::Impl {
public:
    explicit Impl(std::string senderName) : senderName_(std::move(senderName)) {}

    ~Impl() {
        stop();
    }

    bool configure(const VideoOutputConfig& config) {
        if (config.frameSize.width <= 0 || config.frameSize.height <= 0 ||
            !std::isfinite(config.framesPerSecond) || config.framesPerSecond <= 0.0) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "Invalid output configuration";
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) {
            status_ = "Stop Spout before changing its format";
            return false;
        }
        config_ = config;
        bgraFrame_.create(config.frameSize, CV_8UC4);
        status_ = "Ready";
        return true;
    }

    bool start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }
        if (config_.frameSize.width <= 0 || config_.frameSize.height <= 0) {
            status_ = "Spout is not configured";
            return false;
        }

        sender_.SetSenderFormat(DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!sender_.OpenDirectX11() || !sender_.SetSenderName(senderName_.c_str())) {
            sender_.CloseDirectX11();
            status_ = "D3D11 initialization failed";
            return false;
        }

        bgraFrame_.setTo(cv::Scalar(0, 0, 0, 255));
        const bool created = sender_.SendImage(
            bgraFrame_.ptr<unsigned char>(),
            static_cast<unsigned int>(bgraFrame_.cols),
            static_cast<unsigned int>(bgraFrame_.rows),
            static_cast<unsigned int>(bgraFrame_.step));
        if (!created || !sender_.IsInitialized()) {
            sender_.ReleaseSender();
            sender_.CloseDirectX11();
            status_ = "Spout sender creation failed";
            return false;
        }

        actualSenderName_ = sender_.GetName() != nullptr ? sender_.GetName() : senderName_;
        lastFrameTime_ = {};
        running_.store(true, std::memory_order_release);
        status_ = "Running: " + actualSenderName_;
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.exchange(false, std::memory_order_acq_rel) && !sender_.IsInitialized()) {
            return;
        }
        sender_.ReleaseSender();
        sender_.CloseDirectX11();
        actualSenderName_.clear();
        status_ = "Stopped";
    }

    void submitFrame(const cv::Mat& correctedBgrFrame) {
        if (!running_.load(std::memory_order_acquire) || correctedBgrFrame.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load(std::memory_order_relaxed)) {
            return;
        }
        if (correctedBgrFrame.size() != config_.frameSize || correctedBgrFrame.type() != CV_8UC3) {
            status_ = "Unexpected frame format";
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto minimumInterval = std::chrono::duration<double>(1.0 / config_.framesPerSecond);
        if (lastFrameTime_.time_since_epoch().count() != 0 && now - lastFrameTime_ < minimumInterval) {
            return;
        }

        try {
            cv::cvtColor(correctedBgrFrame, bgraFrame_, cv::COLOR_BGR2BGRA);
        } catch (const cv::Exception&) {
            status_ = "BGR to BGRA conversion failed";
            return;
        }

        if (!sender_.SendImage(
                bgraFrame_.ptr<unsigned char>(),
                static_cast<unsigned int>(bgraFrame_.cols),
                static_cast<unsigned int>(bgraFrame_.rows),
                static_cast<unsigned int>(bgraFrame_.step))) {
            status_ = "Spout frame submission failed";
            return;
        }
        lastFrameTime_ = now;
    }

    [[nodiscard]] bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string statusText() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    [[nodiscard]] VideoOutputConfig config() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    [[nodiscard]] std::string senderName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return actualSenderName_.empty() ? senderName_ : actualSenderName_;
    }

private:
    mutable std::mutex mutex_;
    spoutDX sender_;
    VideoOutputConfig config_;
    cv::Mat bgraFrame_;
    std::string senderName_;
    std::string actualSenderName_;
    std::string status_ = "Stopped";
    std::chrono::steady_clock::time_point lastFrameTime_{};
    std::atomic<bool> running_{false};
};

SpoutOutput::SpoutOutput(std::string senderName)
    : impl_(std::make_unique<Impl>(std::move(senderName))) {}

SpoutOutput::~SpoutOutput() = default;

bool SpoutOutput::configure(const VideoOutputConfig& config) {
    return impl_->configure(config);
}

bool SpoutOutput::start() {
    return impl_->start();
}

void SpoutOutput::stop() {
    impl_->stop();
}

void SpoutOutput::submitFrame(const cv::Mat& correctedBgrFrame) {
    impl_->submitFrame(correctedBgrFrame);
}

bool SpoutOutput::isRunning() const noexcept {
    return impl_->isRunning();
}

std::string SpoutOutput::name() const {
    return "Spout";
}

std::string SpoutOutput::statusText() const {
    return impl_->statusText();
}

VideoOutputConfig SpoutOutput::config() const {
    return impl_->config();
}

std::string SpoutOutput::senderName() const {
    return impl_->senderName();
}

} // namespace wpc
