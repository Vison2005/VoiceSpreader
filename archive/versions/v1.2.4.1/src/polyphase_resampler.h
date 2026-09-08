#pragma once

#include <cstddef>
#include <vector>

// 轻量级窗口化 sinc 多相重采样器，替代实时路径中的线性插值。
class PolyphaseResampler
{
public:
    static constexpr std::size_t kHalfTaps = 8;
    static constexpr std::size_t kTapCount = kHalfTaps * 2;
    static constexpr std::size_t kPhaseCount = 256;

    explicit PolyphaseResampler(std::size_t channels);

    float sample(const std::vector<float>& interleaved,
                 std::size_t frameCount,
                 double position,
                 std::size_t channel) const;

private:
    std::size_t channels_ = 0;
};
