#include "../src/audio_clock_model.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace
{
void testFitsClockAndConvertsCoordinates()
{
    constexpr double qpcFrequency = 10'000'000.0;
    constexpr double nominalRate = 48'000.0;
    constexpr double driftPpm = 125.0;
    const double actualRate = nominalRate * (1.0 + driftPpm / 1'000'000.0);

    AudioClockModel model(qpcFrequency, 32);
    for (std::uint64_t index = 0; index < 20; ++index) {
        const double seconds = static_cast<double>(index) * 0.25;
        const auto qpc = static_cast<std::uint64_t>(std::llround(seconds * qpcFrequency));
        const auto frame = static_cast<std::uint64_t>(
            std::llround(seconds * actualRate));
        assert(model.addSample(frame, qpc));
    }

    assert(model.ready());
    assert(std::abs(model.framesPerSecond() - actualRate) < 0.2);
    assert(std::abs(model.driftPpm(nominalRate) - driftPpm) < 5.0);
    const double qpc = 13.75 * qpcFrequency;
    const double expectedFrame = 13.75 * actualRate;
    assert(std::abs(model.frameAtQpc(qpc) - expectedFrame) < 1.0);
    assert(std::abs(model.qpcAtFrame(expectedFrame) - qpc) < 300.0);
}

void testRejectsNonMonotonicAndDownweightsOutlier()
{
    constexpr double qpcFrequency = 1'000'000.0;
    constexpr double nominalRate = 48'000.0;
    AudioClockModel model(qpcFrequency, 16);
    assert(model.addSample(0, 0));
    assert(model.addSample(48'000, 1'000'000));
    assert(!model.addSample(48'001, 1'000'000));
    assert(!model.addSample(47'999, 1'100'000));
    assert(model.addSample(96'000, 2'200'000));
    assert(model.addSample(144'000, 3'000'000));
    assert(model.addSample(192'000, 4'000'000));
    assert(std::abs(model.driftPpm(nominalRate)) < 1'000.0);
    assert(model.residualRmsFrames() > 0.0);
}

void testRequiresEnoughSamples()
{
    AudioClockModel model(1'000'000.0);
    assert(model.addSample(0, 0));
    assert(!model.ready());
    assert(model.addSample(48'000, 1'000'000));
    assert(!model.ready());
    assert(std::isnan(model.qpcAtFrame(10.0)));
    assert(model.addSample(96'000, 2'000'000));
    assert(model.ready());
}
}

int main()
{
    testFitsClockAndConvertsCoordinates();
    testRejectsNonMonotonicAndDownweightsOutlier();
    testRequiresEnoughSamples();
    std::cout << "AudioClockModel tests passed\n";
    return 0;
}
