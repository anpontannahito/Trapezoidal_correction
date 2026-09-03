#include "PerspectiveCorrector.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace wpc {

PerspectiveCorrector::PerspectiveCorrector(
    const cv::Size inputSize,
    const cv::Size outputSize,
    const PointArray& normalizedPoints)
    : inputSize_(std::max(1, inputSize.width), std::max(1, inputSize.height)),
      outputSize_(std::max(1, outputSize.width), std::max(1, outputSize.height)) {
    std::lock_guard<std::mutex> lock(mutex_);
    setNormalizedPointsLocked(normalizedPoints);
    recomputeTransformLocked();
}

void PerspectiveCorrector::updateInputSize(const cv::Size inputSize) {
    if (inputSize.width <= 0 || inputSize.height <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (inputSize == inputSize_) {
        return;
    }

    const float scaleX = static_cast<float>(inputSize.width) /
                         static_cast<float>(std::max(1, inputSize_.width));
    const float scaleY = static_cast<float>(inputSize.height) /
                         static_cast<float>(std::max(1, inputSize_.height));
    for (auto& point : points_) {
        point.x *= scaleX;
        point.y *= scaleY;
    }
    inputSize_ = inputSize;
    recomputeTransformLocked();
}

void PerspectiveCorrector::setOutputSize(const cv::Size outputSize) {
    if (outputSize.width <= 0 || outputSize.height <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (outputSize == outputSize_) {
        return;
    }
    outputSize_ = outputSize;
    recomputeTransformLocked();
}

void PerspectiveCorrector::resetPoints() {
    std::lock_guard<std::mutex> lock(mutex_);
    setNormalizedPointsLocked(defaultNormalizedPoints());
    recomputeTransformLocked();
}

void PerspectiveCorrector::setPoint(const std::size_t index, cv::Point2f point) {
    if (index >= points_.size()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    point.x = std::clamp(point.x, 0.0F, static_cast<float>(inputSize_.width - 1));
    point.y = std::clamp(point.y, 0.0F, static_cast<float>(inputSize_.height - 1));
    if (cv::norm(points_[index] - point) < 0.01) {
        return;
    }
    points_[index] = point;
    recomputeTransformLocked();
}

PerspectiveCorrector::PointArray PerspectiveCorrector::points() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return points_;
}

PerspectiveCorrector::PointArray PerspectiveCorrector::normalizedPoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PointArray normalized{};
    const float denominatorX = static_cast<float>(std::max(1, inputSize_.width - 1));
    const float denominatorY = static_cast<float>(std::max(1, inputSize_.height - 1));
    for (std::size_t i = 0; i < points_.size(); ++i) {
        normalized[i] = {points_[i].x / denominatorX, points_[i].y / denominatorY};
    }
    return normalized;
}

cv::Size PerspectiveCorrector::inputSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inputSize_;
}

cv::Size PerspectiveCorrector::outputSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outputSize_;
}

bool PerspectiveCorrector::isGeometryValid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return geometryValid_;
}

void PerspectiveCorrector::setEnabled(const bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_release);
}

bool PerspectiveCorrector::isEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

bool PerspectiveCorrector::apply(const cv::Mat& source, cv::Mat& destination) const {
    if (source.empty()) {
        return false;
    }

    cv::Mat transform;
    cv::Size outputSize;
    bool hasValidTransform = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transform = cachedTransform_; // 小さなMatヘッダーだけをコピーし、行列データは参照カウントで保持する。
        outputSize = outputSize_;
        hasValidTransform = hasValidTransform_;
    }

    try {
        if (isEnabled() && hasValidTransform) {
            cv::warpPerspective(
                source,
                destination,
                transform,
                outputSize,
                cv::INTER_LINEAR,
                cv::BORDER_CONSTANT,
                cv::Scalar::all(0));
        } else if (source.size() == outputSize) {
            source.copyTo(destination);
        } else {
            cv::resize(source, destination, outputSize, 0.0, 0.0, cv::INTER_LINEAR);
        }
    } catch (const cv::Exception&) {
        // メモリ不足が原因の場合、同じ大きさの黒画像確保を再試行しない。
        destination.release();
        return false;
    }
    return true;
}

PerspectiveCorrector::PointArray PerspectiveCorrector::defaultNormalizedPoints() {
    return {{{0.10F, 0.10F}, {0.90F, 0.10F}, {0.90F, 0.90F}, {0.10F, 0.90F}}};
}

bool PerspectiveCorrector::isConvexNonDegenerate(const PointArray& points) {
    float expectedSign = 0.0F;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const cv::Point2f edgeA = points[(i + 1) % points.size()] - points[i];
        const cv::Point2f edgeB = points[(i + 2) % points.size()] - points[(i + 1) % points.size()];
        const float cross = edgeA.x * edgeB.y - edgeA.y * edgeB.x;
        if (!std::isfinite(cross) || std::abs(cross) < 1.0F) {
            return false;
        }
        if (expectedSign == 0.0F) {
            expectedSign = cross;
        } else if ((cross > 0.0F) != (expectedSign > 0.0F)) {
            return false;
        }
    }
    return true;
}

void PerspectiveCorrector::setNormalizedPointsLocked(const PointArray& normalizedPoints) {
    const float maxX = static_cast<float>(std::max(0, inputSize_.width - 1));
    const float maxY = static_cast<float>(std::max(0, inputSize_.height - 1));
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const float normalizedX = std::isfinite(normalizedPoints[i].x)
                                      ? std::clamp(normalizedPoints[i].x, 0.0F, 1.0F)
                                      : defaultNormalizedPoints()[i].x;
        const float normalizedY = std::isfinite(normalizedPoints[i].y)
                                      ? std::clamp(normalizedPoints[i].y, 0.0F, 1.0F)
                                      : defaultNormalizedPoints()[i].y;
        points_[i] = {normalizedX * maxX, normalizedY * maxY};
    }
}

void PerspectiveCorrector::recomputeTransformLocked() {
    geometryValid_ = isConvexNonDegenerate(points_);
    if (!geometryValid_) {
        return;
    }

    const PointArray destinationPoints{{
        {0.0F, 0.0F},
        {static_cast<float>(outputSize_.width - 1), 0.0F},
        {static_cast<float>(outputSize_.width - 1), static_cast<float>(outputSize_.height - 1)},
        {0.0F, static_cast<float>(outputSize_.height - 1)}}};

    cv::Mat candidate = cv::getPerspectiveTransform(points_.data(), destinationPoints.data());
    const double determinant = cv::determinant(candidate);
    if (!cv::checkRange(candidate) || !std::isfinite(determinant) || std::abs(determinant) < 1.0e-12) {
        geometryValid_ = false;
        return;
    }

    cachedTransform_ = std::move(candidate);
    hasValidTransform_ = true;
}

} // namespace wpc
