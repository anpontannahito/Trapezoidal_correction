#include "CameraManager.h"

#include <opencv2/videoio.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <utility>

namespace wpc {

namespace {

constexpr int MaxConsecutiveReadFailures = 30;

int fourccForPixelFormat(const std::string& pixelFormat) {
    if (pixelFormat == "MJPG") {
        return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    }
    if (pixelFormat == "YUY2") {
        return cv::VideoWriter::fourcc('Y', 'U', 'Y', '2');
    }
    if (pixelFormat == "NV12") {
        return cv::VideoWriter::fourcc('N', 'V', '1', '2');
    }
    if (pixelFormat == "I420") {
        return cv::VideoWriter::fourcc('I', '4', '2', '0');
    }
    return 0;
}

std::string pixelFormatFromFourcc(const int fourcc) {
    if (fourcc == 0) {
        return "Auto";
    }
    std::string value(4, '\0');
    value[0] = static_cast<char>(fourcc & 0xFF);
    value[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    value[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    value[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    return value;
}

} // namespace

std::string toString(const CameraBackendPreference backend) {
    switch (backend) {
    case CameraBackendPreference::DirectShow:
        return "DSHOW";
    case CameraBackendPreference::MediaFoundation:
        return "MSMF";
    case CameraBackendPreference::Any:
        return "ANY";
    }
    return "DSHOW";
}

CameraBackendPreference cameraBackendFromString(const std::string& value) {
    if (value == "MSMF") {
        return CameraBackendPreference::MediaFoundation;
    }
    if (value == "ANY") {
        return CameraBackendPreference::Any;
    }
    return CameraBackendPreference::DirectShow;
}

CameraManager::~CameraManager() {
    close();
}

bool CameraManager::open(const CameraConfig& config) {
    close();
    config_ = config;

    struct BackendChoice {
        int api;
        const char* name;
    };

    std::array<BackendChoice, 3> choices{};
    switch (config.preferredBackend) {
    case CameraBackendPreference::DirectShow:
        choices = {{{cv::CAP_DSHOW, "DirectShow"},
                    {cv::CAP_MSMF, "Media Foundation"},
                    {cv::CAP_ANY, "Auto"}}};
        break;
    case CameraBackendPreference::MediaFoundation:
        choices = {{{cv::CAP_MSMF, "Media Foundation"},
                    {cv::CAP_DSHOW, "DirectShow"},
                    {cv::CAP_ANY, "Auto"}}};
        break;
    case CameraBackendPreference::Any:
        choices = {{{cv::CAP_ANY, "Auto"},
                    {cv::CAP_DSHOW, "DirectShow"},
                    {cv::CAP_MSMF, "Media Foundation"}}};
        break;
    }

    for (const auto& choice : choices) {
        if (tryOpen(choice.api, choice.name, config)) {
            return true;
        }
    }

    std::cerr << "カメラ " << config.deviceIndex << " を開けませんでした。\n";
    return false;
}

bool CameraManager::tryOpen(
    const int backendApi,
    const std::string& backendName,
    const CameraConfig& config) {
    capture_.release();
    try {
        if (!capture_.open(config.deviceIndex, backendApi)) {
            return false;
        }

        // USB帯域を抑えて1080p/60fpsを選びやすくする。未対応カメラではsetが失敗するだけで継続できる。
        const int requestedFourcc = fourccForPixelFormat(config.requestedPixelFormat);
        if (requestedFourcc != 0) {
            capture_.set(cv::CAP_PROP_FOURCC, requestedFourcc);
        } else if (config.preferMjpeg) {
            capture_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        }
        capture_.set(cv::CAP_PROP_FRAME_WIDTH, config.requestedWidth);
        capture_.set(cv::CAP_PROP_FRAME_HEIGHT, config.requestedHeight);
        capture_.set(cv::CAP_PROP_FPS, config.requestedFps);
        capture_.set(cv::CAP_PROP_BUFFERSIZE, 1.0);

        cv::Mat initialFrame;
        if (!capture_.read(initialFrame) || initialFrame.empty()) {
            std::cerr << backendName
                      << " はカメラを開きましたが、最初のフレームを取得できませんでした。\n";
            capture_.release();
            return false;
        }

        const double reportedWidth = capture_.get(cv::CAP_PROP_FRAME_WIDTH);
        const double reportedHeight = capture_.get(cv::CAP_PROP_FRAME_HEIGHT);
        const int width = std::isfinite(reportedWidth)
                              ? std::max(1, static_cast<int>(reportedWidth))
                              : initialFrame.cols;
        const int height = std::isfinite(reportedHeight)
                               ? std::max(1, static_cast<int>(reportedHeight))
                               : initialFrame.rows;
        actualFrameSize_ = initialFrame.size();
        actualFps_ = capture_.get(cv::CAP_PROP_FPS);
        actualPixelFormat_ = pixelFormatFromFourcc(
            static_cast<int>(capture_.get(cv::CAP_PROP_FOURCC)));
        activeBackendName_ = backendName;
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            latestFrame_ = std::move(initialFrame);
            ++latestSequence_;
        }
        opened_.store(true, std::memory_order_release);
        connected_.store(true, std::memory_order_release);

        std::cout << "カメラ " << config.deviceIndex << " を " << backendName << " で開きました: "
                  << actualFrameSize_.width << 'x' << actualFrameSize_.height
                  << " @ " << actualFps_ << " fps"
                  << " (reported " << width << 'x' << height << ")\n";
        return true;
    } catch (const cv::Exception& error) {
        std::cerr << backendName << " の初期化に失敗しました: " << error.what() << '\n';
        capture_.release();
        opened_.store(false, std::memory_order_release);
        return false;
    }
}

bool CameraManager::start() {
    if (!opened_.load(std::memory_order_acquire)) {
        return false;
    }
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    connected_.store(true, std::memory_order_release);
    captureThread_ = std::thread(&CameraManager::captureLoop, this);
    return true;
}

void CameraManager::stop() {
    running_.store(false, std::memory_order_release);
    frameAvailable_.notify_all();
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
}

void CameraManager::close() {
    stop();
    capture_.release();
    opened_.store(false, std::memory_order_release);
    connected_.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(frameMutex_);
    latestFrame_.release();
    latestSequence_ = 0;
}

bool CameraManager::waitForLatest(
    cv::Mat& destination,
    std::uint64_t& lastSequence,
    const std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(frameMutex_);
    const bool ready = frameAvailable_.wait_for(lock, timeout, [this, lastSequence] {
        return latestSequence_ != lastSequence || !running_.load(std::memory_order_acquire);
    });

    if (!ready || latestSequence_ == lastSequence || latestFrame_.empty()) {
        return false;
    }

    std::swap(destination, latestFrame_);
    lastSequence = latestSequence_;
    return true;
}

bool CameraManager::isOpen() const {
    return opened_.load(std::memory_order_acquire);
}

bool CameraManager::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool CameraManager::isConnected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

cv::Size CameraManager::actualFrameSize() const noexcept {
    return actualFrameSize_;
}

double CameraManager::actualFps() const noexcept {
    return actualFps_;
}

const std::string& CameraManager::actualPixelFormat() const noexcept {
    return actualPixelFormat_;
}

const std::string& CameraManager::activeBackendName() const noexcept {
    return activeBackendName_;
}

void CameraManager::captureLoop() {
    cv::Mat captureBuffer;
    int consecutiveFailures = 0;

    while (running_.load(std::memory_order_acquire)) {
        bool readSucceeded = false;
        try {
            readSucceeded = capture_.read(captureBuffer) && !captureBuffer.empty();
        } catch (const cv::Exception&) {
            readSucceeded = false;
        }

        if (!readSucceeded) {
            ++consecutiveFailures;
            if (consecutiveFailures >= MaxConsecutiveReadFailures) {
                connected_.store(false, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        consecutiveFailures = 0;
        connected_.store(true, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            std::swap(latestFrame_, captureBuffer);
            ++latestSequence_;
        }
        frameAvailable_.notify_one();
    }

    connected_.store(false, std::memory_order_release);
    frameAvailable_.notify_all();
}

} // namespace wpc
