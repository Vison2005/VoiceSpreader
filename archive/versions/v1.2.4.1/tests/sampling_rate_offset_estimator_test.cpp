#include "../src/sampling_rate_offset_estimator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>

namespace
{
void testEstimatesDriftWithNoiseAndOutlier()
{
    constexpr double expectedPpm = 137.5;
    constexpr double initialDelaySeconds = 0.018;
    SamplingRateOffsetEstimator estimator(40, 6);
    for (int index = 0; index < 36; ++index) {
        const double timeSeconds = static_cast<double>(index) * 5.0;
        const double deterministicNoise = static_cast<double>((index % 5) - 2) * 8.0e-6;
        double delaySeconds = initialDelaySeconds
                              + expectedPpm * 1.0e-6 * timeSeconds
                              + deterministicNoise;
        double confidence = 0.9;
        if (index == 17) {
            delaySeconds += 0.012;
            confidence = 0.08;
        }
        assert(estimator.addSample(timeSeconds, delaySeconds, confidence));
    }

    assert(estimator.ready());
    assert(std::abs(estimator.driftPpm() - expectedPpm) < 1.5);
    assert(estimator.residualRmsSeconds() < 0.001);
    const double predicted = estimator.delayAt(200.0);
    const double expected = initialDelaySeconds + expectedPpm * 1.0e-6 * 200.0;
    assert(std::abs(predicted - expected) < 0.0001);
}

void testRejectsInvalidOrderAndRequiresTimeSpan()
{
    SamplingRateOffsetEstimator estimator(8, 3);
    assert(estimator.addSample(10.0, 0.01));
    assert(!estimator.addSample(10.0, 0.02));
    assert(!estimator.addSample(9.0, 0.02));
    assert(!estimator.addSample(11.0, 0.02, 0.0));
    assert(estimator.addSample(10.25, 0.01001));
    assert(estimator.addSample(10.5, 0.01002));
    assert(!estimator.ready());
    assert(estimator.addSample(11.25, 0.01005));
    assert(estimator.ready());
}

void testResetClearsEstimate()
{
    SamplingRateOffsetEstimator estimator(8, 3);
    assert(estimator.addSample(0.0, 0.0));
    assert(estimator.addSample(1.0, 0.0001));
    assert(estimator.addSample(2.0, 0.0002));
    assert(estimator.ready());
    estimator.reset();
    assert(!estimator.ready());
    assert(estimator.sampleCount() == 0);
    assert(estimator.driftPpm() == 0.0);
}
}

int main()
{
    testEstimatesDriftWithNoiseAndOutlier();
    testRejectsInvalidOrderAndRequiresTimeSpan();
    testResetClearsEstimate();
    std::cout << "SamplingRateOffsetEstimator tests passed\n";
    return 0;
}
