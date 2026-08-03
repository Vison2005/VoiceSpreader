#include "../src/frame_ring_buffer.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <iostream>

namespace
{
template<std::size_t Size>
bool equals(const std::array<BYTE, Size>& actual, const std::array<BYTE, Size>& expected)
{
    for (std::size_t index = 0; index < Size; ++index) {
        if (actual[index] != expected[index]) {
            std::cerr << "Mismatch at index " << index
                      << ": actual=" << static_cast<int>(actual[index])
                      << ", expected=" << static_cast<int>(expected[index]) << '\n';
            return false;
        }
    }
    return true;
}

bool testWrapAround()
{
    FrameRingBuffer buffer(8, 1);
    const std::array<BYTE, 4> first{1, 2, 3, 4};
    buffer.write(first.data(), first.size(), false);

    std::array<BYTE, 2> prefix{};
    if (buffer.read(prefix.data(), prefix.size()) != prefix.size()
        || !equals(prefix, std::array<BYTE, 2>{1, 2})) {
        return false;
    }

    const std::array<BYTE, 4> second{5, 6, 7, 8};
    buffer.write(second.data(), second.size(), false);

    std::array<BYTE, 6> remaining{};
    return buffer.read(remaining.data(), remaining.size()) == remaining.size()
           && equals(remaining, std::array<BYTE, 6>{3, 4, 5, 6, 7, 8});
}

bool testOverflowKeepsNewestFrames()
{
    FrameRingBuffer buffer(4, 1);
    const std::array<BYTE, 3> first{1, 2, 3};
    const std::array<BYTE, 3> second{4, 5, 6};
    buffer.write(first.data(), first.size(), false);
    buffer.write(second.data(), second.size(), false);

    std::array<BYTE, 4> output{};
    return buffer.overflowFrames() == 2
           && buffer.read(output.data(), output.size()) == output.size()
           && equals(output, std::array<BYTE, 4>{3, 4, 5, 6});
}

bool testSilentFrames()
{
    FrameRingBuffer buffer(4, 1);
    buffer.write(nullptr, 3, true);

    std::array<BYTE, 3> output{9, 9, 9};
    return buffer.read(output.data(), output.size()) == output.size()
           && equals(output, std::array<BYTE, 3>{0, 0, 0});
}

bool testSkipFrames()
{
    FrameRingBuffer buffer(8, 1);
    const std::array<BYTE, 6> input{1, 2, 3, 4, 5, 6};
    buffer.write(input.data(), input.size(), false);

    if (buffer.skip(2) != 2 || buffer.availableFrames() != 4) {
        return false;
    }

    std::array<BYTE, 4> output{};
    return buffer.read(output.data(), output.size()) == output.size()
           && equals(output, std::array<BYTE, 4>{3, 4, 5, 6});
}
}

int main()
{
    if (!testWrapAround()) {
        std::cerr << "testWrapAround failed\n";
        return 1;
    }
    if (!testOverflowKeepsNewestFrames()) {
        std::cerr << "testOverflowKeepsNewestFrames failed\n";
        return 1;
    }
    if (!testSilentFrames()) {
        std::cerr << "testSilentFrames failed\n";
        return 1;
    }
    if (!testSkipFrames()) {
        std::cerr << "testSkipFrames failed\n";
        return 1;
    }

    std::cout << "FrameRingBuffer tests passed\n";
    return 0;
}
