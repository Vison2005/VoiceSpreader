#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

// 用设备帧位置和 QPC 位置拟合设备硬件时钟。
// 该类不依赖平台 API，便于在桌面端和单元测试中复用。
class AudioClockModel
{
public:
    struct Sample
    {
        std::uint64_t deviceFrame = 0;
        std::uint64_t qpcTicks = 0;
    };

    explicit AudioClockModel(double qpcTicksPerSecond = 10'000'000.0,
                             std::size_t maximumSamples = 32);

    void setQpcTicksPerSecond(double qpcTicksPerSecond);
    void reset();

    // 拒绝非单调样本；返回 false 表示样本没有被接收。
    bool addSample(std::uint64_t deviceFrame, std::uint64_t qpcTicks);

    bool ready() const;
    std::size_t sampleCount() const;
    double framesPerSecond() const;
    double driftPpm(double nominalFrameRate) const;
    double residualRmsFrames() const;
    double frameAtQpc(double qpcTicks) const;
    double qpcAtFrame(double deviceFrame) const;

private:
    void refit();

    double qpcTicksPerSecond_ = 10'000'000.0;
    std::size_t maximumSamples_ = 32;
    std::deque<Sample> samples_;
    double originQpcTicks_ = 0.0;
    double originDeviceFrame_ = 0.0;
    double framesPerQpcTick_ = 0.0;
    double interceptFrames_ = 0.0;
    double residualRmsFrames_ = 0.0;
};
