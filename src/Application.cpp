#include "Application.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

namespace wpc {

namespace {

constexpr int InitialWindowWidth = 1600;
constexpr int InitialWindowHeight = 720;
constexpr int HeaderHeight = 84;
constexpr int PaneGap = 8;
constexpr int PointRadius = 7;
constexpr int HitRadius = 20;
constexpr int MaxCameraIndices = 8;

bool isCaptureFriendlyFormat(const std::string& format) {
    return format == "MJPG" || format == "NV12" || format == "YUY2" || format == "I420";
}

std::string shortened(const std::string& value, const std::size_t maximumLength) {
    return value.size() <= maximumLength ? value : value.substr(0, maximumLength - 3) + "...";
}

} // namespace

Application::Application(std::filesystem::path settingsPath)
    : settingsManager_(std::move(settingsPath)) {}

Application::~Application() {
    shutdown();
}

int Application::run() {
    if (!initialize()) {
        return 1;
    }

    ProcessingStats stats;
    bool running = true;
    while (running) {
        if (processor_->takeLatest(sourceFrame_, correctedFrame_, displayedSequence_, stats)) {
            render(stats);
            cv::imshow(WindowName, canvas_);
        }

        const int key = cv::waitKey(1) & 0xFF;
        switch (key) {
        case 27: // Esc
            running = false;
            break;
        case 'r':
        case 'R':
            corrector_->resetPoints();
            break;
        case ' ':
            corrector_->setEnabled(!corrector_->isEnabled());
            break;
        case 'c':
        case 'C':
            switchToNextCamera();
            break;
        case 'm':
        case 'M':
            switchToNextMode();
            break;
        case 's':
        case 'S':
            toggleExternalOutput();
            break;
        case 'o':
        case 'O':
            cycleOutputMethod();
            break;
        case 'g':
        case 'G':
            toggleRecording();
            break;
        case 'v':
        case 'V':
            outputManager_.setSelectedMethod(ExternalOutputMethod::VirtualCamera);
            settings_.outputMethod = toString(ExternalOutputMethod::VirtualCamera);
            toggleExternalOutput();
            break;
        default:
            break;
        }

        if (pendingExternalOutputToggle_) {
            pendingExternalOutputToggle_ = false;
            toggleExternalOutput();
        }
        if (pendingOutputMethodCycle_) {
            pendingOutputMethodCycle_ = false;
            cycleOutputMethod();
        }
        if (pendingRecordingToggle_) {
            pendingRecordingToggle_ = false;
            toggleRecording();
        }
        if (pendingCameraSwitch_) {
            pendingCameraSwitch_ = false;
            switchToNextCamera();
        }
        if (pendingModeSwitch_) {
            pendingModeSwitch_ = false;
            switchToNextMode();
        }

        if (cv::getWindowProperty(WindowName, cv::WND_PROP_VISIBLE) < 1.0) {
            running = false;
        }
    }

    updateSettingsFromRuntime();
    if (!settingsManager_.save(settings_)) {
        std::cerr << "設定の保存に失敗しました: " << settingsManager_.path() << '\n';
    }
    shutdown();
    return 0;
}

void Application::mouseCallbackThunk(
    const int event,
    const int x,
    const int y,
    const int flags,
    void* userData) {
    if (userData != nullptr) {
        static_cast<Application*>(userData)->onMouse(event, x, y, flags);
    }
}

void Application::onMouse(const int event, const int x, const int y, const int flags) {
    if (event == cv::EVENT_LBUTTONDOWN) {
        const cv::Point position{x, y};
        if (externalOutputButtonRect_.contains(position)) {
            pendingExternalOutputToggle_ = true;
            return;
        }
        if (outputMethodButtonRect_.contains(position)) {
            pendingOutputMethodCycle_ = true;
            return;
        }
        if (recordingButtonRect_.contains(position)) {
            pendingRecordingToggle_ = true;
            return;
        }
        if (cameraButtonRect_.contains(position)) {
            pendingCameraSwitch_ = true;
            return;
        }
        if (modeButtonRect_.contains(position)) {
            pendingModeSwitch_ = true;
            return;
        }
    }

    if (!corrector_ || sourceImageRect_.empty()) {
        return;
    }

    if (event == cv::EVENT_LBUTTONDOWN) {
        const auto points = corrector_->points();
        int bestIndex = -1;
        int bestDistanceSquared = HitRadius * HitRadius + 1;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const cv::Point displayedPoint = mapSourcePointToDisplay(points[i]);
            const int dx = displayedPoint.x - x;
            const int dy = displayedPoint.y - y;
            const int distanceSquared = dx * dx + dy * dy;
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestIndex = static_cast<int>(i);
            }
        }
        draggedPoint_ = bestIndex;
    } else if (event == cv::EVENT_MOUSEMOVE && draggedPoint_ >= 0 &&
               (flags & cv::EVENT_FLAG_LBUTTON) != 0) {
        corrector_->setPoint(
            static_cast<std::size_t>(draggedPoint_),
            mapDisplayPointToSource(x, y));
    } else if (event == cv::EVENT_LBUTTONUP) {
        if (draggedPoint_ >= 0) {
            corrector_->setPoint(
                static_cast<std::size_t>(draggedPoint_),
                mapDisplayPointToSource(x, y));
        }
        draggedPoint_ = -1;
    }
}

bool Application::initialize() {
    if (!settingsManager_.load(settings_)) {
        std::cout << "既定設定を使用します。終了時に " << settingsManager_.path() << " へ保存します。\n";
    }

    enumerateCameraModes();
    selectBestAvailableMode();

    if (!camera_.open(settings_.camera)) {
        std::cerr << "Webカメラを利用できないため終了します。\n";
        return false;
    }

    if (settings_.outputFollowsCapture) {
        settings_.outputSize = camera_.actualFrameSize();
    }

    corrector_ = std::make_unique<PerspectiveCorrector>(
        camera_.actualFrameSize(),
        settings_.outputSize,
        settings_.normalizedPoints);
    outputManager_.setSelectedMethod(externalOutputMethodFromString(settings_.outputMethod));
    if (!outputManager_.configure({settings_.outputSize, settings_.outputFps}) ||
        !outputManager_.startDispatcher()) {
        std::cerr << "外部出力マネージャーを初期化できませんでした。\n";
        shutdown();
        return false;
    }
    processor_ = std::make_unique<FrameProcessor>(
        camera_, *corrector_, &outputManager_);

    if (!camera_.start() || !processor_->start()) {
        std::cerr << "映像処理スレッドを開始できませんでした。\n";
        shutdown();
        return false;
    }

    cv::namedWindow(WindowName, cv::WINDOW_NORMAL | cv::WINDOW_FREERATIO);
    cv::resizeWindow(WindowName, InitialWindowWidth, InitialWindowHeight);
    cv::setMouseCallback(WindowName, &Application::mouseCallbackThunk, this);
    initialized_ = true;
    return true;
}

void Application::shutdown() {
    if (processor_) {
        processor_->stop();
    }
    outputManager_.shutdown();
    camera_.close();
    if (initialized_) {
        cv::destroyWindow(WindowName);
        initialized_ = false;
    }
}

void Application::render(const ProcessingStats& stats) {
    int canvasWidth = InitialWindowWidth;
    int canvasHeight = InitialWindowHeight;
    try {
        const cv::Rect windowImageRect = cv::getWindowImageRect(WindowName);
        if (windowImageRect.width >= 320 && windowImageRect.height >= 240) {
            canvasWidth = windowImageRect.width;
            canvasHeight = windowImageRect.height;
        }
    } catch (const cv::Exception&) {
        // getWindowImageRect非対応のHighGUIバックエンドでは初期サイズを使う。
    }

    canvas_.create(canvasHeight, canvasWidth, CV_8UC3);
    canvas_.setTo(cv::Scalar(22, 22, 22));

    const int contentHeight = std::max(1, canvasHeight - HeaderHeight);
    const int paneWidth = std::max(1, (canvasWidth - PaneGap) / 2);
    const cv::Rect leftBounds(0, HeaderHeight, paneWidth, contentHeight);
    const cv::Rect rightBounds(paneWidth + PaneGap, HeaderHeight,
                               std::max(1, canvasWidth - paneWidth - PaneGap), contentHeight);
    sourceImageRect_ = fitRect(sourceFrame_.size(), leftBounds);
    const cv::Rect correctedRect = fitRect(correctedFrame_.size(), rightBounds);

    if (!sourceImageRect_.empty()) {
        cv::Mat sourceRoi = canvas_(sourceImageRect_);
        cv::resize(sourceFrame_, sourceRoi, sourceImageRect_.size(), 0.0, 0.0, cv::INTER_LINEAR);
    }
    if (!correctedRect.empty()) {
        cv::Mat correctedRoi = canvas_(correctedRect);
        cv::resize(correctedFrame_, correctedRoi, correctedRect.size(), 0.0, 0.0, cv::INTER_LINEAR);
    }

    const auto points = corrector_->points();
    const bool valid = corrector_->isGeometryValid();
    const cv::Scalar boundaryColor = valid ? cv::Scalar(60, 230, 60) : cv::Scalar(40, 40, 240);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const cv::Point from = mapSourcePointToDisplay(points[i]);
        const cv::Point to = mapSourcePointToDisplay(points[(i + 1) % points.size()]);
        cv::line(canvas_, from, to, boundaryColor, 2, cv::LINE_AA);
        cv::circle(canvas_, from, PointRadius + 2, cv::Scalar(0, 0, 0), cv::FILLED, cv::LINE_AA);
        cv::circle(canvas_, from, PointRadius, cv::Scalar(0, 220, 255), cv::FILLED, cv::LINE_AA);
        cv::putText(canvas_, std::to_string(i + 1), from + cv::Point(10, -10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    }

    std::string cameraName = "Camera " + std::to_string(settings_.camera.deviceIndex);
    for (const auto& device : cameraDevices_) {
        if (device.captureIndex == settings_.camera.deviceIndex) {
            cameraName = device.friendlyName;
            break;
        }
    }
    std::ostringstream inputStatus;
    inputStatus << "Input: " << shortened(cameraName, 32) << "  "
                << camera_.actualFrameSize().width << 'x' << camera_.actualFrameSize().height
                << " @ " << std::fixed << std::setprecision(1) << camera_.actualFps()
                << " FPS / " << camera_.actualPixelFormat()
                << "    Process: " << stats.framesPerSecond << " FPS";
    cv::putText(canvas_, inputStatus.str(), {10, 22}, cv::FONT_HERSHEY_SIMPLEX,
                0.50, cv::Scalar(235, 235, 235), 1, cv::LINE_AA);

    const VideoOutputConfig outputConfig = outputManager_.config();
    std::ostringstream outputStatus;
    outputStatus << "Output: " << outputConfig.frameSize.width << 'x' << outputConfig.frameSize.height
                 << " @ " << std::fixed << std::setprecision(1)
                 << outputConfig.framesPerSecond << " FPS    "
                 << toString(outputManager_.selectedMethod()) << ": "
                 << outputManager_.externalStatusText()
                 << "    Record: " << outputManager_.recordingStatusText();
    cv::putText(canvas_, outputStatus.str(), {10, 43}, cv::FONT_HERSHEY_SIMPLEX,
                0.50, cv::Scalar(235, 235, 235), 1, cv::LINE_AA);

    externalOutputButtonRect_ = {10, 51, 170, 27};
    outputMethodButtonRect_ = {190, 51, 160, 27};
    recordingButtonRect_ = {360, 51, 170, 27};
    cameraButtonRect_ = {540, 51, 150, 27};
    modeButtonRect_ = {700, 51, 180, 27};
    const auto drawButton = [this](const cv::Rect& rectangle, const std::string& label, const cv::Scalar& color) {
        cv::rectangle(canvas_, rectangle, color, cv::FILLED, cv::LINE_AA);
        cv::rectangle(canvas_, rectangle, cv::Scalar(210, 210, 210), 1, cv::LINE_AA);
        cv::putText(canvas_, label, {rectangle.x + 8, rectangle.y + 19},
                    cv::FONT_HERSHEY_SIMPLEX, 0.47, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    };
    drawButton(
        externalOutputButtonRect_,
        outputManager_.isExternalOutputRunning() ? "Stop Output [S]" : "Start Output [S]",
        outputManager_.isExternalOutputRunning() ? cv::Scalar(55, 75, 150) : cv::Scalar(65, 115, 65));
    drawButton(
        outputMethodButtonRect_,
        "Method: " + toString(outputManager_.selectedMethod()) + " [O]",
        cv::Scalar(90, 75, 55));
    drawButton(
        recordingButtonRect_,
        outputManager_.isRecording() ? "Stop Recording [G]" : "Start Recording [G]",
        outputManager_.isRecording() ? cv::Scalar(55, 75, 150) : cv::Scalar(65, 115, 65));
    drawButton(cameraButtonRect_, "Next Camera [C]", cv::Scalar(90, 75, 55));
    drawButton(modeButtonRect_, "Next Native Mode [M]", cv::Scalar(90, 75, 55));

    const std::string rightLabel = corrector_->isEnabled() ? "Corrected" : "Bypass [Space: OFF]";
    cv::putText(canvas_, rightLabel, {paneWidth + PaneGap + 10, HeaderHeight + 22},
                cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(235, 235, 235), 1, cv::LINE_AA);

    if (!valid) {
        cv::putText(canvas_, "Invalid quadrilateral - using last valid transform",
                    {10, std::max(HeaderHeight + 20, canvasHeight - 12)},
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(50, 80, 255), 1, cv::LINE_AA);
    } else if (!camera_.isConnected()) {
        cv::putText(canvas_, "Camera disconnected - waiting for frames",
                    {10, std::max(HeaderHeight + 20, canvasHeight - 12)},
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(50, 80, 255), 1, cv::LINE_AA);
    }
}

bool Application::switchToNextCamera() {
    const CameraConfig originalConfig = settings_.camera;
    if (!cameraDevices_.empty()) {
        std::size_t currentIndex = 0;
        for (std::size_t index = 0; index < cameraDevices_.size(); ++index) {
            if (cameraDevices_[index].captureIndex == originalConfig.deviceIndex) {
                currentIndex = index;
                break;
            }
        }
        for (std::size_t offset = 1; offset <= cameraDevices_.size(); ++offset) {
            const auto& device = cameraDevices_[(currentIndex + offset) % cameraDevices_.size()];
            settings_.camera.deviceIndex = device.captureIndex;
            currentCameraModes_ = device.modes;
            selectBestAvailableMode();
            const CameraConfig candidate = settings_.camera;
            settings_.camera = originalConfig;
            if (restartCamera(candidate)) {
                return true;
            }
        }
        settings_.camera = originalConfig;
        selectBestAvailableMode();
        return false;
    }

    for (int offset = 1; offset < MaxCameraIndices; ++offset) {
        CameraConfig candidate = originalConfig;
        candidate.deviceIndex = (originalConfig.deviceIndex + offset) % MaxCameraIndices;
        if (restartCamera(candidate)) {
            return true;
        }
    }
    return false;
}

bool Application::switchToNextMode() {
    if (currentCameraModes_.empty()) {
        return false;
    }
    std::size_t currentIndex = 0;
    for (std::size_t index = 0; index < currentCameraModes_.size(); ++index) {
        const auto& mode = currentCameraModes_[index];
        if (static_cast<int>(mode.width) == settings_.camera.requestedWidth &&
            static_cast<int>(mode.height) == settings_.camera.requestedHeight &&
            std::abs(mode.framesPerSecond() - settings_.camera.requestedFps) < 0.01 &&
            mode.pixelFormat == settings_.camera.requestedPixelFormat) {
            currentIndex = index;
            break;
        }
    }
    const auto& mode = currentCameraModes_[(currentIndex + 1) % currentCameraModes_.size()];
    CameraConfig candidate = settings_.camera;
    candidate.requestedWidth = static_cast<int>(mode.width);
    candidate.requestedHeight = static_cast<int>(mode.height);
    candidate.requestedFps = mode.framesPerSecond();
    candidate.requestedPixelFormat = mode.pixelFormat;
    return restartCamera(candidate);
}

bool Application::restartCamera(const CameraConfig& config) {
    const CameraConfig originalConfig = settings_.camera;
    const bool restartExternalOutput = outputManager_.isExternalOutputRunning();
    outputManager_.stopExternalOutput();
    processor_->stop();
    camera_.close();

    auto startConfiguration = [this](const CameraConfig& selectedConfig) {
        if (!camera_.open(selectedConfig)) {
            return false;
        }
        corrector_->updateInputSize(camera_.actualFrameSize());
        if (settings_.outputFollowsCapture) {
            settings_.outputSize = camera_.actualFrameSize();
            corrector_->setOutputSize(settings_.outputSize);
        }
        const VideoOutputConfig currentOutputConfig = outputManager_.config();
        if (currentOutputConfig.frameSize != settings_.outputSize ||
            std::abs(currentOutputConfig.framesPerSecond - settings_.outputFps) > 0.001) {
            if (!outputManager_.configure({settings_.outputSize, settings_.outputFps})) {
                return false;
            }
        }
        return camera_.start() && processor_->start();
    };

    if (!startConfiguration(config)) {
        processor_->stop();
        camera_.close();
        startConfiguration(originalConfig);
        if (restartExternalOutput) {
            outputManager_.startExternalOutput();
        }
        std::cerr << "選択したカメラモードを開始できなかったため、元の設定へ戻しました。\n";
        return false;
    }

    settings_.camera = config;
    if (restartExternalOutput) {
        outputManager_.startExternalOutput();
    }
    return true;
}

void Application::toggleExternalOutput() {
    if (outputManager_.isExternalOutputRunning()) {
        outputManager_.stopExternalOutput();
    } else {
        if (!outputManager_.startExternalOutput()) {
            std::cerr << outputManager_.externalStatusText() << '\n';
        }
    }
}

void Application::cycleOutputMethod() {
    const bool restart = outputManager_.isExternalOutputRunning();
    outputManager_.stopExternalOutput();
    const ExternalOutputMethod next =
        outputManager_.selectedMethod() == ExternalOutputMethod::Spout
            ? ExternalOutputMethod::VirtualCamera
            : ExternalOutputMethod::Spout;
    outputManager_.setSelectedMethod(next);
    settings_.outputMethod = toString(next);
    if (restart && !outputManager_.startExternalOutput()) {
        std::cerr << outputManager_.externalStatusText() << '\n';
    }
}

void Application::toggleRecording() {
    if (outputManager_.isRecording()) {
        outputManager_.stopRecording();
        std::cout << outputManager_.recordingStatusText() << '\n';
    } else if (!outputManager_.startRecording(settings_.recordingDirectory)) {
        std::cerr << outputManager_.recordingStatusText() << '\n';
    }
}

void Application::enumerateCameraModes() {
    cameraDevices_ = CameraDeviceEnumerator::enumerate();
    cameraDevices_.erase(
        std::remove_if(cameraDevices_.begin(), cameraDevices_.end(), [](const CameraDeviceInfo& device) {
            return device.friendlyName.find("Perspective Camera") != std::string::npos;
        }),
        cameraDevices_.end());
    for (const auto& device : cameraDevices_) {
        std::cout << "Camera " << device.captureIndex << ": " << device.friendlyName << '\n';
        for (const auto& mode : device.modes) {
            std::cout << "  " << mode.displayName() << '\n';
        }
    }
}

void Application::selectBestAvailableMode() {
    currentCameraModes_.clear();
    for (const auto& device : cameraDevices_) {
        if (device.captureIndex == settings_.camera.deviceIndex) {
            currentCameraModes_ = device.modes;
            break;
        }
    }
    if (currentCameraModes_.empty()) {
        return;
    }

    const CameraMode* bestMode = nullptr;
    double bestScore = std::numeric_limits<double>::max();
    for (const auto& mode : currentCameraModes_) {
        const double normalDimensionScore =
            std::abs(static_cast<double>(mode.width) - settings_.camera.requestedWidth) +
            std::abs(static_cast<double>(mode.height) - settings_.camera.requestedHeight);
        const double rotatedDimensionScore =
            std::abs(static_cast<double>(mode.height) - settings_.camera.requestedWidth) +
            std::abs(static_cast<double>(mode.width) - settings_.camera.requestedHeight);
        const double dimensionScore = std::min(normalDimensionScore, rotatedDimensionScore);
        const double fpsScore = std::abs(mode.framesPerSecond() - settings_.camera.requestedFps) * 20.0;
        const double formatPenalty = mode.pixelFormat == settings_.camera.requestedPixelFormat
                                         ? 0.0
                                         : (isCaptureFriendlyFormat(mode.pixelFormat) ? 100.0 : 10000.0);
        const double score = dimensionScore + fpsScore + formatPenalty;
        if (score < bestScore) {
            bestScore = score;
            bestMode = &mode;
        }
    }
    if (bestMode != nullptr) {
        settings_.camera.requestedWidth = static_cast<int>(bestMode->width);
        settings_.camera.requestedHeight = static_cast<int>(bestMode->height);
        settings_.camera.requestedFps = bestMode->framesPerSecond();
        settings_.camera.requestedPixelFormat = bestMode->pixelFormat;
        settings_.camera.preferredBackend = CameraBackendPreference::MediaFoundation;
    }
}

void Application::updateSettingsFromRuntime() {
    if (!corrector_) {
        return;
    }
    settings_.normalizedPoints = corrector_->normalizedPoints();
    settings_.outputSize = corrector_->outputSize();
    settings_.outputFps = outputManager_.config().framesPerSecond;
    settings_.outputMethod = toString(outputManager_.selectedMethod());
}

cv::Rect Application::fitRect(const cv::Size imageSize, const cv::Rect& bounds) {
    if (imageSize.width <= 0 || imageSize.height <= 0 ||
        bounds.width <= 0 || bounds.height <= 0) {
        return {};
    }

    const double scale = std::min(
        static_cast<double>(bounds.width) / static_cast<double>(imageSize.width),
        static_cast<double>(bounds.height) / static_cast<double>(imageSize.height));
    const int width = std::max(1, static_cast<int>(std::lround(imageSize.width * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(imageSize.height * scale)));
    return {
        bounds.x + (bounds.width - width) / 2,
        bounds.y + (bounds.height - height) / 2,
        std::min(width, bounds.width),
        std::min(height, bounds.height)};
}

cv::Point Application::mapSourcePointToDisplay(const cv::Point2f point) const {
    const cv::Size inputSize = corrector_->inputSize();
    const float denominatorX = static_cast<float>(std::max(1, inputSize.width - 1));
    const float denominatorY = static_cast<float>(std::max(1, inputSize.height - 1));
    return {
        sourceImageRect_.x + static_cast<int>(std::lround(point.x / denominatorX *
                                                         (sourceImageRect_.width - 1))),
        sourceImageRect_.y + static_cast<int>(std::lround(point.y / denominatorY *
                                                         (sourceImageRect_.height - 1)))};
}

cv::Point2f Application::mapDisplayPointToSource(const int x, const int y) const {
    const cv::Size inputSize = corrector_->inputSize();
    const float normalizedX = static_cast<float>(x - sourceImageRect_.x) /
                              static_cast<float>(std::max(1, sourceImageRect_.width - 1));
    const float normalizedY = static_cast<float>(y - sourceImageRect_.y) /
                              static_cast<float>(std::max(1, sourceImageRect_.height - 1));
    return {
        std::clamp(normalizedX, 0.0F, 1.0F) * static_cast<float>(inputSize.width - 1),
        std::clamp(normalizedY, 0.0F, 1.0F) * static_cast<float>(inputSize.height - 1)};
}

} // namespace wpc
