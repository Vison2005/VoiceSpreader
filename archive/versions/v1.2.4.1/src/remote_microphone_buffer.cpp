#include "remote_microphone_buffer.h"

#include <algorithm>
#include <cmath>

void RemoteMicrophoneBuffer::setConnected(bool connected, std::uint32_t sampleRate)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = connected;
        if (!connected || (sampleRate != 0 && sampleRate_ != sampleRate)) {
            samples_.clear();
            firstFrameIndex_ = 0;
            nextFrameIndex_ = 0;
            clockModel_.reset();
        }
        if (sampleRate != 0) {
            sampleRate_ = sampleRate;
        }
    }
    condition_.notify_all();
}

void RemoteMicrophoneBuffer::addClockSample(std::uint64_t frameIndex,
                                             std::uint64_t monotonicNanoseconds)
{
    if (monotonicNanoseconds == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_) {
        clockModel_.addSample(frameIndex, monotonicNanoseconds);
    }
}

bool RemoteMicrophoneBuffer::isConnected() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

std::uint32_t RemoteMicrophoneBuffer::sampleRate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sampleRate_;
}

void RemoteMicrophoneBuffer::append(std::uint64_t firstFrameIndex,
                                    std::uint32_t sampleRate,
                                    const std::vector<float>& samples)
{
    if (sampleRate == 0 || samples.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sampleRate_ != sampleRate) {
            samples_.clear();
            firstFrameIndex_ = firstFrameIndex;
            nextFrameIndex_ = firstFrameIndex;
            sampleRate_ = sampleRate;
        }
        connected_ = true;
        if (samples_.empty()) {
            firstFrameIndex_ = firstFrameIndex;
            nextFrameIndex_ = firstFrameIndex;
        }

        std::size_t sourceOffset = 0;
        if (firstFrameIndex < nextFrameIndex_) {
            sourceOffset = static_cast<std::size_t>(
                std::min<std::uint64_t>(nextFrameIndex_ - firstFrameIndex,
                                        samples.size()));
        } else if (firstFrameIndex > nextFrameIndex_) {
            const std::uint64_t gap = firstFrameIndex - nextFrameIndex_;
            const std::uint64_t maximumGap = static_cast<std::uint64_t>(sampleRate_) * 2;
            if (gap > maximumGap) {
                samples_.clear();
                firstFrameIndex_ = firstFrameIndex;
            } else {
                samples_.insert(samples_.end(), static_cast<std::size_t>(gap), 0.0F);
            }
            nextFrameIndex_ = firstFrameIndex;
        }

        for (std::size_t index = sourceOffset; index < samples.size(); ++index) {
            samples_.push_back(samples[index]);
            ++nextFrameIndex_;
        }
        trimLocked();
    }
    condition_.notify_all();
}

std::uint64_t RemoteMicrophoneBuffer::latestFrameIndex() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nextFrameIndex_;
}

std::size_t RemoteMicrophoneBuffer::bufferedFrames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

std::uint64_t RemoteMicrophoneBuffer::trimmedFrames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return trimmedFrames_;
}

bool RemoteMicrophoneBuffer::waitUntilFrame(std::uint64_t frameIndex,
                                            std::chrono::milliseconds timeout,
                                            const std::atomic_bool* cancellation)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] {
        return nextFrameIndex_ >= frameIndex || !connected_
               || (cancellation != nullptr && cancellation->load());
    }) && nextFrameIndex_ >= frameIndex;
}

RemoteAudioSnapshot RemoteMicrophoneBuffer::snapshotFrom(
    std::uint64_t firstFrameIndex) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    RemoteAudioSnapshot snapshot;
    snapshot.sampleRate = sampleRate_;
    if (clockModel_.ready()) {
        const double measuredRate = clockModel_.framesPerSecond();
        if (std::isfinite(measuredRate)
            && measuredRate >= static_cast<double>(sampleRate_) * 0.95
            && measuredRate <= static_cast<double>(sampleRate_) * 1.05) {
            snapshot.sampleRate = static_cast<std::uint32_t>(std::lround(measuredRate));
        }
    }
    snapshot.firstFrameIndex = std::max(firstFrameIndex, firstFrameIndex_);
    if (snapshot.firstFrameIndex >= nextFrameIndex_ || samples_.empty()) {
        return snapshot;
    }

    const std::size_t offset = static_cast<std::size_t>(
        snapshot.firstFrameIndex - firstFrameIndex_);
    snapshot.samples.reserve(samples_.size() - offset);
    auto iterator = samples_.cbegin();
    std::advance(iterator, static_cast<std::ptrdiff_t>(offset));
    snapshot.samples.assign(iterator, samples_.cend());
    return snapshot;
}

void RemoteMicrophoneBuffer::trimLocked()
{
    const std::size_t maximumFrames = static_cast<std::size_t>(sampleRate_) * 30;
    while (samples_.size() > maximumFrames) {
        samples_.pop_front();
        ++firstFrameIndex_;
        ++trimmedFrames_;
    }
}
