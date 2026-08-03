#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct RemoteAudioSnapshot
{
    std::uint32_t sampleRate = 0;
    std::uint64_t firstFrameIndex = 0;
    std::vector<float> samples;
};

class RemoteMicrophoneBuffer
{
public:
    void setConnected(bool connected, std::uint32_t sampleRate = 0);
    bool isConnected() const;
    std::uint32_t sampleRate() const;
    void append(std::uint64_t firstFrameIndex,
                std::uint32_t sampleRate,
                const std::vector<float>& samples);
    std::uint64_t latestFrameIndex() const;
    bool waitUntilFrame(std::uint64_t frameIndex,
                        std::chrono::milliseconds timeout,
                        const std::atomic_bool* cancellation = nullptr);
    RemoteAudioSnapshot snapshotFrom(std::uint64_t firstFrameIndex) const;

private:
    void trimLocked();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<float> samples_;
    std::uint64_t firstFrameIndex_ = 0;
    std::uint64_t nextFrameIndex_ = 0;
    std::uint32_t sampleRate_ = 0;
    bool connected_ = false;
};
