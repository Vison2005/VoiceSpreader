#include "../src/acoustic_alignment.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
bool closeTo(double actual, double expected)
{
    return std::abs(actual - expected) < 1.0e-9;
}

bool verifyTargets(const std::vector<double>& underlying,
                   const std::vector<double>& expected,
                   double maximumDelay = 500.0)
{
    const std::vector<double> actual = calculateMinimalAcousticDelays(
        underlying,
        maximumDelay);
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (!closeTo(actual[index], expected[index])) {
            std::cerr << "Unexpected target at " << index << ": " << actual[index]
                      << ", expected " << expected[index] << '\n';
            return false;
        }
    }
    return true;
}
}

int main()
{
    // 固有延迟较大的设备必须保持 0 ms，不能再被向后推。
    if (!verifyTargets({180.0, 70.0}, {0.0, 110.0})
        || !verifyTargets({100.0, 160.0, 130.0}, {60.0, 0.0, 30.0})) {
        return 1;
    }

    // 上一轮多加的公共延迟必须能够退回，而不是成为下一轮的新基准。
    const std::vector<double> measured{215.0, 215.0};
    const std::vector<double> currentDelays{35.0, 145.0};
    std::vector<double> recoveredUnderlying;
    for (std::size_t index = 0; index < measured.size(); ++index) {
        recoveredUnderlying.push_back(measured[index] - currentDelays[index]);
    }
    if (!verifyTargets(recoveredUnderlying, {0.0, 110.0})
        || !verifyTargets({900.0, 100.0}, {0.0, 500.0}, 500.0)) {
        return 1;
    }

    // 手机和电脑时钟原点不同，但设备间的跨时钟坐标差仍等于真实延迟差。
    constexpr double captureRate = 48000.0;
    const double slowCoordinate = calculateCrossClockArrivalCoordinateMilliseconds(
        static_cast<std::uint64_t>(20.180 * captureRate),
        captureRate,
        100LL * 10'000'000LL);
    const double fastCoordinate = calculateCrossClockArrivalCoordinateMilliseconds(
        static_cast<std::uint64_t>(22.070 * captureRate),
        captureRate,
        102LL * 10'000'000LL);
    if (!closeTo(slowCoordinate - fastCoordinate, 110.0)
        || !verifyTargets({slowCoordinate, fastCoordinate}, {0.0, 110.0})) {
        std::cerr << "Cross-clock coordinate reversed the delay direction\n";
        return 1;
    }

    if (!calculateMinimalAcousticDelays({}).empty()) {
        return 1;
    }
    try {
        calculateMinimalAcousticDelays(
            {10.0, std::numeric_limits<double>::quiet_NaN()});
        return 1;
    } catch (const std::invalid_argument&) {
    }

    return 0;
}
