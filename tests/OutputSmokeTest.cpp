#include "RecordingOutput.h"
#include "SpoutOutput.h"

#include <SpoutDX.h>

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <opencv2/core.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

bool verifyH264File(const std::filesystem::path& path) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        return false;
    }
    HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> mediaType;
    GUID subtype = GUID_NULL;
    if (SUCCEEDED(result)) {
        result = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    }
    if (SUCCEEDED(result)) {
        result = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &mediaType);
    }
    if (SUCCEEDED(result)) {
        result = mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
    }
    mediaType.Reset();
    reader.Reset();
    MFShutdown();
    if (uninitializeCom) {
        CoUninitialize();
    }
    return SUCCEEDED(result) && subtype == MFVideoFormat_H264;
}

} // namespace

int wmain(const int argumentCount, wchar_t* arguments[]) {
    if (argumentCount != 2) {
        std::cerr << "Output directory is required.\n";
        return 2;
    }
    const cv::Size frameSize{320, 240};
    const wpc::VideoOutputConfig config{frameSize, 30.0};
    cv::Mat frame(frameSize, CV_8UC3, cv::Scalar(24, 96, 224));

    wpc::SpoutOutput spout("Perspective Camera Smoke Test");
    if (!spout.configure(config) || !spout.start()) {
        std::cerr << "Spout start failed: " << spout.statusText() << '\n';
        return 1;
    }
    spout.submitFrame(frame);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    spoutDX receiver;
    receiver.SetReceiverName("Perspective Camera Smoke Test");
    std::vector<unsigned char> received(
        static_cast<std::size_t>(frameSize.width) * frameSize.height * 4U);
    bool receivedFrame = false;
    for (int attempt = 0; attempt < 20 && !receivedFrame; ++attempt) {
        spout.submitFrame(frame);
        receivedFrame = receiver.ReceiveImage(
            received.data(),
            static_cast<unsigned int>(frameSize.width),
            static_cast<unsigned int>(frameSize.height),
            false,
            false);
        if (!receivedFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    receiver.ReleaseReceiver();
    receiver.CloseDirectX11();
    spout.stop();
    if (!receivedFrame) {
        std::cerr << "Spout receiver did not receive a frame.\n";
        return 1;
    }

    const std::filesystem::path outputDirectory(arguments[1]);
    const std::filesystem::path recordingPath =
        outputDirectory / (L"recording-smoke-" + std::to_wstring(GetCurrentProcessId()) + L".mp4");
    wpc::RecordingOutput recording;
    recording.setOutputPath(recordingPath);
    if (!recording.configure(config) || !recording.start()) {
        std::cerr << "Recording start failed: " << recording.statusText() << '\n';
        return 1;
    }
    for (int index = 0; index < 30; ++index) {
        frame.setTo(cv::Scalar(index * 5 % 255, 96, 224));
        recording.submitFrame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
    }
    recording.stop();
    std::error_code filesystemError;
    const auto recordedBytes = std::filesystem::file_size(recordingPath, filesystemError);
    if (filesystemError || recordedBytes < 1'024 || !verifyH264File(recordingPath)) {
        std::cerr << "H.264 recording validation failed: " << recording.statusText() << '\n';
        return 1;
    }

    std::cout << "Spout frame received; H.264 recording verified: "
              << recordingPath.string() << " (" << recordedBytes << " bytes)\n";
    return 0;
}
