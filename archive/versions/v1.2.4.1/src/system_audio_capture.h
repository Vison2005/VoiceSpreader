#pragma once

#include "audio_device.h"

#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

class SystemAudioCapture
{
public:
    using StatusCallback = std::function<void(const QString&)>;
    using PcmCallback = std::function<void(std::uint64_t firstFrameIndex,
                                           std::uint32_t sampleRate,
                                           std::uint16_t channels,
                                           const std::int16_t* samples,
                                           std::size_t sampleCount)>;

    explicit SystemAudioCapture(StatusCallback statusCallback);
    ~SystemAudioCapture();

    SystemAudioCapture(const SystemAudioCapture&) = delete;
    SystemAudioCapture& operator=(const SystemAudioCapture&) = delete;

    bool start(const AudioDevice& device, int volumePercent, PcmCallback pcmCallback);
    void stop();
    bool isActive() const;
    void setVolumePercent(int volumePercent);

private:
    void run(AudioDevice device);
    void finishInitialization(bool succeeded, const QString& failureMessage = {});

    StatusCallback statusCallback_;
    PcmCallback pcmCallback_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> active_{false};
    std::atomic<int> volumePercent_{100};
    mutable std::mutex initializationMutex_;
    std::condition_variable initializationCondition_;
    bool initializationFinished_ = false;
    bool initializationSucceeded_ = false;
    QString failureMessage_;
};
