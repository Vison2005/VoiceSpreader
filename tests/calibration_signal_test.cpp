#include "../src/calibration_signal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

namespace
{
bool verifyProbe(const std::vector<float>& probe,
                 std::uint32_t sampleRate,
                 double expectedSeconds,
                 double injectedDelaySeconds,
                 float amplitude,
                 double bandLowHz,
                 double bandHighHz,
                 const char* name)
{
    std::vector<float> recording(sampleRate * 2, 0.0F);
    for (std::size_t frame = 0; frame < recording.size(); ++frame) {
        recording[frame] = static_cast<float>(
            0.004 * std::sin(2.0 * 3.14159265358979323846
                             * 997.0 * frame / sampleRate));
    }
    const std::size_t insertionFrame = static_cast<std::size_t>(
        std::llround((expectedSeconds + injectedDelaySeconds) * sampleRate));
    for (std::size_t index = 0; index < probe.size(); ++index) {
        recording[insertionFrame + index] += amplitude * probe[index];
    }
    const ProbeDetection detection = detectKnownProbe(recording,
                                                       sampleRate,
                                                       probe,
                                                       expectedSeconds,
                                                       0.05,
                                                       0.45,
                                                       0.035,
                                                       bandLowHz,
                                                       bandHighHz);
    if (!detection.detected
        || std::abs(detection.relativeDelaySeconds - injectedDelaySeconds) > 0.001) {
        std::cerr << name << " probe failed, delay=" << detection.relativeDelaySeconds
                  << ", confidence=" << detection.confidence << '\n';
        return false;
    }
    return true;
}

bool verifyFractionalDelay()
{
    constexpr std::uint32_t sampleRate = 16000;
    constexpr double expectedSeconds = 0.15;
    constexpr double injectedDelaySeconds = 0.1234;
    constexpr double fractionalSample = 0.37;
    const std::vector<float> probe = generateCalibrationProbe(sampleRate);
    std::vector<float> recording(sampleRate, 0.0F);
    const std::size_t insertionFrame = static_cast<std::size_t>(
        std::llround((expectedSeconds + injectedDelaySeconds) * sampleRate));
    for (std::size_t index = 0; index < probe.size(); ++index) {
        recording[insertionFrame + index]
            += static_cast<float>((1.0 - fractionalSample) * probe[index]);
        recording[insertionFrame + index + 1]
            += static_cast<float>(fractionalSample * probe[index]);
    }
    const ProbeDetection detection = detectKnownProbe(recording,
                                                       sampleRate,
                                                       probe,
                                                       expectedSeconds,
                                                       0.05,
                                                       0.30,
                                                       0.05);
    const double expectedDelay = (static_cast<double>(insertionFrame)
                                  + fractionalSample)
                                 / sampleRate
                                 - expectedSeconds;
    if (!detection.detected
        || std::abs(detection.relativeDelaySeconds - expectedDelay) > 1.0 / sampleRate) {
        std::cerr << "Fractional probe failed, delay=" << detection.relativeDelaySeconds
                  << ", expected=" << expectedDelay
                  << ", confidence=" << detection.confidence << '\n';
        return false;
    }
    return true;
}
}

int main()
{
    constexpr std::uint32_t sampleRate = 48000;
    constexpr double expectedSeconds = 0.30;
    constexpr double injectedDelaySeconds = 0.175;

    std::vector<float> recording(sampleRate * 2, 0.0F);
    const std::vector<float> probe = generateCalibrationProbe(sampleRate);
    const std::size_t insertionFrame = static_cast<std::size_t>(
        std::llround((expectedSeconds + injectedDelaySeconds) * sampleRate));
    for (std::size_t index = 0; index < probe.size(); ++index) {
        recording[insertionFrame + index] = -probe[index];
    }

    const ProbeDetection detection = detectCalibrationProbe(recording,
                                                             sampleRate,
                                                             expectedSeconds);
    if (!detection.detected) {
        std::cerr << "Probe was not detected\n";
        return 1;
    }
    if (std::abs(detection.relativeDelaySeconds - injectedDelaySeconds) > 0.001) {
        std::cerr << "Unexpected delay: " << detection.relativeDelaySeconds << '\n';
        return 1;
    }
    if (detection.confidence < 0.95) {
        std::cerr << "Unexpected confidence: " << detection.confidence << '\n';
        return 1;
    }

    if (!verifyProbe(generateUltrasonicProbe(sampleRate),
                     sampleRate,
                     expectedSeconds,
                     injectedDelaySeconds,
                     0.003F,
                     19800.0,
                     22500.0,
                     "Ultrasonic")) {
        return 1;
    }
    if (!verifyProbe(generateSpreadSpectrumProbe(sampleRate, 0x123456u),
                     sampleRate,
                     expectedSeconds,
                     injectedDelaySeconds,
                     0.0012F,
                     13500.0,
                     18100.0,
                     "Spread-spectrum")) {
        return 1;
    }
    if (!verifyFractionalDelay()) {
        return 1;
    }

    std::cout << "Calibration signal test passed\n";
    return 0;
}
