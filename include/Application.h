#pragma once

#include "CameraManager.h"
#include "CameraDeviceEnumerator.h"
#include "FrameProcessor.h"
#include "OutputManager.h"
#include "PerspectiveCorrector.h"
#include "SettingsManager.h"

#include <opencv2/core.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wpc {

class Application {
public:
    explicit Application(std::filesystem::path settingsPath);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    static constexpr const char* WindowName = "Webcam Perspective Correction";

    static void mouseCallbackThunk(int event, int x, int y, int flags, void* userData);
    void onMouse(int event, int x, int y, int flags);

    bool initialize();
    void shutdown();
    void render(const ProcessingStats& stats);
    bool switchToNextCamera();
    bool switchToNextMode();
    bool restartCamera(const CameraConfig& config);
    void toggleExternalOutput();
    void cycleOutputMethod();
    void toggleRecording();
    void enumerateCameraModes();
    void selectBestAvailableMode();
    void updateSettingsFromRuntime();

    static cv::Rect fitRect(cv::Size imageSize, const cv::Rect& bounds);
    cv::Point mapSourcePointToDisplay(cv::Point2f point) const;
    cv::Point2f mapDisplayPointToSource(int x, int y) const;

    SettingsManager settingsManager_;
    AppSettings settings_;
    CameraManager camera_;
    OutputManager outputManager_;
    std::unique_ptr<PerspectiveCorrector> corrector_;
    std::unique_ptr<FrameProcessor> processor_;

    cv::Mat sourceFrame_;
    cv::Mat correctedFrame_;
    cv::Mat canvas_;
    cv::Rect sourceImageRect_;
    cv::Rect externalOutputButtonRect_;
    cv::Rect outputMethodButtonRect_;
    cv::Rect recordingButtonRect_;
    cv::Rect cameraButtonRect_;
    cv::Rect modeButtonRect_;
    std::vector<CameraDeviceInfo> cameraDevices_;
    std::vector<CameraMode> currentCameraModes_;
    std::uint64_t displayedSequence_ = 0;
    int draggedPoint_ = -1;
    bool initialized_ = false;
    bool pendingExternalOutputToggle_ = false;
    bool pendingOutputMethodCycle_ = false;
    bool pendingRecordingToggle_ = false;
    bool pendingCameraSwitch_ = false;
    bool pendingModeSwitch_ = false;
};

} // namespace wpc
