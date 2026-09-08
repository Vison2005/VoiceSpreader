#include "../src/polyphase_resampler.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
void testConstantSignalIsPreserved()
{
    PolyphaseResampler resampler(2);
    std::vector<float> samples(64 * 2, 0.37F);
    for (double position : {0.0, 0.125, 1.25, 8.5, 31.75, 63.0}) {
        assert(std::abs(resampler.sample(samples, 64, position, 0) - 0.37F) < 0.002F);
        assert(std::abs(resampler.sample(samples, 64, position, 1) - 0.37F) < 0.002F);
    }
}

void testFractionalDelayIsSmooth()
{
    PolyphaseResampler resampler(1);
    std::vector<float> samples(128);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = std::sin(static_cast<float>(index) * 0.11F);
    }
    const float first = resampler.sample(samples, samples.size(), 48.10, 0);
    const float second = resampler.sample(samples, samples.size(), 48.20, 0);
    const float third = resampler.sample(samples, samples.size(), 48.30, 0);
    assert(std::abs(second - first) < 0.2F);
    assert(std::abs(third - second) < 0.2F);
}

void testInvalidInputIsSilent()
{
    PolyphaseResampler resampler(1);
    assert(resampler.sample({}, 0, 0.0, 0) == 0.0F);
    assert(resampler.sample(std::vector<float>{1.0F}, 1, 0.0, 2) == 0.0F);
}
}

int main()
{
    testConstantSignalIsPreserved();
    testFractionalDelayIsSmooth();
    testInvalidInputIsSilent();
    std::cout << "PolyphaseResampler tests passed\n";
    return 0;
}
