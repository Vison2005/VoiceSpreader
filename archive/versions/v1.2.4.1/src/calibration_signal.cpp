#include "calibration_signal.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace
{
constexpr double probeDurationSeconds = 0.12;
constexpr double startFrequencyHz = 600.0;
constexpr double endFrequencyHz = 5200.0;
constexpr double fadeDurationSeconds = 0.01;
constexpr double pi = 3.14159265358979323846;

using Complex = std::complex<double>;

std::size_t nextPowerOfTwo(std::size_t value)
{
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

void fft(std::vector<Complex>& values, bool inverse)
{
    if (values.size() < 2) {
        return;
    }

    for (std::size_t index = 1, reversed = 0; index < values.size(); ++index) {
        std::size_t bit = values.size() >> 1;
        for (; (reversed & bit) != 0; bit >>= 1) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }

    for (std::size_t length = 2; length <= values.size(); length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * pi
                             / static_cast<double>(length);
        const Complex step(std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < values.size(); offset += length) {
            Complex phase(1.0, 0.0);
            const std::size_t half = length / 2;
            for (std::size_t index = 0; index < half; ++index) {
                const Complex even = values[offset + index];
                const Complex odd = phase * values[offset + index + half];
                values[offset + index] = even + odd;
                values[offset + index + half] = even - odd;
                phase *= step;
            }
        }
    }

    if (inverse) {
        const double scale = 1.0 / static_cast<double>(values.size());
        for (Complex& value : values) {
            value *= scale;
        }
    }
}

std::vector<double> gccPhatCorrelation(const std::vector<float>& recording,
                                        const std::vector<float>& probe)
{
    const std::size_t fftSize = nextPowerOfTwo(recording.size() + probe.size() - 1);
    std::vector<Complex> recordingSpectrum(fftSize);
    std::vector<Complex> probeSpectrum(fftSize);
    for (std::size_t index = 0; index < recording.size(); ++index) {
        recordingSpectrum[index] = static_cast<double>(recording[index]);
    }
    for (std::size_t index = 0; index < probe.size(); ++index) {
        probeSpectrum[index] = static_cast<double>(probe[index]);
    }
    fft(recordingSpectrum, false);
    fft(probeSpectrum, false);
    for (std::size_t index = 0; index < fftSize; ++index) {
        const Complex crossSpectrum = recordingSpectrum[index]
                                      * std::conj(probeSpectrum[index]);
        const double magnitude = std::abs(crossSpectrum);
        recordingSpectrum[index] = magnitude > 1.0e-12
                                       ? crossSpectrum / magnitude
                                       : Complex(0.0, 0.0);
    }
    fft(recordingSpectrum, true);

    std::vector<double> result(fftSize, 0.0);
    for (std::size_t index = 0; index < fftSize; ++index) {
        result[index] = recordingSpectrum[index].real();
    }
    return result;
}

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

std::vector<float> generateExponentialSineSweep(std::uint32_t sampleRate,
                                                double durationSeconds,
                                                double sweepStartFrequencyHz,
                                                double sweepEndFrequencyHz)
{
    const double nyquist = static_cast<double>(sampleRate) * 0.5;
    if (sampleRate == 0 || !std::isfinite(durationSeconds)
        || !std::isfinite(sweepStartFrequencyHz)
        || !std::isfinite(sweepEndFrequencyHz)
        || durationSeconds <= 0.0 || sweepStartFrequencyHz <= 0.0
        || sweepEndFrequencyHz <= sweepStartFrequencyHz
        || sweepEndFrequencyHz >= nyquist) {
        return {};
    }

    const std::size_t frameCount = static_cast<std::size_t>(
        std::llround(durationSeconds * sampleRate));
    if (frameCount < 2) {
        return {};
    }
    const double logarithmicRatio = std::log(sweepEndFrequencyHz
                                             / sweepStartFrequencyHz);
    const double sweepConstant = durationSeconds / logarithmicRatio;
    const double fadeSeconds = std::min(0.02, durationSeconds * 0.04);
    std::vector<float> result(frameCount, 0.0F);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const double seconds = static_cast<double>(frame) / sampleRate;
        const double phase = 2.0 * pi * sweepStartFrequencyHz * sweepConstant
                             * std::expm1(seconds / sweepConstant);
        result[frame] = static_cast<float>(
            std::sin(phase) * smoothFade(seconds, durationSeconds, fadeSeconds));
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

    if (firstOffset > lastOffset) {
        return result;
    }

    // 先用 GCC-PHAT 的互谱相位定位候选峰，再只对峰值附近计算归一化相关。
    // 这样把全搜索从 O(N*M) 降为 O(N log N)，同时保留原有置信度定义。
    const std::vector<double> phatCorrelation = gccPhatCorrelation(samples, probe);
    const std::size_t maximumOffset = samples.size() - probe.size();
    std::size_t bestOffset = firstOffset;
    double bestPhatScore = -1.0;
    for (std::size_t offset = firstOffset; offset <= lastOffset; ++offset) {
        if (offset >= phatCorrelation.size()) {
            break;
        }
        const double score = std::abs(phatCorrelation[offset]);
        if (score > bestPhatScore) {
            bestPhatScore = score;
            bestOffset = offset;
        }
    }

    auto normalizedScore = [&](std::size_t offset) {
        if (offset > maximumOffset) {
            return 0.0;
        }
        double dot = 0.0;
        for (std::size_t index = 0; index < probe.size(); ++index) {
            dot += static_cast<double>(probe[index]) * samples[offset + index];
        }
        const double windowEnergy = energyPrefix[offset + probe.size()]
                                    - energyPrefix[offset];
        if (windowEnergy <= std::numeric_limits<double>::epsilon()) {
            return 0.0;
        }
        return std::abs(dot) / std::sqrt(probeEnergy * windowEnergy);
    };

    const double bestScore = normalizedScore(bestOffset);
    double fractionalOffset = 0.0;
    if (bestOffset > firstOffset && bestOffset < lastOffset) {
        const double previousScore = normalizedScore(bestOffset - 1);
        const double nextScore = normalizedScore(bestOffset + 1);
        const double denominator = previousScore - 2.0 * bestScore + nextScore;
        if (std::abs(denominator) > 1.0e-12) {
            fractionalOffset = std::clamp(
                0.5 * (previousScore - nextScore) / denominator, -0.5, 0.5);
        }
    }

    result.confidence = bestScore;
    result.arrivalSeconds = (static_cast<double>(bestOffset) + fractionalOffset)
                            * decimation / sampleRate;
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

ProbeDetection detectSpreadSpectrumProbe(const std::vector<float>& recording,
                                         std::uint32_t sampleRate,
                                         const std::vector<float>& referenceProbe,
                                         double expectedStartSeconds,
                                         double searchBeforeSeconds,
                                         double searchAfterSeconds)
{
    const double highFrequency = std::min(
        18100.0,
        static_cast<double>(sampleRate) * 0.5 - 250.0);
    if (highFrequency <= 13500.0) {
        return {};
    }
    return detectKnownProbe(recording,
                            sampleRate,
                            referenceProbe,
                            expectedStartSeconds,
                            searchBeforeSeconds,
                            searchAfterSeconds,
                            0.035,
                            13500.0,
                            highFrequency);
}

SweepImpulseResponseAnalysis analyzeExponentialSweepImpulseResponse(
    const std::vector<float>& recording,
    std::uint32_t sampleRate,
    const std::vector<float>& referenceSweep,
    double expectedStartSeconds,
    double searchBeforeSeconds,
    double maximumDelaySeconds)
{
    SweepImpulseResponseAnalysis result;
    if (recording.empty() || referenceSweep.empty() || sampleRate == 0
        || !std::isfinite(expectedStartSeconds) || !std::isfinite(searchBeforeSeconds)
        || !std::isfinite(maximumDelaySeconds) || expectedStartSeconds < 0.0
        || searchBeforeSeconds < 0.0 || maximumDelaySeconds <= 0.0) {
        return result;
    }
    result.status = ProbeDetectionStatus::RecordingTooShort;

    const double sweepDurationSeconds = static_cast<double>(referenceSweep.size())
                                        / sampleRate;
    const double segmentStartSeconds = std::max(0.0,
                                                 expectedStartSeconds
                                                     - searchBeforeSeconds);
    const double availablePreRollSeconds = expectedStartSeconds - segmentStartSeconds;
    const double segmentEndSeconds = expectedStartSeconds + sweepDurationSeconds
                                     + maximumDelaySeconds + 0.08;
    const std::size_t segmentStartFrame = static_cast<std::size_t>(
        std::floor(segmentStartSeconds * sampleRate));
    const std::size_t segmentEndFrame = std::min<std::size_t>(
        recording.size(),
        static_cast<std::size_t>(std::ceil(segmentEndSeconds * sampleRate)));
    if (segmentEndFrame <= segmentStartFrame + referenceSweep.size()) {
        return result;
    }

    const std::size_t segmentFrames = segmentEndFrame - segmentStartFrame;
    const std::size_t excitationOffset = static_cast<std::size_t>(
        std::llround(availablePreRollSeconds * sampleRate));
    if (excitationOffset + referenceSweep.size() > segmentFrames) {
        return result;
    }

    const std::size_t fftSize = nextPowerOfTwo(segmentFrames * 2);
    std::vector<Complex> recordingSpectrum(fftSize);
    std::vector<Complex> excitationSpectrum(fftSize);
    for (std::size_t index = 0; index < segmentFrames; ++index) {
        recordingSpectrum[index] = recording[segmentStartFrame + index];
    }
    for (std::size_t index = 0; index < referenceSweep.size(); ++index) {
        excitationSpectrum[excitationOffset + index] = referenceSweep[index];
    }
    fft(recordingSpectrum, false);
    fft(excitationSpectrum, false);

    double maximumExcitationPower = 0.0;
    for (const Complex& value : excitationSpectrum) {
        maximumExcitationPower = std::max(maximumExcitationPower, std::norm(value));
    }
    if (maximumExcitationPower <= std::numeric_limits<double>::epsilon()) {
        result.status = ProbeDetectionStatus::NoUsableEnergy;
        return result;
    }
    const double regularization = maximumExcitationPower * 1.0e-7;
    for (std::size_t index = 0; index < fftSize; ++index) {
        const double power = std::norm(excitationSpectrum[index]);
        recordingSpectrum[index] = recordingSpectrum[index]
                                   * std::conj(excitationSpectrum[index])
                                   / (power + regularization);
    }
    fft(recordingSpectrum, true);

    const std::size_t searchFrames = std::min<std::size_t>(
        static_cast<std::size_t>(std::ceil(maximumDelaySeconds * sampleRate)),
        fftSize - 1);
    if (searchFrames < 3) {
        return result;
    }
    const std::size_t smoothingRadius = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(std::llround(sampleRate * 0.00025)));
    std::vector<double> energyPrefix(searchFrames + smoothingRadius + 2, 0.0);
    const std::size_t availableResponseFrames = std::min<std::size_t>(
        energyPrefix.size() - 1,
        recordingSpectrum.size());
    for (std::size_t index = 0; index < availableResponseFrames; ++index) {
        energyPrefix[index + 1] = energyPrefix[index]
                                  + std::norm(recordingSpectrum[index]);
    }
    for (std::size_t index = availableResponseFrames;
         index + 1 < energyPrefix.size();
         ++index) {
        energyPrefix[index + 1] = energyPrefix[index];
    }

    std::vector<double> envelope(searchFrames, 0.0);
    double maximumEnvelope = 0.0;
    std::size_t maximumEnvelopeIndex = 0;
    for (std::size_t index = 0; index < searchFrames; ++index) {
        const std::size_t first = index > smoothingRadius ? index - smoothingRadius : 0;
        const std::size_t last = std::min(index + smoothingRadius + 1,
                                          energyPrefix.size() - 1);
        envelope[index] = std::sqrt(
            std::max(0.0, energyPrefix[last] - energyPrefix[first])
            / std::max<std::size_t>(1, last - first));
        if (envelope[index] > maximumEnvelope) {
            maximumEnvelope = envelope[index];
            maximumEnvelopeIndex = index;
        }
    }
    if (maximumEnvelope <= std::numeric_limits<double>::epsilon()) {
        result.status = ProbeDetectionStatus::NoUsableEnergy;
        return result;
    }

    std::vector<double> sortedEnvelope = envelope;
    const auto middle = sortedEnvelope.begin()
                        + static_cast<std::ptrdiff_t>(sortedEnvelope.size() / 2);
    std::nth_element(sortedEnvelope.begin(), middle, sortedEnvelope.end());
    const double noiseEnvelope = std::max(*middle,
                                          std::numeric_limits<double>::epsilon());
    const double candidateThreshold = std::max(maximumEnvelope * 0.05,
                                               noiseEnvelope * 5.0);
    std::vector<std::size_t> candidateIndices;
    for (std::size_t index = 1; index + 1 < envelope.size(); ++index) {
        if (envelope[index] >= candidateThreshold
            && envelope[index] >= envelope[index - 1]
            && envelope[index] > envelope[index + 1]) {
            candidateIndices.push_back(index);
        }
    }
    if (candidateIndices.empty()) {
        candidateIndices.push_back(maximumEnvelopeIndex);
    }

    // 先按峰值强度做非极大值抑制，避免同一个宽峰产生大量相邻候选。
    std::sort(candidateIndices.begin(), candidateIndices.end(),
              [&envelope](std::size_t left, std::size_t right) {
                  return envelope[left] > envelope[right];
              });
    const std::size_t minimumPeakSeparationFrames = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(std::llround(sampleRate * 0.001)));
    constexpr std::size_t maximumCandidateCount = 12;
    std::vector<std::size_t> selectedIndices;
    for (std::size_t index : candidateIndices) {
        const bool overlapsSelected = std::any_of(
            selectedIndices.cbegin(),
            selectedIndices.cend(),
            [index, minimumPeakSeparationFrames](std::size_t selected) {
                return index > selected
                           ? index - selected < minimumPeakSeparationFrames
                           : selected - index < minimumPeakSeparationFrames;
            });
        if (!overlapsSelected) {
            selectedIndices.push_back(index);
            if (selectedIndices.size() >= maximumCandidateCount) {
                break;
            }
        }
    }

    result.peaks.reserve(selectedIndices.size());
    for (std::size_t peakIndex : selectedIndices) {
        double fractionalIndex = 0.0;
        if (peakIndex > 0 && peakIndex + 1 < envelope.size()) {
            const double previous = envelope[peakIndex - 1];
            const double current = envelope[peakIndex];
            const double next = envelope[peakIndex + 1];
            const double denominator = previous - 2.0 * current + next;
            if (std::abs(denominator) > 1.0e-15) {
                fractionalIndex = std::clamp(
                    0.5 * (previous - next) / denominator,
                    -0.5,
                    0.5);
            }
        }

        ImpulseResponsePeak peak;
        const double peakEnvelope = envelope[peakIndex];
        peak.delaySeconds = (static_cast<double>(peakIndex) + fractionalIndex)
                            / sampleRate;
        peak.signalToNoiseDb = 20.0 * std::log10(peakEnvelope / noiseEnvelope);
        peak.relativeToStrongest = peakEnvelope / maximumEnvelope;
        peak.confidence = std::clamp((peak.signalToNoiseDb - 10.0) / 20.0,
                                     0.0,
                                     1.0)
                          * std::clamp(peak.relativeToStrongest / 0.05, 0.0, 1.0);
        result.peaks.push_back(peak);
    }
    std::sort(result.peaks.begin(), result.peaks.end(),
              [](const ImpulseResponsePeak& left, const ImpulseResponsePeak& right) {
                  return left.delaySeconds < right.delaySeconds;
              });
    result.status = result.peaks.empty() ? ProbeDetectionStatus::LowConfidence
                                        : ProbeDetectionStatus::Detected;
    return result;
}

ProbeDetection detectExponentialSweepImpulseResponse(
    const std::vector<float>& recording,
    std::uint32_t sampleRate,
    const std::vector<float>& referenceSweep,
    double expectedStartSeconds,
    double searchBeforeSeconds,
    double maximumDelaySeconds)
{
    const SweepImpulseResponseAnalysis analysis = analyzeExponentialSweepImpulseResponse(
        recording,
        sampleRate,
        referenceSweep,
        expectedStartSeconds,
        searchBeforeSeconds,
        maximumDelaySeconds);
    ProbeDetection result;
    result.status = analysis.status;
    if (analysis.peaks.empty()) {
        return result;
    }
    const ImpulseResponsePeak& peak = analysis.peaks.front();
    result.arrivalSeconds = peak.delaySeconds;
    result.relativeDelaySeconds = peak.delaySeconds;
    result.confidence = peak.confidence;
    result.signalToNoiseDb = peak.signalToNoiseDb;
    result.directToStrongestRatio = peak.relativeToStrongest;
    result.detected = peak.signalToNoiseDb >= 14.0;
    result.status = result.detected ? ProbeDetectionStatus::Detected
                                    : ProbeDetectionStatus::LowConfidence;
    return result;
}

ProbeDetection selectConsistentImpulseResponsePeakPair(
    const SweepImpulseResponseAnalysis& first,
    const SweepImpulseResponseAnalysis& second,
    double maximumSpreadSeconds)
{
    ProbeDetection result;
    if (!std::isfinite(maximumSpreadSeconds) || maximumSpreadSeconds <= 0.0) {
        return result;
    }
    if (first.peaks.empty() || second.peaks.empty()) {
        result.status = first.status != ProbeDetectionStatus::Detected
                            ? first.status
                            : second.status;
        return result;
    }

    const ImpulseResponsePeak* bestFirst = nullptr;
    const ImpulseResponsePeak* bestSecond = nullptr;
    double bestScore = 0.0;
    double bestSpread = 0.0;
    for (const ImpulseResponsePeak& firstPeak : first.peaks) {
        for (const ImpulseResponsePeak& secondPeak : second.peaks) {
            const double spread = std::abs(firstPeak.delaySeconds
                                           - secondPeak.delaySeconds);
            if (spread > maximumSpreadSeconds) {
                continue;
            }
            const double minimumSnr = std::min(firstPeak.signalToNoiseDb,
                                               secondPeak.signalToNoiseDb);
            const double relativeStrength = std::sqrt(
                firstPeak.relativeToStrongest * secondPeak.relativeToStrongest);
            if (minimumSnr < 14.0 || relativeStrength < 0.12) {
                continue;
            }
            const double consistency = 1.0 - spread / maximumSpreadSeconds;
            const double snrWeight = std::clamp((minimumSnr - 12.0) / 18.0,
                                                0.0,
                                                1.0);
            const double score = relativeStrength * (0.75 + 0.25 * consistency)
                                 * (0.70 + 0.30 * snrWeight);
            if (score > bestScore) {
                bestScore = score;
                bestFirst = &firstPeak;
                bestSecond = &secondPeak;
                bestSpread = spread;
            }
        }
    }

    if (bestFirst == nullptr || bestSecond == nullptr) {
        result.status = ProbeDetectionStatus::LowConfidence;
        return result;
    }
    result.relativeDelaySeconds = (bestFirst->delaySeconds + bestSecond->delaySeconds)
                                  * 0.5;
    result.arrivalSeconds = result.relativeDelaySeconds;
    result.signalToNoiseDb = std::min(bestFirst->signalToNoiseDb,
                                      bestSecond->signalToNoiseDb);
    result.directToStrongestRatio = std::sqrt(
        bestFirst->relativeToStrongest * bestSecond->relativeToStrongest);
    result.consistencySpreadSeconds = bestSpread;
    const double consistency = 1.0 - bestSpread / maximumSpreadSeconds;
    result.confidence = std::clamp(bestScore * (0.8 + 0.2 * consistency),
                                   0.0,
                                   1.0);
    result.detected = true;
    result.status = ProbeDetectionStatus::Detected;
    return result;
}
