#include "audio_clock_model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>
#include <vector>

namespace
{
constexpr std::size_t kMinimumFitSamples = 3;

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

AudioClockModel::AudioClockModel(double qpcTicksPerSecond, std::size_t maximumSamples)
    : qpcTicksPerSecond_(qpcTicksPerSecond > 0.0 ? qpcTicksPerSecond : 10'000'000.0)
    , maximumSamples_(std::max(kMinimumFitSamples, maximumSamples))
{
}

void AudioClockModel::setQpcTicksPerSecond(double qpcTicksPerSecond)
{
    if (qpcTicksPerSecond > 0.0 && std::isfinite(qpcTicksPerSecond)) {
        qpcTicksPerSecond_ = qpcTicksPerSecond;
        refit();
    }
}

void AudioClockModel::reset()
{
    samples_.clear();
    originQpcTicks_ = 0.0;
    originDeviceFrame_ = 0.0;
    framesPerQpcTick_ = 0.0;
    interceptFrames_ = 0.0;
    residualRmsFrames_ = 0.0;
}

bool AudioClockModel::addSample(std::uint64_t deviceFrame, std::uint64_t qpcTicks)
{
    if (!samples_.empty()
        && (deviceFrame <= samples_.back().deviceFrame || qpcTicks <= samples_.back().qpcTicks)) {
        return false;
    }

    samples_.push_back({deviceFrame, qpcTicks});
    while (samples_.size() > maximumSamples_) {
        samples_.pop_front();
    }
    refit();
    return true;
}

bool AudioClockModel::ready() const
{
    return samples_.size() >= kMinimumFitSamples && framesPerQpcTick_ > 0.0;
}

std::size_t AudioClockModel::sampleCount() const
{
    return samples_.size();
}

double AudioClockModel::framesPerSecond() const
{
    return framesPerQpcTick_ * qpcTicksPerSecond_;
}

double AudioClockModel::driftPpm(double nominalFrameRate) const
{
    if (!ready() || nominalFrameRate <= 0.0 || !std::isfinite(nominalFrameRate)) {
        return 0.0;
    }
    return (framesPerSecond() / nominalFrameRate - 1.0) * 1'000'000.0;
}

double AudioClockModel::residualRmsFrames() const
{
    return residualRmsFrames_;
}

double AudioClockModel::frameAtQpc(double qpcTicks) const
{
    return originDeviceFrame_ + interceptFrames_
           + framesPerQpcTick_ * (qpcTicks - originQpcTicks_);
}

double AudioClockModel::qpcAtFrame(double deviceFrame) const
{
    if (!ready()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return originQpcTicks_
           + (deviceFrame - originDeviceFrame_ - interceptFrames_) / framesPerQpcTick_;
}

void AudioClockModel::refit()
{
    if (samples_.size() < 2) {
        framesPerQpcTick_ = 0.0;
        interceptFrames_ = 0.0;
        residualRmsFrames_ = 0.0;
        return;
    }

    originQpcTicks_ = static_cast<double>(samples_.front().qpcTicks);
    originDeviceFrame_ = static_cast<double>(samples_.front().deviceFrame);

    std::vector<double> x;
    std::vector<double> y;
    x.reserve(samples_.size());
    y.reserve(samples_.size());
    for (const Sample& sample : samples_) {
        x.push_back(static_cast<double>(sample.qpcTicks) - originQpcTicks_);
        y.push_back(static_cast<double>(sample.deviceFrame) - originDeviceFrame_);
    }

    auto fit = [&](const std::vector<double>& weights) {
        double sumW = 0.0;
        double sumX = 0.0;
        double sumY = 0.0;
        for (std::size_t index = 0; index < x.size(); ++index) {
            sumW += weights[index];
            sumX += weights[index] * x[index];
            sumY += weights[index] * y[index];
        }
        if (sumW <= 0.0) {
            return std::pair<double, double>{0.0, 0.0};
        }
        const double meanX = sumX / sumW;
        const double meanY = sumY / sumW;
        double covariance = 0.0;
        double variance = 0.0;
        for (std::size_t index = 0; index < x.size(); ++index) {
            covariance += weights[index] * (x[index] - meanX) * (y[index] - meanY);
            variance += weights[index] * (x[index] - meanX) * (x[index] - meanX);
        }
        if (variance <= 0.0) {
            return std::pair<double, double>{0.0, meanY};
        }
        const double slope = covariance / variance;
        return std::pair<double, double>{slope, meanY - slope * meanX};
    };

    std::vector<double> weights(x.size(), 1.0);
    double slope = 0.0;
    double intercept = 0.0;
    if (x.size() >= kMinimumFitSamples) {
        // Theil-Sen 初始估计对少量异常帧位置更稳健，再用 Huber 权重细化。
        std::vector<double> pairwiseSlopes;
        pairwiseSlopes.reserve(x.size() * (x.size() - 1) / 2);
        for (std::size_t first = 0; first < x.size(); ++first) {
            for (std::size_t second = first + 1; second < x.size(); ++second) {
                if (x[second] > x[first]) {
                    pairwiseSlopes.push_back((y[second] - y[first])
                                             / (x[second] - x[first]));
                }
            }
        }
        slope = median(pairwiseSlopes);
        std::vector<double> intercepts;
        intercepts.reserve(x.size());
        for (std::size_t index = 0; index < x.size(); ++index) {
            intercepts.push_back(y[index] - slope * x[index]);
        }
        intercept = median(intercepts);
    } else {
        std::tie(slope, intercept) = fit(weights);
    }
    if (x.size() >= kMinimumFitSamples && slope > 0.0) {
        std::vector<double> residuals;
        residuals.reserve(x.size());
        for (std::size_t index = 0; index < x.size(); ++index) {
            residuals.push_back(std::abs(y[index] - (slope * x[index] + intercept)));
        }
        const double scale = std::max(1.0, 1.4826 * median(residuals));
        const double huberLimit = 3.0 * scale;
        for (std::size_t index = 0; index < x.size(); ++index) {
            const double absoluteResidual = residuals[index];
            weights[index] = absoluteResidual > huberLimit
                                 ? huberLimit / absoluteResidual
                                 : 1.0;
        }
        std::tie(slope, intercept) = fit(weights);
    }

    framesPerQpcTick_ = slope > 0.0 && std::isfinite(slope) ? slope : 0.0;
    interceptFrames_ = std::isfinite(intercept) ? intercept : 0.0;

    double squaredResiduals = 0.0;
    for (std::size_t index = 0; index < x.size(); ++index) {
        const double residual = y[index] - (framesPerQpcTick_ * x[index] + interceptFrames_);
        squaredResiduals += residual * residual;
    }
    residualRmsFrames_ = std::sqrt(squaredResiduals / static_cast<double>(x.size()));
}
