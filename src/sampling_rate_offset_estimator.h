#pragma once

#include <cstddef>
#include <deque>

// 从一系列“观测时间—相对延迟”样本拟合采样率偏差。
// 若 delay(t) = delay(0) + skew * t，则 driftPpm = skew * 1e6。
class SamplingRateOffsetEstimator
{
public:
    struct Sample
    {
        double timeSeconds = 0.0;
        double delaySeconds = 0.0;
        double confidence = 1.0;
    };

    explicit SamplingRateOffsetEstimator(std::size_t maximumSamples = 32,
                                         std::size_t minimumSamples = 5,
                                         double maximumAbsoluteDriftPpm = 2'000.0);

    void reset();

    // 时间必须严格递增；置信度位于 (0, 1]。返回 false 表示样本被拒绝。
    bool addSample(double timeSeconds,
                   double delaySeconds,
                   double confidence = 1.0);

    bool ready() const;
    std::size_t sampleCount() const;
    double driftPpm() const;
    double residualRmsSeconds() const;
    double delayAt(double timeSeconds) const;

private:
    void refit();

    std::size_t maximumSamples_ = 32;
    std::size_t minimumSamples_ = 5;
    double maximumAbsoluteDriftPpm_ = 2'000.0;
    std::deque<Sample> samples_;
    double originTimeSeconds_ = 0.0;
    double interceptSeconds_ = 0.0;
    double slopeSecondsPerSecond_ = 0.0;
    double residualRmsSeconds_ = 0.0;
    bool fitted_ = false;
};
