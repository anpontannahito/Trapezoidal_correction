#include "RecordingOutput.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <utility>

namespace wpc {

namespace {

using Microsoft::WRL::ComPtr;

std::string hresultText(const HRESULT result) {
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << static_cast<std::uint32_t>(result);
    return stream.str();
}

void frameRateRatio(const double fps, UINT32& numerator, UINT32& denominator) {
    if (std::abs(fps - 29.97) < 0.02) {
        numerator = 30'000;
        denominator = 1'001;
        return;
    }
    if (std::abs(fps - 59.94) < 0.02) {
        numerator = 60'000;
        denominator = 1'001;
        return;
    }
    numerator = static_cast<UINT32>(std::max(1.0, std::round(fps * 1'000.0)));
    denominator = 1'000;
    const UINT32 divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
}

HRESULT createSinkWriter(
    const std::filesystem::path& path,
    const VideoOutputConfig& config,
    ComPtr<IMFSinkWriter>& writer,
    DWORD& streamIndex) {
    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 3);
    if (FAILED(result) ||
        FAILED(result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE)) ||
        FAILED(result = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE)) ||
        FAILED(result = attributes->SetUINT32(MF_LOW_LATENCY, TRUE))) {
        return result;
    }

    result = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer);
    if (FAILED(result)) {
        return result;
    }

    UINT32 rateNumerator = 0;
    UINT32 rateDenominator = 0;
    frameRateRatio(config.framesPerSecond, rateNumerator, rateDenominator);
    const auto pixelCount = static_cast<std::uint64_t>(config.frameSize.width) * config.frameSize.height;
    const UINT32 bitRate = static_cast<UINT32>(std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(pixelCount * config.framesPerSecond * 0.14),
        4'000'000ULL,
        80'000'000ULL));

    ComPtr<IMFMediaType> outputType;
    result = MFCreateMediaType(&outputType);
    if (FAILED(result) ||
        FAILED(result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
        FAILED(result = outputType->SetUINT32(MF_MT_AVG_BITRATE, bitRate)) ||
        FAILED(result = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(result = MFSetAttributeSize(
                   outputType.Get(), MF_MT_FRAME_SIZE,
                   static_cast<UINT32>(config.frameSize.width),
                   static_cast<UINT32>(config.frameSize.height))) ||
        FAILED(result = MFSetAttributeRatio(
                   outputType.Get(), MF_MT_FRAME_RATE, rateNumerator, rateDenominator)) ||
        FAILED(result = MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
        FAILED(result = writer->AddStream(outputType.Get(), &streamIndex))) {
        return result;
    }

    const UINT32 rowBytes = static_cast<UINT32>(config.frameSize.width) * 4U;
    const UINT32 sampleBytes = rowBytes * static_cast<UINT32>(config.frameSize.height);
    ComPtr<IMFMediaType> inputType;
    result = MFCreateMediaType(&inputType);
    if (FAILED(result) ||
        FAILED(result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(result = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(result = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(result = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE)) ||
        FAILED(result = inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE)) ||
        FAILED(result = inputType->SetUINT32(MF_MT_SAMPLE_SIZE, sampleBytes)) ||
        FAILED(result = inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, rowBytes)) ||
        FAILED(result = MFSetAttributeSize(
                   inputType.Get(), MF_MT_FRAME_SIZE,
                   static_cast<UINT32>(config.frameSize.width),
                   static_cast<UINT32>(config.frameSize.height))) ||
        FAILED(result = MFSetAttributeRatio(
                   inputType.Get(), MF_MT_FRAME_RATE, rateNumerator, rateDenominator)) ||
        FAILED(result = MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
        FAILED(result = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr))) {
        return result;
    }
    return writer->BeginWriting();
}

HRESULT writeFrame(
    IMFSinkWriter* writer,
    const DWORD streamIndex,
    const cv::Mat& bgraFrame,
    const LONGLONG sampleTime,
    const LONGLONG sampleDuration) {
    const DWORD rowBytes = static_cast<DWORD>(bgraFrame.cols) * 4U;
    const DWORD sampleBytes = rowBytes * static_cast<DWORD>(bgraFrame.rows);
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateAlignedMemoryBuffer(sampleBytes, MF_64_BYTE_ALIGNMENT, &buffer);
    if (FAILED(result)) {
        return result;
    }

    BYTE* destination = nullptr;
    DWORD maximumLength = 0;
    result = buffer->Lock(&destination, &maximumLength, nullptr);
    if (FAILED(result)) {
        return result;
    }
    if (maximumLength < sampleBytes) {
        buffer->Unlock();
        return MF_E_BUFFERTOOSMALL;
    }
    if (bgraFrame.isContinuous() && bgraFrame.step == rowBytes) {
        std::memcpy(destination, bgraFrame.ptr(), sampleBytes);
    } else {
        for (int row = 0; row < bgraFrame.rows; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * rowBytes,
                        bgraFrame.ptr(row), rowBytes);
        }
    }
    const HRESULT unlockResult = buffer->Unlock();
    if (FAILED(unlockResult)) {
        return unlockResult;
    }
    result = buffer->SetCurrentLength(sampleBytes);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IMFSample> sample;
    result = MFCreateSample(&sample);
    if (FAILED(result) ||
        FAILED(result = sample->AddBuffer(buffer.Get())) ||
        FAILED(result = sample->SetSampleTime(sampleTime)) ||
        FAILED(result = sample->SetSampleDuration(sampleDuration))) {
        return result;
    }
    return writer->WriteSample(streamIndex, sample.Get());
}

} // namespace

class RecordingOutput::Impl {
public:
    ~Impl() {
        stop();
    }

    bool configure(const VideoOutputConfig& config) {
        if (config.frameSize.width <= 0 || config.frameSize.height <= 0 ||
            (config.frameSize.width & 1) != 0 || (config.frameSize.height & 1) != 0 ||
            !std::isfinite(config.framesPerSecond) || config.framesPerSecond <= 0.0) {
            setStatus("H.264 requires a positive, even-sized frame");
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) {
            status_ = "Stop recording before changing its format";
            return false;
        }
        config_ = config;
        status_ = "Ready";
        return true;
    }

    void setOutputPath(std::filesystem::path outputPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load(std::memory_order_acquire)) {
            outputPath_ = std::move(outputPath);
        }
    }

    bool start() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }
        if (outputPath_.empty()) {
            status_ = "Recording path is empty";
            return false;
        }
        if (config_.frameSize.width <= 0 || config_.frameSize.height <= 0) {
            status_ = "Recording is not configured";
            return false;
        }

        std::error_code filesystemError;
        const auto parent = outputPath_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, filesystemError);
        }
        if (filesystemError) {
            status_ = "Cannot create recording directory: " + filesystemError.message();
            return false;
        }

        stopRequested_ = false;
        initializationFinished_ = false;
        initializationSucceeded_ = false;
        latestSequence_ = 0;
        consumedSequence_ = 0;
        running_.store(true, std::memory_order_release);
        worker_ = std::thread(&Impl::workerLoop, this);
        initializedCondition_.wait(lock, [this] { return initializationFinished_; });
        const bool succeeded = initializationSucceeded_;
        lock.unlock();
        if (!succeeded && worker_.joinable()) {
            worker_.join();
        }
        return succeeded;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
        }
        frameCondition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        running_.store(false, std::memory_order_release);
    }

    void submitFrame(const cv::Mat& correctedBgrFrame) {
        if (!running_.load(std::memory_order_acquire) || correctedBgrFrame.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load(std::memory_order_relaxed) || stopRequested_) {
            return;
        }
        if (correctedBgrFrame.size() != config_.frameSize || correctedBgrFrame.type() != CV_8UC3) {
            status_ = "Unexpected recording frame format";
            return;
        }
        try {
            correctedBgrFrame.copyTo(latestFrame_);
            ++latestSequence_;
        } catch (const cv::Exception&) {
            status_ = "Recording frame copy failed";
            return;
        }
        frameCondition_.notify_one();
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

    [[nodiscard]] std::filesystem::path outputPath() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return outputPath_;
    }

private:
    void setStatus(std::string status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = std::move(status);
    }

    void completeInitialization(const bool succeeded, std::string status) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            initializationSucceeded_ = succeeded;
            initializationFinished_ = true;
            status_ = std::move(status);
            if (!succeeded) {
                running_.store(false, std::memory_order_release);
            }
        }
        initializedCondition_.notify_all();
    }

    void workerLoop() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitializeCom = SUCCEEDED(comResult);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            completeInitialization(false, "COM initialization failed: " + hresultText(comResult));
            return;
        }

        HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(result)) {
            completeInitialization(false, "Media Foundation startup failed: " + hresultText(result));
            if (uninitializeCom) {
                CoUninitialize();
            }
            return;
        }

        VideoOutputConfig localConfig;
        std::filesystem::path localPath;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            localConfig = config_;
            localPath = outputPath_;
        }
        ComPtr<IMFSinkWriter> writer;
        DWORD streamIndex = 0;
        result = createSinkWriter(localPath, localConfig, writer, streamIndex);
        if (FAILED(result)) {
            completeInitialization(false, "H.264 writer initialization failed: " + hresultText(result));
            writer.Reset();
            MFShutdown();
            if (uninitializeCom) {
                CoUninitialize();
            }
            return;
        }

        completeInitialization(true, "Recording: " + localPath.filename().string());
        UINT32 rateNumerator = 0;
        UINT32 rateDenominator = 0;
        frameRateRatio(localConfig.framesPerSecond, rateNumerator, rateDenominator);
        const LONGLONG sampleDuration =
            static_cast<LONGLONG>(10'000'000ULL * rateDenominator / rateNumerator);
        LONGLONG sampleTime = 0;
        cv::Mat frame;
        cv::Mat bgraFrame(localConfig.frameSize, CV_8UC4);
        std::chrono::steady_clock::time_point lastWritten{};
        const auto minimumInterval = std::chrono::duration<double>(1.0 / localConfig.framesPerSecond);

        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                frameCondition_.wait(lock, [this] {
                    return stopRequested_ || latestSequence_ != consumedSequence_;
                });
                if (stopRequested_) {
                    break;
                }
                std::swap(frame, latestFrame_);
                consumedSequence_ = latestSequence_;
            }

            const auto now = std::chrono::steady_clock::now();
            if (lastWritten.time_since_epoch().count() != 0 && now - lastWritten < minimumInterval) {
                continue;
            }
            try {
                cv::cvtColor(frame, bgraFrame, cv::COLOR_BGR2BGRA);
            } catch (const cv::Exception&) {
                setStatus("Recording color conversion failed");
                break;
            }
            result = writeFrame(writer.Get(), streamIndex, bgraFrame, sampleTime, sampleDuration);
            if (FAILED(result)) {
                setStatus("H.264 frame write failed: " + hresultText(result));
                break;
            }
            sampleTime += sampleDuration;
            lastWritten = now;
        }

        const HRESULT finalizeResult = writer->Finalize();
        writer.Reset();
        if (FAILED(finalizeResult) && SUCCEEDED(result)) {
            result = finalizeResult;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (FAILED(result)) {
                status_ = "Recording stopped with error: " + hresultText(result);
            } else {
                status_ = "Saved: " + localPath.filename().string();
            }
            running_.store(false, std::memory_order_release);
        }
        MFShutdown();
        if (uninitializeCom) {
            CoUninitialize();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable frameCondition_;
    std::condition_variable initializedCondition_;
    std::thread worker_;
    VideoOutputConfig config_;
    std::filesystem::path outputPath_;
    cv::Mat latestFrame_;
    std::uint64_t latestSequence_ = 0;
    std::uint64_t consumedSequence_ = 0;
    bool stopRequested_ = false;
    bool initializationFinished_ = false;
    bool initializationSucceeded_ = false;
    std::string status_ = "Stopped";
    std::atomic<bool> running_{false};
};

RecordingOutput::RecordingOutput() : impl_(std::make_unique<Impl>()) {}
RecordingOutput::~RecordingOutput() = default;

bool RecordingOutput::configure(const VideoOutputConfig& config) {
    return impl_->configure(config);
}

void RecordingOutput::setOutputPath(std::filesystem::path outputPath) {
    impl_->setOutputPath(std::move(outputPath));
}

bool RecordingOutput::start() {
    return impl_->start();
}

void RecordingOutput::stop() {
    impl_->stop();
}

void RecordingOutput::submitFrame(const cv::Mat& correctedBgrFrame) {
    impl_->submitFrame(correctedBgrFrame);
}

bool RecordingOutput::isRunning() const noexcept {
    return impl_->isRunning();
}

std::string RecordingOutput::name() const {
    return "H.264 Recording";
}

std::string RecordingOutput::statusText() const {
    return impl_->statusText();
}

VideoOutputConfig RecordingOutput::config() const {
    return impl_->config();
}

std::filesystem::path RecordingOutput::outputPath() const {
    return impl_->outputPath();
}

} // namespace wpc
