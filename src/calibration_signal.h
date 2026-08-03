#pragma once

#include <cstdint>
#include <vector>

struct ProbeDetection
{
    bool detected = false;
    double arrivalSeconds = 0.0;
    double relativeDelaySeconds = 0.0;
    double confidence = 0.0;
};

std::vector<float> generateCalibrationProbe(std::uint32_t sampleRate);

std::vector<float> generateUltrasonicProbe(std::uint32_t sampleRate);

std::vector<float> generateSpreadSpectrumProbe(std::uint32_t sampleRate,
                                               std::uint32_t seed = 0x51A7E3u);

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
