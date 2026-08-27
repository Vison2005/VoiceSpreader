#pragma once

#include "audio_device.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class OutputWorker;

class RemoteMicrophoneOutput
{
public:
    using StatusCallback = std::function<void(const QString&)>;

    explicit RemoteMicrophoneOutput(StatusCallback statusCallback);
    ~RemoteMicrophoneOutput();

    RemoteMicrophoneOutput(const RemoteMicrophoneOutput&) = delete;
    RemoteMicrophoneOutput& operator=(const RemoteMicrophoneOutput&) = delete;

    bool start(const AudioDevice& device,
               std::uint32_t sampleRate,
               int bufferMilliseconds,
               int volumePercent);
    void stop();
    bool isActive() const;
    void setVolumePercent(int volumePercent);
    void push(std::uint32_t sampleRate, const std::vector<float>& samples);

private:
    static std::vector<unsigned char> createMonoFloatFormat(std::uint32_t sampleRate);

    StatusCallback statusCallback_;
    mutable std::mutex mutex_;
    std::unique_ptr<OutputWorker> worker_;
    std::uint32_t sampleRate_ = 0;
};
