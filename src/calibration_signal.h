#pragma once

#include <cstdint>
#include <vector>

enum class ProbeDetectionStatus
{
    InvalidInput,
    RecordingTooShort,
    NoUsableEnergy,
    LowConfidence,
    Detected
};

struct ProbeDetection
{
    bool detected = false;
    double arrivalSeconds = 0.0;
    double relativeDelaySeconds = 0.0;
    double confidence = 0.0;
    double signalToNoiseDb = 0.0;
    double directToStrongestRatio = 0.0;
    double consistencySpreadSeconds = 0.0;
    ProbeDetectionStatus status = ProbeDetectionStatus::InvalidInput;
};

struct ImpulseResponsePeak
{
    double delaySeconds = 0.0;
    double confidence = 0.0;
    double signalToNoiseDb = 0.0;
    double relativeToStrongest = 0.0;
};

struct SweepImpulseResponseAnalysis
{
    ProbeDetectionStatus status = ProbeDetectionStatus::InvalidInput;
    std::vector<ImpulseResponsePeak> peaks;
};

std::vector<float> generateCalibrationProbe(std::uint32_t sampleRate);

std::vector<float> generateUltrasonicProbe(std::uint32_t sampleRate);

std::vector<float> generateSpreadSpectrumProbe(std::uint32_t sampleRate,
                                               std::uint32_t seed = 0x51A7E3u);

std::vector<float> generateExponentialSineSweep(std::uint32_t sampleRate,
                                                double durationSeconds = 0.8,
                                                double sweepStartFrequencyHz = 100.0,
                                                double sweepEndFrequencyHz = 18000.0);

ProbeDetection detectKnownProbe(const std::vector<float>& recording,
                                std::uint32_t sampleRate,
                                const std::vector<float>& referenceProbe,
                                double expectedStartSeconds,
                                double searchBeforeSeconds = 0.05,
                                double searchAfterSeconds = 0.45,
                                double detectionThreshold = 0.08,
                                double bandLowHz = 0.0,
                                double bandHighHz = 0.0);

ProbeDetection detectCalibrationProbe(const std::vector<float>& recording,
                                      std::uint32_t sampleRate,
                                      double expectedStartSeconds,
                                      double searchBeforeSeconds = 0.05,
                                      double searchAfterSeconds = 0.45);

ProbeDetection detectSpreadSpectrumProbe(const std::vector<float>& recording,
                                         std::uint32_t sampleRate,
                                         const std::vector<float>& referenceProbe,
                                         double expectedStartSeconds,
                                         double searchBeforeSeconds = 0.05,
                                         double searchAfterSeconds = 0.45);

// 对 ESS 录音做正则化反卷积，并返回最早可信直接声脉冲的相对时延。
ProbeDetection detectExponentialSweepImpulseResponse(
    const std::vector<float>& recording,
    std::uint32_t sampleRate,
    const std::vector<float>& referenceSweep,
    double expectedStartSeconds,
    double searchBeforeSeconds = 0.05,
    double maximumDelaySeconds = 1.2);

// 保留脉冲响应中的多个可信峰，供重复测量做跨轮一致性匹配。
SweepImpulseResponseAnalysis analyzeExponentialSweepImpulseResponse(
    const std::vector<float>& recording,
    std::uint32_t sampleRate,
    const std::vector<float>& referenceSweep,
    double expectedStartSeconds,
    double searchBeforeSeconds = 0.05,
    double maximumDelaySeconds = 1.2);

// 在两轮 ESS 的候选峰中选择时间位置一致且能量较强的一对。
ProbeDetection selectConsistentImpulseResponsePeakPair(
    const SweepImpulseResponseAnalysis& first,
    const SweepImpulseResponseAnalysis& second,
    double maximumSpreadSeconds = 0.002);
