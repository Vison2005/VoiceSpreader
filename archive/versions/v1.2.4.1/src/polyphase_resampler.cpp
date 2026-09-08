#include "polyphase_resampler.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
using CoefficientTable = std::array<std::array<float, PolyphaseResampler::kTapCount>,
                                    PolyphaseResampler::kPhaseCount>;

CoefficientTable buildCoefficientTable()
{
    constexpr double pi = 3.14159265358979323846;
    CoefficientTable table{};
    for (std::size_t phase = 0; phase < PolyphaseResampler::kPhaseCount; ++phase) {
        const double fraction = static_cast<double>(phase)
                                / static_cast<double>(PolyphaseResampler::kPhaseCount);
        double sum = 0.0;
        for (std::size_t tap = 0; tap < PolyphaseResampler::kTapCount; ++tap) {
            const double offset = static_cast<double>(tap)
                                  - static_cast<double>(PolyphaseResampler::kHalfTaps - 1);
            const double x = offset - fraction;
            const double sincArgument = 0.90 * x;
            const double sinc = std::abs(sincArgument) < 1.0e-12
                                    ? 1.0
                                    : std::sin(pi * sincArgument) / (pi * sincArgument);
            const double window = 0.5
                                  * (1.0
                                     - std::cos(2.0 * pi
                                                * (static_cast<double>(tap) + 0.5)
                                                / static_cast<double>(PolyphaseResampler::kTapCount)));
            table[phase][tap] = static_cast<float>(sinc * window);
            sum += sinc * window;
        }
        if (std::abs(sum) > 1.0e-12) {
            for (float& coefficient : table[phase]) {
                coefficient = static_cast<float>(coefficient / sum);
            }
        }
    }
    return table;
}

const CoefficientTable& coefficientTable()
{
    static const CoefficientTable table = buildCoefficientTable();
    return table;
}
}

PolyphaseResampler::PolyphaseResampler(std::size_t channels)
    : channels_(channels)
{
}

float PolyphaseResampler::sample(const std::vector<float>& interleaved,
                                 std::size_t frameCount,
                                 double position,
                                 std::size_t channel) const
{
    if (channels_ == 0 || frameCount == 0 || channel >= channels_ || !std::isfinite(position)) {
        return 0.0F;
    }

    const auto leftFrame = static_cast<std::int64_t>(std::floor(position));
    const double fraction = position - static_cast<double>(leftFrame);
    const auto phase = static_cast<std::size_t>(std::clamp(
        static_cast<long long>(std::llround(fraction * static_cast<double>(kPhaseCount))),
        0LL,
        static_cast<long long>(kPhaseCount - 1)));

    float value = 0.0F;
    const auto& coefficients = coefficientTable()[phase];
    for (std::size_t tap = 0; tap < kTapCount; ++tap) {
        const auto unclampedFrame = leftFrame
                                     + static_cast<std::int64_t>(tap)
                                     - static_cast<std::int64_t>(kHalfTaps - 1);
        const auto frame = static_cast<std::size_t>(std::clamp<std::int64_t>(
            unclampedFrame, 0, static_cast<std::int64_t>(frameCount - 1)));
        value += interleaved[frame * channels_ + channel] * coefficients[tap];
    }
    return value;
}
