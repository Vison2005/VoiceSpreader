#include "sampling_rate_offset_estimator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
double median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    if ((values.size() % 2) != 0) {
        return *middle;
    }
    const auto lower = std::max_element(values.begin(), middle);
    return (*lower + *middle) * 0.5;
}
}

SamplingRateOffsetEstimator::SamplingRateOffsetEstimator(
    std::size_t maximumSamples,
    std::size_t minimumSamples,
    double maximumAbsoluteDriftPpm)
    : maximumSamples_(std::max<std::size_t>(2, maximumSamples))
    , minimumSamples_(std::clamp<std::size_t>(minimumSamples, 2, maximumSamples_))
    , maximumAbsoluteDriftPpm_(std::max(1.0, maximumAbsoluteDriftPpm))
{
}

void SamplingRateOffsetEstimator::reset()
{
    samples_.clear();
    originTimeSeconds_ = 0.0;
    interceptSeconds_ = 0.0;
    slopeSecondsPerSecond_ = 0.0;
    residualRmsSeconds_ = 0.0;
    fitted_ = false;
}

bool SamplingRateOffsetEstimator::addSample(double timeSeconds,
                                            double delaySeconds,
                                            double confidence)
{
    if (!std::isfinite(timeSeconds) || !std::isfinite(delaySeconds)
        || !std::isfinite(confidence) || confidence <= 0.0
        || (!samples_.empty() && timeSeconds <= samples_.back().timeSeconds)) {
        return false;
    }

    samples_.push_back({timeSeconds, delaySeconds, std::clamp(confidence, 0.01, 1.0)});
    while (samples_.size() > maximumSamples_) {
        samples_.pop_front();
    }
    refit();
    return true;
}

bool SamplingRateOffsetEstimator::ready() const
{
    return fitted_ && samples_.size() >= minimumSamples_
           && samples_.back().timeSeconds - samples_.front().timeSeconds >= 1.0;
}

std::size_t SamplingRateOffsetEstimator::sampleCount() const
{
    return samples_.size();
}

double SamplingRateOffsetEstimator::driftPpm() const
{
    return ready() ? slopeSecondsPerSecond_ * 1'000'000.0 : 0.0;
}

double SamplingRateOffsetEstimator::residualRmsSeconds() const
{
    return residualRmsSeconds_;
}

double SamplingRateOffsetEstimator::delayAt(double timeSeconds) const
{
    if (!fitted_ || !std::isfinite(timeSeconds)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return interceptSeconds_
           + slopeSecondsPerSecond_ * (timeSeconds - originTimeSeconds_);
}

void SamplingRateOffsetEstimator::refit()
{
    fitted_ = false;
    if (samples_.size() < 2) {
        return;
    }

    originTimeSeconds_ = samples_.front().timeSeconds;
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> confidenceWeights;
    x.reserve(samples_.size());
    y.reserve(samples_.size());
    confidenceWeights.reserve(samples_.size());
    for (const Sample& sample : samples_) {
        x.push_back(sample.timeSeconds - originTimeSeconds_);
        y.push_back(sample.delaySeconds);
        confidenceWeights.push_back(sample.confidence * sample.confidence);
    }

    // Theil-Sen 初值避免一次混响误峰把普通最小二乘斜率拉偏。
    std::vector<double> pairwiseSlopes;
    pairwiseSlopes.reserve(x.size() * (x.size() - 1) / 2);
    for (std::size_t first = 0; first < x.size(); ++first) {
        for (std::size_t second = first + 1; second < x.size(); ++second) {
            const double deltaTime = x[second] - x[first];
            if (deltaTime > 0.0) {
                pairwiseSlopes.push_back((y[second] - y[first]) / deltaTime);
            }
        }
    }
    double slope = median(pairwiseSlopes);
    std::vector<double> intercepts;
    intercepts.reserve(x.size());
    for (std::size_t index = 0; index < x.size(); ++index) {
        intercepts.push_back(y[index] - slope * x[index]);
    }
    double intercept = median(intercepts);

    std::vector<double> absoluteResiduals;
    absoluteResiduals.reserve(x.size());
    for (std::size_t index = 0; index < x.size(); ++index) {
        absoluteResiduals.push_back(std::abs(y[index] - (intercept + slope * x[index])));
    }
    const double robustScale = std::max(20.0e-6,
                                        1.4826 * median(absoluteResiduals));
    const double huberLimit = 2.5 * robustScale;
    std::vector<double> weights = confidenceWeights;
    for (std::size_t index = 0; index < weights.size(); ++index) {
        if (absoluteResiduals[index] > huberLimit) {
            weights[index] *= huberLimit / absoluteResiduals[index];
        }
    }

    double sumWeight = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    for (std::size_t index = 0; index < x.size(); ++index) {
        sumWeight += weights[index];
        sumX += weights[index] * x[index];
        sumY += weights[index] * y[index];
    }
    if (sumWeight <= 0.0) {
        return;
    }
    const double meanX = sumX / sumWeight;
    const double meanY = sumY / sumWeight;
    double covariance = 0.0;
    double variance = 0.0;
    for (std::size_t index = 0; index < x.size(); ++index) {
        covariance += weights[index] * (x[index] - meanX) * (y[index] - meanY);
        variance += weights[index] * (x[index] - meanX) * (x[index] - meanX);
    }
    if (variance <= std::numeric_limits<double>::epsilon()) {
        return;
    }

    slope = covariance / variance;
    const double maximumSlope = maximumAbsoluteDriftPpm_ * 1.0e-6;
    slope = std::clamp(slope, -maximumSlope, maximumSlope);
    intercept = meanY - slope * meanX;

    double weightedSquaredResiduals = 0.0;
    for (std::size_t index = 0; index < x.size(); ++index) {
        const double residual = y[index] - (intercept + slope * x[index]);
        weightedSquaredResiduals += weights[index] * residual * residual;
    }
    interceptSeconds_ = intercept;
    slopeSecondsPerSecond_ = slope;
    residualRmsSeconds_ = std::sqrt(weightedSquaredResiduals / sumWeight);
    fitted_ = std::isfinite(interceptSeconds_)
              && std::isfinite(slopeSecondsPerSecond_)
              && std::isfinite(residualRmsSeconds_);
}
