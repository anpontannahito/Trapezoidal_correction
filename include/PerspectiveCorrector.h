#pragma once

#include <opencv2/core.hpp>

#include <atomic>
#include <array>
#include <cstddef>
#include <mutex>

namespace wpc {

class PerspectiveCorrector {
public:
    using PointArray = std::array<cv::Point2f, 4>;

    PerspectiveCorrector(
        cv::Size inputSize,
        cv::Size outputSize,
        const PointArray& normalizedPoints);

    void updateInputSize(cv::Size inputSize);
    void setOutputSize(cv::Size outputSize);
    void resetPoints();
    void setPoint(std::size_t index, cv::Point2f point);

    [[nodiscard]] PointArray points() const;
    [[nodiscard]] PointArray normalizedPoints() const;
    [[nodiscard]] cv::Size inputSize() const;
    [[nodiscard]] cv::Size outputSize() const;
    [[nodiscard]] bool isGeometryValid() const;

    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isEnabled() const noexcept;

    // 変換行列は点または出力サイズが変わった場合だけ再計算される。
    bool apply(const cv::Mat& source, cv::Mat& destination) const;

private:
    static PointArray defaultNormalizedPoints();
    static bool isConvexNonDegenerate(const PointArray& points);
    void setNormalizedPointsLocked(const PointArray& normalizedPoints);
    void recomputeTransformLocked();

    mutable std::mutex mutex_;
    cv::Size inputSize_;
    cv::Size outputSize_;
    PointArray points_{};
    cv::Mat cachedTransform_;
    bool hasValidTransform_ = false;
    bool geometryValid_ = false;
    std::atomic<bool> enabled_{true};
};

} // namespace wpc
