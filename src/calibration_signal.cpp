#include "calibration_signal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace
{
constexpr double probeDurationSeconds = 0.12;
constexpr double startFrequencyHz = 600.0;
constexpr double endFrequencyHz = 5200.0;
constexpr double fadeDurationSeconds = 0.01;
constexpr double pi = 3.14159265358979323846;

double smoothFade(double seconds, double duration, double fadeSeconds)
{
    const double fadeIn = std::clamp(seconds / fadeSeconds, 0.0, 1.0);
    const double fadeOut = std::clamp((duration - seconds) / fadeSeconds, 0.0, 1.0);
    const double linear = std::min(fadeIn, fadeOut);
    return 0.5 - 0.5 * std::cos(pi * linear);
}

float probeSample(double seconds)
{
    if (seconds < 0.0 || seconds >= probeDurationSeconds) {
        return 0.0F;
    }

    const double sweepRate = (endFrequencyHz - startFrequencyHz) / probeDurationSeconds;
    const double phase = 2.0 * pi
                         * (startFrequencyHz * seconds
                            + 0.5 * sweepRate * seconds * seconds);
    const double fadeIn = std::min(1.0, seconds / fadeDurationSeconds);
    const double fadeOut = std::min(1.0,
                                    (probeDurationSeconds - seconds)
                                        / fadeDurationSeconds);
    return static_cast<float>(0.35 * std::sin(phase) * std::min(fadeIn, fadeOut));
}

std::vector<float> applyBandPass(const std::vector<float>& input,
                                 std::uint32_t sampleRate,
                                 double lowFrequency,
                                 double highFrequency)
{
    const double nyquist = static_cast<double>(sampleRate) * 0.5;
    const double low = std::clamp(lowFrequency, 20.0, nyquist - 40.0);
    const double high = std::clamp(highFrequency, low + 20.0, nyquist - 20.0);
    if (input.empty() || sampleRate == 0 || high <= low) {
        return input;
    }

    constexpr int tapCount = 129;
    constexpr int middle = tapCount / 2;
    std::vector<double> kernel(tapCount, 0.0);
    for (int tap = 0; tap < tapCount; ++tap) {
        const int offset = tap - middle;
        double value = 0.0;
        if (offset == 0) {
            value = 2.0 * (high - low) / sampleRate;
        } else {
            value = (std::sin(2.0 * pi * high * offset / sampleRate)
                     - std::sin(2.0 * pi * low * offset / sampleRate))
                    / (pi * offset);
        }
        const double window = 0.54
                              - 0.46 * std::cos(2.0 * pi * tap / (tapCount - 1));
        kernel[static_cast<std::size_t>(tap)] = value * window;
    }

    std::vector<float> output(input.size(), 0.0F);
    for (std::size_t index = 0; index < input.size(); ++index) {
        double value = 0.0;
        const int finalTap = std::min<int>(tapCount - 1, static_cast<int>(index));
        for (int tap = 0; tap <= finalTap; ++tap) {
            value += kernel[static_cast<std::size_t>(tap)]
                     * input[index - static_cast<std::size_t>(tap)];
        }
        output[index] = static_cast<float>(value);
    }
    return output;
}
}

std::vector<float> generateCalibrationProbe(std::uint32_t sampleRate)
{
    if (sampleRate == 0) {
        return {};
    }

    const std::size_t frameCount = static_cast<std::size_t>(
        std::llround(probeDurationSeconds * sampleRate));
    std::vector<float> result(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        result[frame] = probeSample(static_cast<double>(frame) / sampleRate);
    }
    return result;
}

std::vector<float> generateUltrasonicProbe(std::uint32_t sampleRate)
{
    constexpr double durationSeconds = 0.24;
    constexpr double fadeSeconds = 0.018;
    if (sampleRate < 44100) {
        return {};
    }

    const double nyquist = static_cast<double>(sampleRate) * 0.5;
    const double startFrequency = std::min(20400.0, nyquist - 1650.0);
    const double endFrequency = std::min(22000.0, nyquist - 350.0);
    if (startFrequency < 19500.0 || endFrequency - startFrequency < 1100.0) {
        return {};
    }

    const std::size_t frameCount = static_cast<std::size_t>(
        std::llround(durationSeconds * sampleRate));
    std::vector<float> result(frameCount, 0.0F);
    const double sweepRate = (endFrequency - startFrequency) / durationSeconds;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const double seconds = static_cast<double>(frame) / sampleRate;
        const double phase = 2.0 * pi
                             * (startFrequency * seconds
                                + 0.5 * sweepRate * seconds * seconds);
        result[frame] = static_cast<float>(
            std::sin(phase) * smoothFade(seconds, durationSeconds, fadeSeconds));
    }
    return result;
}

std::vector<float> generateSpreadSpectrumProbe(std::uint32_t sampleRate,
                                               std::uint32_t seed)
{
    constexpr double durationSeconds = 0.24;
    constexpr double chipRate = 750.0;
    constexpr double fadeSeconds = 0.015;
    if (sampleRate < 22050) {
        return {};
    }

    const double carrierFrequency = std::min(15800.0,
                                             static_cast<double>(sampleRate) * 0.34);
    const std::size_t frameCount = static_cast<std::size_t>(
        std::llround(durationSeconds * sampleRate));
    const std::size_t chipCount = static_cast<std::size_t>(
        std::ceil(durationSeconds * chipRate)) + 2;
    std::vector<float> chips(chipCount, 1.0F);
    std::uint32_t state = seed == 0 ? 0x51A7E3u : seed;
    for (std::size_t index = 0; index < chipCount; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        chips[index] = (state & 1u) == 0 ? -1.0F : 1.0F;
    }

    std::vector<float> result(frameCount, 0.0F);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const double seconds = static_cast<double>(frame) / sampleRate;
        const double chipPosition = seconds * chipRate;
        const std::size_t leftChip = std::min<std::size_t>(
            static_cast<std::size_t>(chipPosition), chipCount - 2);
        const double fraction = chipPosition - std::floor(chipPosition);
        // 在码片边缘做余弦过渡，避免低电平探针产生明显的宽带咔哒声。
        const double blend = 0.5 - 0.5 * std::cos(pi * fraction);
        const double chip = chips[leftChip]
                            + (chips[leftChip + 1] - chips[leftChip]) * blend;
        result[frame] = static_cast<float>(
            chip * std::sin(2.0 * pi * carrierFrequency * seconds)
            * smoothFade(seconds, durationSeconds, fadeSeconds));
    }
    return result;
}

ProbeDetection detectKnownProbe(const std::vector<float>& recording,
                                std::uint32_t sampleRate,
                                const std::vector<float>& referenceProbe,
                                double expectedStartSeconds,
                                double searchBeforeSeconds,
                                double searchAfterSeconds,
                                double detectionThreshold,
                                double bandLowHz,
                                double bandHighHz)
{
    ProbeDetection result;
    if (recording.empty() || referenceProbe.empty() || sampleRate == 0) {
        return result;
    }

    const bool useBandPass = bandLowHz > 0.0 && bandHighHz > bandLowHz;
    const std::vector<float> filteredRecording = useBandPass
                                                     ? applyBandPass(recording,
                                                                     sampleRate,
                                                                     bandLowHz,
                                                                     bandHighHz)
                                                     : recording;
    const std::vector<float> filteredReference = useBandPass
                                                     ? applyBandPass(referenceProbe,
                                                                     sampleRate,
                                                                     bandLowHz,
                                                                     bandHighHz)
                                                     : referenceProbe;

    // 先带通再降到约 16 kHz。高频信号会有控制地折叠到低频，发送端参考和录音仍保持同一编码。
    const std::uint32_t decimation = std::max<std::uint32_t>(1, sampleRate / 16000);
    const double decimatedRate = static_cast<double>(sampleRate) / decimation;
    std::vector<float> probe;
    probe.reserve((filteredReference.size() + decimation - 1) / decimation);
    for (std::size_t index = 0; index < filteredReference.size(); index += decimation) {
        probe.push_back(filteredReference[index]);
    }

    std::vector<float> samples;
    samples.reserve((filteredRecording.size() + decimation - 1) / decimation);
    for (std::size_t index = 0; index < filteredRecording.size(); index += decimation) {
        samples.push_back(filteredRecording[index]);
    }
    if (samples.size() <= probe.size()) {
        return result;
    }

    double probeEnergy = 0.0;
    for (float sample : probe) {
        probeEnergy += static_cast<double>(sample) * sample;
    }
    if (probeEnergy <= std::numeric_limits<double>::epsilon()) {
        return result;
    }

    std::vector<double> energyPrefix(samples.size() + 1, 0.0);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        energyPrefix[index + 1] = energyPrefix[index]
                                  + static_cast<double>(samples[index]) * samples[index];
    }

    const double searchStart = std::max(0.0, expectedStartSeconds - searchBeforeSeconds);
    const double searchEnd = expectedStartSeconds + searchAfterSeconds;
    const std::size_t firstOffset = std::min<std::size_t>(
        static_cast<std::size_t>(std::llround(searchStart * decimatedRate)),
        samples.size() - probe.size());
    const std::size_t lastOffset = std::min<std::size_t>(
        static_cast<std::size_t>(std::llround(searchEnd * decimatedRate)),
        samples.size() - probe.size());

    double bestScore = 0.0;
    std::size_t bestOffset = firstOffset;
    for (std::size_t offset = firstOffset; offset <= lastOffset; ++offset) {
        double dot = 0.0;
        for (std::size_t index = 0; index < probe.size(); ++index) {
            dot += static_cast<double>(probe[index]) * samples[offset + index];
        }
        const double windowEnergy = energyPrefix[offset + probe.size()]
                                    - energyPrefix[offset];
        if (windowEnergy <= std::numeric_limits<double>::epsilon()) {
            continue;
        }
        const double score = std::abs(dot) / std::sqrt(probeEnergy * windowEnergy);
        if (score > bestScore) {
            bestScore = score;
            bestOffset = offset;
        }
    }

    result.confidence = bestScore;
    result.arrivalSeconds = static_cast<double>(bestOffset * decimation) / sampleRate;
    result.relativeDelaySeconds = result.arrivalSeconds - expectedStartSeconds;
    result.detected = bestScore >= detectionThreshold;
    return result;
}

ProbeDetection detectCalibrationProbe(const std::vector<float>& recording,
                                      std::uint32_t sampleRate,
                                      double expectedStartSeconds,
                                      double searchBeforeSeconds,
                                      double searchAfterSeconds)
{
    const std::vector<float> fullProbe = generateCalibrationProbe(sampleRate);
    return detectKnownProbe(recording,
                            sampleRate,
                            fullProbe,
                            expectedStartSeconds,
                            searchBeforeSeconds,
                            searchAfterSeconds,
                            0.08);
}
