#include "VirtualCameraOutput.h"

#include "SharedFrameProtocol.h"
#include "VirtualCameraIds.h"

#include <Windows.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <sddl.h>
#include <wrl/client.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <utility>

namespace wpc {

namespace {

using Microsoft::WRL::ComPtr;

std::wstring moduleDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    const std::size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

std::string hresultMessage(const HRESULT result) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    if (length != 0 && message != nullptr) {
        const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (utf8Length > 0) {
            std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(length), utf8.data(), utf8Length, nullptr, nullptr);
            while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == '\n' || utf8.back() == ' ')) {
                utf8.pop_back();
            }
            stream << ": " << utf8;
        }
        LocalFree(message);
    }
    return stream.str();
}

bool isAccessDenied(const HRESULT result) {
    return result == E_ACCESSDENIED ||
           result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
}

HRESULT registerMediaSourceDll() {
    const std::filesystem::path dllPath =
        std::filesystem::path(moduleDirectory()) / virtual_camera_ids::MediaSourceDllName;
    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (module == nullptr) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    using RegisterFunction = HRESULT(STDAPICALLTYPE*)();
    const auto registerFunction = reinterpret_cast<RegisterFunction>(
        GetProcAddress(module, "DllRegisterServer"));
    const HRESULT result = registerFunction == nullptr
                               ? HRESULT_FROM_WIN32(GetLastError())
                               : registerFunction();
    FreeLibrary(module);
    return result;
}

} // namespace

class VirtualCameraOutput::Impl {
public:
    Impl() = default;

    ~Impl() {
        stop();
        if (mappedHeader_ != nullptr) {
            UnmapViewOfFile(mappedHeader_);
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
        }
    }

    bool configure(const cv::Size outputSize, const double framesPerSecond) {
        std::lock_guard<std::mutex> lock(mutex_);
        const int evenWidth = std::clamp(outputSize.width & ~1, 2, static_cast<int>(shared_frame::MaxWidth));
        const int evenHeight = std::clamp(outputSize.height & ~1, 2, static_cast<int>(shared_frame::MaxHeight));
        outputSize_ = {evenWidth, evenHeight};
        outputFps_ = std::clamp(
            std::isfinite(framesPerSecond) ? framesPerSecond : 30.0,
            1.0,
            120.0);
        nextPublishTime_ = {};

        if (mappedHeader_ != nullptr) {
            initializeBlackFramesLocked();
        }
        return true;
    }

    bool start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }
        if (!ensureMappingLocked()) {
            return false;
        }
        initializeBlackFramesLocked();

        HRESULT result = registerMediaSourceDll();
        if (FAILED(result)) {
            lastStatus_ = isAccessDenied(result)
                              ? "Virtual Camera registration requires administrator privileges. "
                                "Run this application as administrator. " +
                                    hresultMessage(result)
                              : "Virtual Camera DLL registration failed: " +
                                    hresultMessage(result);
            return false;
        }

        result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) {
            comInitialized_ = true;
        } else if (result != RPC_E_CHANGED_MODE) {
            lastStatus_ = "COM initialization failed: " + hresultMessage(result);
            return false;
        }

        result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(result)) {
            lastStatus_ = "Media Foundation startup failed: " + hresultMessage(result);
            cleanupRuntimeLocked();
            return false;
        }
        mediaFoundationStarted_ = true;

        result = MFCreateVirtualCamera(
            MFVirtualCameraType_SoftwareCameraSource,
            MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_CurrentUser,
            virtual_camera_ids::FriendlyName,
            virtual_camera_ids::MediaSourceClsidString,
            nullptr,
            0,
            &virtualCamera_);
        if (SUCCEEDED(result)) {
            result = virtualCamera_->Start(nullptr);
        }
        if (FAILED(result)) {
            lastStatus_ = isAccessDenied(result)
                              ? "Virtual Camera setup requires administrator privileges. "
                                "Run this application as administrator. " +
                                    hresultMessage(result)
                              : "Virtual camera start failed: " + hresultMessage(result);
            releaseVirtualCameraLocked();
            cleanupRuntimeLocked();
            return false;
        }

        running_.store(true, std::memory_order_release);
        lastStatus_ = "Running";
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false, std::memory_order_release);
        releaseVirtualCameraLocked();
        cleanupRuntimeLocked();
        lastStatus_ = "Stopped";
    }

    void publish(const cv::Mat& frame) {
        if (!running_.load(std::memory_order_acquire) || frame.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load(std::memory_order_relaxed) || mappedHeader_ == nullptr) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (nextPublishTime_.time_since_epoch().count() != 0 && now < nextPublishTime_) {
            return;
        }
        nextPublishTime_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(1.0 / outputFps_));

        const cv::Mat* conversionSource = &frame;
        if (frame.size() != outputSize_) {
            cv::resize(frame, resizedBgr_, outputSize_, 0.0, 0.0, cv::INTER_LINEAR);
            conversionSource = &resizedBgr_;
        }

        try {
            cv::cvtColor(*conversionSource, i420_, cv::COLOR_BGR2YUV_I420);
        } catch (const cv::Exception&) {
            return;
        }

        const LONG currentSlot = InterlockedCompareExchange(&mappedHeader_->activeSlot, 0, 0);
        const LONG writeSlot = currentSlot == 0 ? 1 : 0;
        InterlockedIncrement64(&mappedHeader_->slotSequence[writeSlot]);

        auto* destination = reinterpret_cast<std::uint8_t*>(
            shared_frame::slotData(mappedHeader_, static_cast<std::size_t>(writeSlot)));
        const std::size_t yBytes = static_cast<std::size_t>(outputSize_.width) * outputSize_.height;
        const std::size_t chromaPlaneBytes = yBytes / 4;
        const auto* sourceBytes = i420_.ptr<std::uint8_t>();
        std::memcpy(destination, sourceBytes, yBytes);
        const std::uint8_t* sourceU = sourceBytes + yBytes;
        const std::uint8_t* sourceV = sourceU + chromaPlaneBytes;
        std::uint8_t* destinationUv = destination + yBytes;
        for (std::size_t index = 0; index < chromaPlaneBytes; ++index) {
            destinationUv[index * 2] = sourceU[index];
            destinationUv[index * 2 + 1] = sourceV[index];
        }

        MemoryBarrier();
        InterlockedIncrement64(&mappedHeader_->slotSequence[writeSlot]);
        InterlockedExchange(&mappedHeader_->activeSlot, writeSlot);
        InterlockedIncrement64(&mappedHeader_->frameSequence);
    }

    void setInputConnected(const bool connected) noexcept {
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
        auto* const header = mappedHeader_;
        if (header != nullptr) {
            InterlockedExchange(&header->connected, connected ? 1 : 0);
        }
    }

    [[nodiscard]] std::string statusText() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastStatus_;
    }

    [[nodiscard]] std::string mediaSourceDiagnosticText() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mappedHeader_ == nullptr) {
            return {};
        }
        const LONG stage = InterlockedCompareExchange(
            &mappedHeader_->lastFailureStage, 0, 0);
        const HRESULT result = static_cast<HRESULT>(InterlockedCompareExchange(
            &mappedHeader_->lastFailureHresult, 0, 0));
        if (stage == 0) {
            return {};
        }
        return "Media Source diagnostic stage " + std::to_string(stage) +
               ": " + hresultMessage(result);
    }

    [[nodiscard]] bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] cv::Size outputSize() const noexcept {
        return outputSize_;
    }

    [[nodiscard]] double outputFramesPerSecond() const noexcept {
        return outputFps_;
    }

private:
    bool ensureMappingLocked() {
        if (mappedHeader_ != nullptr) {
            return true;
        }

        PSECURITY_DESCRIPTOR descriptor = nullptr;
        SECURITY_ATTRIBUTES securityAttributes{};
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)(A;;GA;;;LS)"
                L"(A;;GRGW;;;AC)(A;;GRGW;;;RC)"
                L"S:(ML;;NW;;;LW)",
                SDDL_REVISION_1,
                &descriptor,
                nullptr)) {
            securityAttributes.nLength = sizeof(securityAttributes);
            securityAttributes.lpSecurityDescriptor = descriptor;
            securityAttributes.bInheritHandle = FALSE;
        }

        const std::uint64_t mappingBytes = shared_frame::MappingBytes;
        mapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            descriptor != nullptr ? &securityAttributes : nullptr,
            PAGE_READWRITE,
            static_cast<DWORD>(mappingBytes >> 32U),
            static_cast<DWORD>(mappingBytes & 0xFFFFFFFFU),
            shared_frame::MappingName);
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        if (mapping_ == nullptr) {
            lastStatus_ = "Shared frame mapping failed: " + hresultMessage(HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }

        mappedHeader_ = static_cast<shared_frame::Header*>(
            MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, shared_frame::MappingBytes));
        if (mappedHeader_ == nullptr) {
            lastStatus_ = "Shared frame view failed: " + hresultMessage(HRESULT_FROM_WIN32(GetLastError()));
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        if (mappedHeader_->magic != shared_frame::Magic ||
            mappedHeader_->version != shared_frame::Version) {
            std::memset(mappedHeader_, 0, sizeof(*mappedHeader_));
            mappedHeader_->magic = shared_frame::Magic;
            mappedHeader_->version = shared_frame::Version;
            mappedHeader_->headerBytes = sizeof(*mappedHeader_);
            mappedHeader_->maxFrameBytes = shared_frame::MaxNv12Bytes;
        }
        return true;
    }

    void initializeBlackFramesLocked() {
        if (mappedHeader_ == nullptr) {
            return;
        }
        const LONG width = outputSize_.width;
        const LONG height = outputSize_.height;
        const LONG yBytes = width * height;
        const LONG dataBytes = yBytes * 3 / 2;
        const LONG frameRateNumerator = static_cast<LONG>(std::lround(outputFps_ * 1000.0));
        InterlockedExchange(&mappedHeader_->width, width);
        InterlockedExchange(&mappedHeader_->height, height);
        InterlockedExchange(&mappedHeader_->frameRateNumerator, frameRateNumerator);
        InterlockedExchange(&mappedHeader_->frameRateDenominator, 1000);
        InterlockedExchange(&mappedHeader_->dataBytes, dataBytes);
        InterlockedExchange(&mappedHeader_->connected, 0);
        InterlockedExchange(&mappedHeader_->lastFailureStage, 0);
        InterlockedExchange(&mappedHeader_->lastFailureHresult, S_OK);
        InterlockedExchange(&mappedHeader_->activeSlot, 0);
        InterlockedExchange64(&mappedHeader_->frameSequence, 0);
        for (std::size_t slot = 0; slot < shared_frame::SlotCount; ++slot) {
            auto* data = reinterpret_cast<std::uint8_t*>(shared_frame::slotData(mappedHeader_, slot));
            std::fill(data, data + yBytes, static_cast<std::uint8_t>(16));
            std::fill(data + yBytes, data + dataBytes, static_cast<std::uint8_t>(128));
            InterlockedExchange64(&mappedHeader_->slotSequence[slot], 0);
        }
    }

    void cleanupRuntimeLocked() {
        if (mediaFoundationStarted_) {
            MFShutdown();
            mediaFoundationStarted_ = false;
        }
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
    }

    void releaseVirtualCameraLocked() {
        if (!virtualCamera_) {
            return;
        }
        virtualCamera_->Stop();
        virtualCamera_->Shutdown();
        virtualCamera_.Reset();
    }

    mutable std::mutex mutex_;
    HANDLE mapping_ = nullptr;
    shared_frame::Header* mappedHeader_ = nullptr;
    cv::Size outputSize_{1920, 1080};
    double outputFps_ = 30.0;
    std::chrono::steady_clock::time_point nextPublishTime_{};
    cv::Mat resizedBgr_;
    cv::Mat i420_;
    ComPtr<IMFVirtualCamera> virtualCamera_;
    std::atomic<bool> running_{false};
    bool comInitialized_ = false;
    bool mediaFoundationStarted_ = false;
    std::string lastStatus_ = "Stopped";
};

VirtualCameraOutput::VirtualCameraOutput() : impl_(std::make_unique<Impl>()) {}
VirtualCameraOutput::~VirtualCameraOutput() = default;

bool VirtualCameraOutput::configure(const VideoOutputConfig& config) {
    return impl_->configure(config.frameSize, config.framesPerSecond);
}

bool VirtualCameraOutput::start() {
    return impl_->start();
}

void VirtualCameraOutput::stop() {
    impl_->stop();
}

void VirtualCameraOutput::submitFrame(const cv::Mat& correctedBgrFrame) {
    impl_->publish(correctedBgrFrame);
}

void VirtualCameraOutput::publish(const cv::Mat& correctedBgrFrame) {
    impl_->publish(correctedBgrFrame);
}

void VirtualCameraOutput::setInputConnected(const bool connected) noexcept {
    impl_->setInputConnected(connected);
}

bool VirtualCameraOutput::isRunning() const noexcept {
    return impl_->isRunning();
}

std::string VirtualCameraOutput::name() const {
    return "Virtual Camera";
}

std::string VirtualCameraOutput::statusText() const {
    return impl_->statusText();
}

VideoOutputConfig VirtualCameraOutput::config() const noexcept {
    return {impl_->outputSize(), impl_->outputFramesPerSecond()};
}

std::string VirtualCameraOutput::mediaSourceDiagnosticText() const {
    return impl_->mediaSourceDiagnosticText();
}

cv::Size VirtualCameraOutput::outputSize() const noexcept {
    return impl_->outputSize();
}

double VirtualCameraOutput::outputFramesPerSecond() const noexcept {
    return impl_->outputFramesPerSecond();
}

} // namespace wpc
