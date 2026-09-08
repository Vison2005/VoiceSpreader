#include "acoustic_alignment.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

std::vector<double> calculateMinimalAcousticDelays(
    const std::vector<double>& underlyingLatenciesMilliseconds,
    double maximumDelayMilliseconds)
{
    if (underlyingLatenciesMilliseconds.empty()) {
        return {};
    }
    if (!std::isfinite(maximumDelayMilliseconds) || maximumDelayMilliseconds < 0.0) {
        throw std::invalid_argument("最大声学补偿必须是非负有限数");
    }
    for (double latency : underlyingLatenciesMilliseconds) {
        if (!std::isfinite(latency)) {
            throw std::invalid_argument("固有声学延迟必须是有限数");
        }
    }

    const double slowestUnderlyingLatency = *std::max_element(
        underlyingLatenciesMilliseconds.cbegin(),
        underlyingLatenciesMilliseconds.cend());
    std::vector<double> delays;
    delays.reserve(underlyingLatenciesMilliseconds.size());
    for (double latency : underlyingLatenciesMilliseconds) {
        delays.push_back(std::clamp(slowestUnderlyingLatency - latency,
                                    0.0,
                                    maximumDelayMilliseconds));
    }
    return delays;
}

double calculateCrossClockArrivalCoordinateMilliseconds(
    std::uint64_t arrivalFrame,
    double captureFramesPerSecond,
    std::int64_t probeStartQpcHundredNanoseconds)
{
    if (!std::isfinite(captureFramesPerSecond) || captureFramesPerSecond <= 0.0) {
        throw std::invalid_argument("捕获采样率必须是正有限数");
    }
    const double captureTimeSeconds = static_cast<double>(arrivalFrame)
                                      / captureFramesPerSecond;
    const double probeStartSeconds = static_cast<double>(
                                         probeStartQpcHundredNanoseconds)
                                     / 10'000'000.0;
    return (captureTimeSeconds - probeStartSeconds) * 1000.0;
}
