#include "frame_ring_buffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

FrameRingBuffer::FrameRingBuffer(std::size_t capacityFrames, std::size_t bytesPerFrame)
    : capacityFrames_(capacityFrames)
    , bytesPerFrame_(bytesPerFrame)
    , data_(capacityFrames * bytesPerFrame)
{
    if (capacityFrames == 0 || bytesPerFrame == 0) {
        throw std::invalid_argument("环形缓冲容量和帧大小必须大于零");
    }
}

void FrameRingBuffer::write(const BYTE* data, std::size_t frameCount, bool silent)
{
    if (frameCount == 0) {
        return;
    }
    if (!silent && data == nullptr) {
        throw std::invalid_argument("非静音音频数据不能为空");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (frameCount >= capacityFrames_) {
        const std::size_t skippedFrames = frameCount - capacityFrames_;
        const BYTE* tail = silent ? nullptr : data + skippedFrames * bytesPerFrame_;
        copyIntoBuffer(0, tail, capacityFrames_, silent);
        readFrame_ = 0;
        writeFrame_ = 0;
        overflowFrames_ += availableFrames_ + skippedFrames;
        availableFrames_ = capacityFrames_;
        return;
    }

    const std::size_t freeFrames = capacityFrames_ - availableFrames_;
    if (frameCount > freeFrames) {
        const std::size_t framesToDrop = frameCount - freeFrames;
        readFrame_ = (readFrame_ + framesToDrop) % capacityFrames_;
        availableFrames_ -= framesToDrop;
        overflowFrames_ += framesToDrop;
    }

    copyIntoBuffer(writeFrame_, data, frameCount, silent);
    writeFrame_ = (writeFrame_ + frameCount) % capacityFrames_;
    availableFrames_ += frameCount;
}

std::size_t FrameRingBuffer::read(BYTE* destination, std::size_t frameCount)
{
    if (destination == nullptr || frameCount == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t framesToRead = std::min(frameCount, availableFrames_);
    const std::size_t firstPart = std::min(framesToRead, capacityFrames_ - readFrame_);

    std::memcpy(destination,
                data_.data() + readFrame_ * bytesPerFrame_,
                firstPart * bytesPerFrame_);

    const std::size_t secondPart = framesToRead - firstPart;
    if (secondPart > 0) {
        std::memcpy(destination + firstPart * bytesPerFrame_,
                    data_.data(),
                    secondPart * bytesPerFrame_);
    }

    readFrame_ = (readFrame_ + framesToRead) % capacityFrames_;
    availableFrames_ -= framesToRead;
    return framesToRead;
}

std::size_t FrameRingBuffer::skip(std::size_t frameCount)
{
    if (frameCount == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t framesToSkip = std::min(frameCount, availableFrames_);
    readFrame_ = (readFrame_ + framesToSkip) % capacityFrames_;
    availableFrames_ -= framesToSkip;
    return framesToSkip;
}

void FrameRingBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    readFrame_ = 0;
    writeFrame_ = 0;
    availableFrames_ = 0;
}

std::size_t FrameRingBuffer::availableFrames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return availableFrames_;
}

std::size_t FrameRingBuffer::capacityFrames() const
{
    return capacityFrames_;
}

std::uint64_t FrameRingBuffer::overflowFrames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return overflowFrames_;
}

void FrameRingBuffer::copyIntoBuffer(std::size_t targetFrame,
                                     const BYTE* data,
                                     std::size_t frameCount,
                                     bool silent)
{
    const std::size_t firstPart = std::min(frameCount, capacityFrames_ - targetFrame);
    BYTE* firstTarget = data_.data() + targetFrame * bytesPerFrame_;

    if (silent) {
        std::memset(firstTarget, 0, firstPart * bytesPerFrame_);
    } else {
        std::memcpy(firstTarget, data, firstPart * bytesPerFrame_);
    }

    const std::size_t secondPart = frameCount - firstPart;
    if (secondPart == 0) {
        return;
    }

    if (silent) {
        std::memset(data_.data(), 0, secondPart * bytesPerFrame_);
    } else {
        std::memcpy(data_.data(), data + firstPart * bytesPerFrame_, secondPart * bytesPerFrame_);
    }
}
