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

bool verifyMixedSpreadSpectrumProbes()
{
    constexpr std::uint32_t sampleRate = 48000;
    constexpr double expectedSeconds = 0.20;
    constexpr double firstDelaySeconds = 0.083;
    constexpr double secondDelaySeconds = 0.217;
    const std::vector<float> firstProbe = generateSpreadSpectrumProbe(
        sampleRate, 0xA71C0000u ^ 0x9E3779B9u);
    const std::vector<float> secondProbe = generateSpreadSpectrumProbe(
        sampleRate, 0xA71C0000u ^ (2u * 0x9E3779B9u));
    std::vector<float> recording(sampleRate, 0.0F);

    const auto addProbe = [&](const std::vector<float>& probe, double delay) {
        const std::size_t insertionFrame = static_cast<std::size_t>(
            std::llround((expectedSeconds + delay) * sampleRate));
        for (std::size_t index = 0; index < probe.size(); ++index) {
            recording[insertionFrame + index] += 0.20F * probe[index];
        }
    };
    addProbe(firstProbe, firstDelaySeconds);
    addProbe(secondProbe, secondDelaySeconds);

    const ProbeDetection firstDetection = detectSpreadSpectrumProbe(
        recording, sampleRate, firstProbe, expectedSeconds);
    const ProbeDetection secondDetection = detectSpreadSpectrumProbe(
        recording, sampleRate, secondProbe, expectedSeconds);
    if (!firstDetection.detected
        || !secondDetection.detected
        || std::abs(firstDetection.relativeDelaySeconds - firstDelaySeconds) > 0.001
        || std::abs(secondDetection.relativeDelaySeconds - secondDelaySeconds) > 0.001) {
        std::cerr << "Mixed spread-spectrum probe failed: first="
                  << firstDetection.relativeDelaySeconds << ", second="
                  << secondDetection.relativeDelaySeconds << '\n';
        return false;
    }
    return true;
}

bool verifyExponentialSweepImpulseResponse(std::uint32_t sampleRate)
{
    constexpr double expectedStartSeconds = 0.20;
    constexpr double directDelaySeconds = 0.0834;
    constexpr double reflectionDelaySeconds = 0.137;
    const std::vector<float> sweep = generateExponentialSineSweep(
        sampleRate,
        0.55,
        120.0,
        std::min(18000.0, sampleRate * 0.42));
    std::vector<float> recording(static_cast<std::size_t>(sampleRate * 1.4), 0.0F);
    const auto addResponse = [&](double delaySeconds, float gain) {
        const std::size_t start = static_cast<std::size_t>(std::llround(
            (expectedStartSeconds + delaySeconds) * sampleRate));
        for (std::size_t index = 0;
             index < sweep.size() && start + index < recording.size();
             ++index) {
            recording[start + index] += gain * sweep[index];
        }
    };
    // 反射峰比直接声更强，检测器仍应选择最早可信脉冲。
    addResponse(directDelaySeconds, -0.35F);
    addResponse(reflectionDelaySeconds, 1.0F);

    std::uint32_t noiseState = 0x1234ABCDu;
    for (float& sample : recording) {
        noiseState = noiseState * 1664525u + 1013904223u;
        const float noise = static_cast<float>((noiseState >> 8) & 0xFFFFu)
                                / 32768.0F
                            - 1.0F;
        sample += noise * 0.0002F;
    }

    const ProbeDetection detection = detectExponentialSweepImpulseResponse(
        recording,
        sampleRate,
        sweep,
        expectedStartSeconds,
        0.05,
        0.30);
    if (!detection.detected
        || detection.status != ProbeDetectionStatus::Detected
        || std::abs(detection.relativeDelaySeconds - directDelaySeconds) > 0.001) {
        std::cerr << "ESS impulse response failed at " << sampleRate
                  << " Hz, delay=" << detection.relativeDelaySeconds
                  << ", confidence=" << detection.confidence << '\n';
        return false;
    }

    const ProbeDetection silentDetection = detectExponentialSweepImpulseResponse(
        std::vector<float>(recording.size(), 0.0F),
        sampleRate,
        sweep,
        expectedStartSeconds,
        0.05,
        0.30);
    if (silentDetection.detected
        || silentDetection.status != ProbeDetectionStatus::NoUsableEnergy) {
        std::cerr << "Silent ESS recording was not rejected\n";
        return false;
    }
    return true;
}

bool verifyConsistentImpulseResponsePeakSelection()
{
    SweepImpulseResponseAnalysis first;
    first.status = ProbeDetectionStatus::Detected;
    first.peaks = {
        {0.0550, 0.4, 17.0, 0.49},
        {0.0920, 0.9, 30.0, 1.00},
    };
    SweepImpulseResponseAnalysis second;
    second.status = ProbeDetectionStatus::Detected;
    second.peaks = {
        {0.0690, 0.4, 17.0, 0.29},
        {0.0928, 0.8, 28.0, 0.80},
    };
    const ProbeDetection consistent = selectConsistentImpulseResponsePeakPair(
        first, second, 0.002);
    if (!consistent.detected
        || std::abs(consistent.relativeDelaySeconds - 0.0924) > 0.0001
        || std::abs(consistent.consistencySpreadSeconds - 0.0008) > 0.0001) {
        std::cerr << "Consistent ESS peak pair was not selected\n";
        return false;
    }

    SweepImpulseResponseAnalysis unrelated = second;
    unrelated.peaks = {
        {0.0147, 0.7, 21.0, 0.27},
        {0.9066, 0.8, 21.0, 1.00},
    };
    if (selectConsistentImpulseResponsePeakPair(first, unrelated, 0.002).detected) {
        std::cerr << "Unrelated ESS peaks were accepted\n";
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
    if (!verifyMixedSpreadSpectrumProbes()) {
        return 1;
    }
    if (!verifyExponentialSweepImpulseResponse(48000)
        || !verifyExponentialSweepImpulseResponse(44100)) {
        return 1;
    }
    if (!verifyConsistentImpulseResponsePeakSelection()) {
        return 1;
    }

    std::cout << "Calibration signal test passed\n";
    return 0;
}
