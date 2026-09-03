#include "OutputManager.h"

#include <Windows.h>
#include <ShlObj.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace wpc {

namespace {

std::filesystem::path resolveRecordingDirectory(const std::filesystem::path& directory) {
    if (directory.is_absolute()) {
        return directory;
    }
    PWSTR videosPath = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &videosPath);
    if (SUCCEEDED(result) && videosPath != nullptr) {
        const std::filesystem::path resolved = std::filesystem::path(videosPath) / directory;
        CoTaskMemFree(videosPath);
        return resolved;
    }
    if (videosPath != nullptr) {
        CoTaskMemFree(videosPath);
    }
    std::error_code error;
    const auto temporaryDirectory = std::filesystem::temp_directory_path(error);
    return error ? directory : temporaryDirectory / "Perspective Camera" / directory;
}

} // namespace

std::string toString(const ExternalOutputMethod method) {
    return method == ExternalOutputMethod::VirtualCamera ? "VirtualCamera" : "Spout";
}

ExternalOutputMethod externalOutputMethodFromString(const std::string& value) {
    return value == "VirtualCamera" ? ExternalOutputMethod::VirtualCamera
                                     : ExternalOutputMethod::Spout;
}

OutputManager::OutputManager() = default;

OutputManager::~OutputManager() {
    shutdown();
}

bool OutputManager::configure(const VideoOutputConfig& config) {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    const bool restartExternal = isExternalOutputRunning();
    const ExternalOutputMethod method = selectedMethod();
    spoutOutput_.stop();
    virtualCameraOutput_.stop();
    if (recordingOutput_.isRunning()) {
        recordingOutput_.stop();
    }

    const bool spoutConfigured = spoutOutput_.configure(config);
    const bool virtualCameraConfigured = virtualCameraOutput_.configure(config);
    const bool recordingConfigured = recordingOutput_.configure(config);
    if (!spoutConfigured || !virtualCameraConfigured || !recordingConfigured) {
        updateNeedsFrames();
        return false;
    }
    config_ = config;
    selectedMethod_.store(method, std::memory_order_release);
    const bool restarted = !restartExternal || selectedOutput().start();
    updateNeedsFrames();
    return restarted;
}

bool OutputManager::startDispatcher() {
    if (dispatcherRunning_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    outputThread_ = std::thread(&OutputManager::outputLoop, this);
    return true;
}

void OutputManager::shutdown() {
    dispatcherRunning_.store(false, std::memory_order_release);
    frameCondition_.notify_all();
    if (outputThread_.joinable()) {
        outputThread_.join();
    }
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    recordingOutput_.stop();
    spoutOutput_.stop();
    virtualCameraOutput_.stop();
    updateNeedsFrames();
}

void OutputManager::submitFrame(const cv::Mat& correctedBgrFrame) {
    if (!needsFrames_.load(std::memory_order_acquire) || correctedBgrFrame.empty()) {
        return;
    }
    try {
        {
            std::lock_guard<std::mutex> frameLock(frameMutex_);
            correctedBgrFrame.copyTo(latestFrame_);
            ++latestSequence_;
        }
        frameCondition_.notify_one();
    } catch (const cv::Exception&) {
        // A transient allocation failure must not terminate camera processing.
    }
}

void OutputManager::setInputConnected(const bool connected) noexcept {
    virtualCameraOutput_.setInputConnected(connected);
}

void OutputManager::setSelectedMethod(const ExternalOutputMethod method) noexcept {
    selectedMethod_.store(method, std::memory_order_release);
}

ExternalOutputMethod OutputManager::selectedMethod() const noexcept {
    return selectedMethod_.load(std::memory_order_acquire);
}

bool OutputManager::startExternalOutput() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    spoutOutput_.stop();
    virtualCameraOutput_.stop();
    const bool started = selectedOutput().start();
    updateNeedsFrames();
    return started;
}

void OutputManager::stopExternalOutput() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    spoutOutput_.stop();
    virtualCameraOutput_.stop();
    updateNeedsFrames();
}

bool OutputManager::isExternalOutputRunning() const noexcept {
    return spoutOutput_.isRunning() || virtualCameraOutput_.isRunning();
}

std::string OutputManager::externalStatusText() const {
    return selectedOutput().statusText();
}

bool OutputManager::startRecording(const std::filesystem::path& directory) {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    if (recordingOutput_.isRunning()) {
        return true;
    }

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm localTime{};
    localtime_s(&localTime, &now);
    std::ostringstream filename;
    filename << "perspective_" << std::put_time(&localTime, "%Y%m%d_%H%M%S") << ".mp4";
    recordingOutput_.setOutputPath(resolveRecordingDirectory(directory) / filename.str());
    const bool started = recordingOutput_.start();
    updateNeedsFrames();
    return started;
}

void OutputManager::stopRecording() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    recordingOutput_.stop();
    updateNeedsFrames();
}

bool OutputManager::isRecording() const noexcept {
    return recordingOutput_.isRunning();
}

std::string OutputManager::recordingStatusText() const {
    return recordingOutput_.statusText();
}

std::filesystem::path OutputManager::recordingPath() const {
    return recordingOutput_.outputPath();
}

VideoOutputConfig OutputManager::config() const {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    return config_;
}

std::string OutputManager::spoutSenderName() const {
    return spoutOutput_.senderName();
}

void OutputManager::outputLoop() {
    cv::Mat frame;
    while (dispatcherRunning_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> frameLock(frameMutex_);
            frameCondition_.wait_for(frameLock, std::chrono::milliseconds(100), [this] {
                return !dispatcherRunning_.load(std::memory_order_relaxed) ||
                       latestSequence_ != consumedSequence_;
            });
            if (!dispatcherRunning_.load(std::memory_order_relaxed)) {
                break;
            }
            if (latestSequence_ == consumedSequence_) {
                continue;
            }
            std::swap(frame, latestFrame_);
            consumedSequence_ = latestSequence_;
        }

        if (spoutOutput_.isRunning()) {
            spoutOutput_.submitFrame(frame);
        }
        if (virtualCameraOutput_.isRunning()) {
            virtualCameraOutput_.submitFrame(frame);
        }
        if (recordingOutput_.isRunning()) {
            recordingOutput_.submitFrame(frame);
        }
        updateNeedsFrames();
    }
}

void OutputManager::updateNeedsFrames() noexcept {
    needsFrames_.store(
        spoutOutput_.isRunning() || virtualCameraOutput_.isRunning() ||
            recordingOutput_.isRunning(),
        std::memory_order_release);
}

IVideoOutput& OutputManager::selectedOutput() noexcept {
    return selectedMethod() == ExternalOutputMethod::VirtualCamera
               ? static_cast<IVideoOutput&>(virtualCameraOutput_)
               : static_cast<IVideoOutput&>(spoutOutput_);
}

const IVideoOutput& OutputManager::selectedOutput() const noexcept {
    return selectedMethod() == ExternalOutputMethod::VirtualCamera
               ? static_cast<const IVideoOutput&>(virtualCameraOutput_)
               : static_cast<const IVideoOutput&>(spoutOutput_);
}

} // namespace wpc
