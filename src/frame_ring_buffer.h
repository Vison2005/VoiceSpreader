#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class FrameRingBuffer
{
public:
    FrameRingBuffer(std::size_t capacityFrames, std::size_t bytesPerFrame);

    void write(const BYTE* data, std::size_t frameCount, bool silent);
    std::size_t read(BYTE* destination, std::size_t frameCount);
    std::size_t skip(std::size_t frameCount);
    std::size_t availableFrames() const;
    std::uint64_t overflowFrames() const;

private:
    void copyIntoBuffer(std::size_t targetFrame, const BYTE* data, std::size_t frameCount, bool silent);

    const std::size_t capacityFrames_;
    const std::size_t bytesPerFrame_;
    std::vector<BYTE> data_;

    mutable std::mutex mutex_;
    std::size_t readFrame_ = 0;
    std::size_t writeFrame_ = 0;
    std::size_t availableFrames_ = 0;
    std::uint64_t overflowFrames_ = 0;
};
