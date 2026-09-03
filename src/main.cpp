#include "Application.h"
#include "CameraManager.h"
#include "CameraDeviceEnumerator.h"
#include "VirtualCameraOutput.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <opencv2/core.hpp>

#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

namespace {

bool readVirtualCameraWithSourceReader(
    cv::Size& frameSize,
    HRESULT& failure,
    int& failureStage) {
    using Microsoft::WRL::ComPtr;

    failureStage = 1;
    ComPtr<IMFAttributes> attributes;
    failure = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(failure)) {
        failure = attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }

    IMFActivate** rawDevices = nullptr;
    UINT32 deviceCount = 0;
    if (SUCCEEDED(failure)) {
        failureStage = 2;
        failure = MFEnumDeviceSources(
            attributes.Get(), &rawDevices, &deviceCount);
    }

    ComPtr<IMFActivate> selectedActivation;
    for (UINT32 index = 0; SUCCEEDED(failure) && index < deviceCount; ++index) {
        wchar_t* rawName = nullptr;
        UINT32 nameLength = 0;
        const HRESULT nameResult = rawDevices[index]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
            &rawName,
            &nameLength);
        if (SUCCEEDED(nameResult) && rawName != nullptr &&
            std::wstring(rawName, nameLength).find(L"Perspective Camera") !=
                std::wstring::npos) {
            selectedActivation = rawDevices[index];
        }
        CoTaskMemFree(rawName);
        rawDevices[index]->Release();
        rawDevices[index] = nullptr;
    }
    CoTaskMemFree(rawDevices);

    if (FAILED(failure)) {
        return false;
    }
    if (!selectedActivation) {
        failure = MF_E_NOT_FOUND;
        return false;
    }

    failureStage = 3;
    ComPtr<IMFMediaSource> mediaSource;
    failure = selectedActivation->ActivateObject(IID_PPV_ARGS(&mediaSource));
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(failure)) {
        failureStage = 4;
        failure = MFCreateSourceReaderFromMediaSource(
            mediaSource.Get(), nullptr, &reader);
    }
    ComPtr<IMFMediaType> nativeType;
    const DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    if (SUCCEEDED(failure)) {
        failureStage = 41;
        failure = reader->GetNativeMediaType(videoStream, 0, &nativeType);
    }
    if (SUCCEEDED(failure)) {
        failureStage = 42;
        failure = reader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    }
    if (SUCCEEDED(failure)) {
        failureStage = 43;
        failure = reader->SetCurrentMediaType(
            videoStream, nullptr, nativeType.Get());
    }
    if (SUCCEEDED(failure)) {
        failureStage = 44;
        failure = reader->SetStreamSelection(videoStream, TRUE);
    }

    DWORD actualStream = 0;
    DWORD streamFlags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(failure)) {
        failureStage = 5;
        failure = reader->ReadSample(
            videoStream,
            0,
            &actualStream,
            &streamFlags,
            &timestamp,
            &sample);
    }
    if (SUCCEEDED(failure) &&
        ((streamFlags & MF_SOURCE_READERF_ERROR) != 0 || !sample)) {
        failure = E_UNEXPECTED;
    }

    if (SUCCEEDED(failure)) {
        failureStage = 6;
        ComPtr<IMFMediaType> currentType;
        failure = reader->GetCurrentMediaType(actualStream, &currentType);
        UINT32 width = 0;
        UINT32 height = 0;
        if (SUCCEEDED(failure)) {
            failure = MFGetAttributeSize(
                currentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
        }
        if (SUCCEEDED(failure)) {
            frameSize = {
                static_cast<int>(width),
                static_cast<int>(height)};
        }
    }

    reader.Reset();
    if (mediaSource) {
        mediaSource->Shutdown();
    }
    selectedActivation->ShutdownObject();
    if (SUCCEEDED(failure)) {
        failureStage = 0;
    }
    return SUCCEEDED(failure);
}

} // namespace

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    try {
        if (argc >= 2 && std::string(argv[1]) == "--list-cameras") {
            const auto devices = wpc::CameraDeviceEnumerator::enumerate();
            for (const auto& device : devices) {
                std::cout << "Camera " << device.captureIndex << ": "
                          << device.friendlyName << '\n';
                for (const auto& mode : device.modes) {
                    std::cout << "  " << mode.displayName() << '\n';
                }
            }
            return devices.empty() ? 1 : 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "--serve-virtual-camera") {
            const int durationSeconds = argc >= 3 ? std::max(1, std::stoi(argv[2])) : 15;
            wpc::VirtualCameraOutput virtualCamera;
            virtualCamera.configure({1280, 720}, 30.0);
            if (!virtualCamera.start()) {
                std::cerr << virtualCamera.statusText() << '\n';
                return 1;
            }
            cv::Mat frame(720, 1280, CV_8UC3, cv::Scalar(32, 96, 160));
            const auto endTime = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(durationSeconds);
            while (std::chrono::steady_clock::now() < endTime) {
                virtualCamera.publish(frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            virtualCamera.stop();
            return 0;
        }
        if (argc >= 2 &&
            (std::string(argv[1]) == "--probe-camera" ||
             std::string(argv[1]) == "--probe-virtual-camera")) {
            const bool allowVirtualCamera = std::string(argv[1]) == "--probe-virtual-camera";
            const int requestedIndex = argc >= 3 ? std::stoi(argv[2]) : 0;
            const auto devices = wpc::CameraDeviceEnumerator::enumerate();
            const wpc::CameraDeviceInfo* selectedDevice = nullptr;
            for (const auto& device : devices) {
                if (device.captureIndex == requestedIndex &&
                    (allowVirtualCamera ||
                     device.friendlyName.find("Perspective Camera") == std::string::npos)) {
                    selectedDevice = &device;
                    break;
                }
            }
            if (selectedDevice == nullptr || selectedDevice->modes.empty()) {
                std::cerr << "指定したカメラまたはネイティブモードが見つかりません。\n";
                return 1;
            }

            const wpc::CameraMode* selectedMode = nullptr;
            double bestScore = std::numeric_limits<double>::max();
            for (const auto& mode : selectedDevice->modes) {
                const double normalScore = std::abs(static_cast<double>(mode.width) - 1920.0) +
                                           std::abs(static_cast<double>(mode.height) - 1080.0);
                const double rotatedScore = std::abs(static_cast<double>(mode.height) - 1920.0) +
                                            std::abs(static_cast<double>(mode.width) - 1080.0);
                const double formatPenalty =
                    mode.pixelFormat == "NV12" || mode.pixelFormat == "MJPG" ||
                            mode.pixelFormat == "YUY2"
                        ? 0.0
                        : 10000.0;
                const double score = std::min(normalScore, rotatedScore) +
                                     std::abs(mode.framesPerSecond() - 30.0) * 20.0 +
                                     formatPenalty;
                if (score < bestScore) {
                    bestScore = score;
                    selectedMode = &mode;
                }
            }

            wpc::CameraConfig config;
            config.deviceIndex = selectedDevice->captureIndex;
            config.requestedWidth = static_cast<int>(selectedMode->width);
            config.requestedHeight = static_cast<int>(selectedMode->height);
            config.requestedFps = selectedMode->framesPerSecond();
            config.requestedPixelFormat = selectedMode->pixelFormat;
            config.preferredBackend = wpc::CameraBackendPreference::MediaFoundation;
            wpc::CameraManager camera;
            if (!camera.open(config) || !camera.start()) {
                std::cerr << "選択したネイティブモードでカメラを開始できませんでした。\n";
                return 1;
            }
            cv::Mat frame;
            std::uint64_t sequence = 0;
            const bool received = camera.waitForLatest(
                frame, sequence, std::chrono::milliseconds(5000));
            camera.close();
            if (!received || frame.empty()) {
                std::cerr << "5秒以内にカメラフレームを取得できませんでした。\n";
                return 1;
            }
            std::cout << "Requested native mode: " << selectedMode->displayName() << '\n'
                      << "Captured frame: " << frame.cols << 'x' << frame.rows << '\n';
            return frame.cols == config.requestedWidth && frame.rows == config.requestedHeight
                       ? 0
                       : 1;
        }
        if (argc >= 2 && std::string(argv[1]) == "--test-virtual-camera") {
            wpc::VirtualCameraOutput virtualCamera;
            virtualCamera.configure({1280, 720}, 30.0);
            if (!virtualCamera.start()) {
                std::cerr << virtualCamera.statusText() << '\n';
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            bool found = false;
            bool sampleReceived = false;
            for (const auto& device : wpc::CameraDeviceEnumerator::enumerate()) {
                if (device.friendlyName.find("Perspective Camera") != std::string::npos) {
                    std::cout << "Virtual camera enumerated: " << device.friendlyName << '\n';
                    found = true;
                    cv::Size sourceReaderFrameSize;
                    HRESULT sourceReaderResult = S_OK;
                    int sourceReaderFailureStage = 0;
                    if (readVirtualCameraWithSourceReader(
                            sourceReaderFrameSize,
                            sourceReaderResult,
                            sourceReaderFailureStage)) {
                        std::cout << "IMFSourceReader sample received: "
                                  << sourceReaderFrameSize.width << 'x'
                                  << sourceReaderFrameSize.height << '\n';
                    } else {
                        std::cerr << "IMFSourceReader stage "
                                  << sourceReaderFailureStage
                                  << " failed: HRESULT 0x"
                                  << std::hex << std::uppercase
                                  << static_cast<unsigned long>(sourceReaderResult)
                                  << std::dec << '\n';
                        const std::string diagnostic =
                            virtualCamera.mediaSourceDiagnosticText();
                        if (!diagnostic.empty()) {
                            std::cerr << diagnostic << '\n';
                        }
                    }
                    wpc::CameraConfig consumerConfig;
                    consumerConfig.deviceIndex = device.captureIndex;
                    consumerConfig.requestedWidth = 1280;
                    consumerConfig.requestedHeight = 720;
                    consumerConfig.requestedFps = 30.0;
                    consumerConfig.requestedPixelFormat = "NV12";
                    consumerConfig.preferredBackend =
                        wpc::CameraBackendPreference::MediaFoundation;
                    wpc::CameraManager consumer;
                    if (consumer.open(consumerConfig) && consumer.start()) {
                        cv::Mat frame;
                        std::uint64_t sequence = 0;
                        sampleReceived = consumer.waitForLatest(
                            frame, sequence, std::chrono::milliseconds(5000));
                        if (sampleReceived && !frame.empty()) {
                            std::cout << "Virtual camera sample received: "
                                      << frame.cols << 'x' << frame.rows << '\n';
                        } else {
                            sampleReceived = false;
                            std::cerr << "Virtual camera sample was not received within 5 seconds.\n";
                        }
                    } else {
                        std::cerr << "Virtual camera could not be opened by an external capture client.\n";
                    }
                    consumer.close();
                    break;
                }
            }
            virtualCamera.stop();
            return found && sampleReceived ? 0 : 1;
        }
        const std::filesystem::path settingsPath =
            argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path("config/settings.json");
        wpc::Application application(settingsPath);
        return application.run();
    } catch (const std::exception& error) {
        std::cerr << "予期しないエラーで終了しました: " << error.what() << '\n';
        return 1;
    }
}
