#pragma once

#include "audio_device.h"
#include "frame_ring_buffer.h"

#include <Windows.h>

#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class OutputWorker
{
public:
    using StatusCallback = std::function<void(const QString&)>;

    OutputWorker(AudioDevice device,
                 std::vector<BYTE> waveFormat,
                 int targetBufferMilliseconds,
                 int volumePercent,
                 bool preferExclusiveMode,
                 StatusCallback statusCallback);
    ~OutputWorker();

    OutputWorker(const OutputWorker&) = delete;
    OutputWorker& operator=(const OutputWorker&) = delete;

    void start();
    bool waitUntilInitialized(std::chrono::milliseconds timeout);
    void stop();
    void push(const BYTE* data,
              std::size_t frameCount,
              bool silent,
              bool allowAcousticProbe = true);
    void setVolumePercent(int volumePercent);
    void setSynchronizationMarginMilliseconds(int marginMilliseconds);
    void setManualDelayMilliseconds(int delayMilliseconds);
    void setAutomaticDelayMilliseconds(int delayMilliseconds);
    void setAcousticDelayMilliseconds(int delayMilliseconds);
    std::uint64_t scheduleAcousticProbe(const std::vector<float>& probe,
                                        float amplitude);
    bool waitForAcousticProbeStart(std::uint64_t generation,
                                   std::chrono::milliseconds timeout,
                                   std::int64_t* qpcHundredNanoseconds);
    void cancelAcousticProbe(std::uint64_t generation);

    bool initializedSuccessfully() const;
    QString failureMessage() const;
    std::uint64_t underrunFrames() const;
    std::uint64_t correctedFrames() const;
    double streamLatencyMilliseconds() const;
    double enginePeriodMilliseconds() const;
    bool usesLowLatencyMode() const;
    int targetBufferMilliseconds() const;
    int acousticDelayMilliseconds() const;
    std::uint32_t inputSampleRate() const;
    const AudioDevice& device() const { return device_; }

private:
    void run();
    void updateTargetBuffer();
    void completeInitialization(bool succeeded, const QString& failure = QString());
    void report(const QString& message) const;

    AudioDevice device_;
    std::vector<BYTE> waveFormat_;
    std::atomic_int baseTargetBufferMilliseconds_{10};
    std::atomic_int manualDelayMilliseconds_{0};
    std::atomic_int automaticDelayMilliseconds_{0};
    std::atomic_int acousticDelayMilliseconds_{0};
    std::atomic_int targetBufferMilliseconds_{10};
    std::atomic_size_t targetBufferFrames_{0};
    std::size_t sampleRate_ = 0;
    std::size_t bytesPerFrame_ = 0;
    StatusCallback statusCallback_;
    std::unique_ptr<FrameRingBuffer> ringBuffer_;

    std::atomic_bool stopRequested_{false};
    std::atomic_bool playbackStarted_{false};
    std::atomic_int requestedVolumePercent_{100};
    std::atomic_uint64_t underrunFrames_{0};
    std::atomic_uint64_t correctedFrames_{0};
    std::atomic_int64_t streamLatencyHundredNanoseconds_{0};
    std::atomic_int64_t enginePeriodHundredNanoseconds_{0};
    std::atomic_bool lowLatencyMode_{false};
    bool preferExclusiveMode_ = true;
    std::thread thread_;

    mutable std::mutex initializationMutex_;
    std::condition_variable initializationCondition_;
    bool initializationCompleted_ = false;
    bool initializationSucceeded_ = false;
    QString failureMessage_;

    mutable std::mutex probeMutex_;
    std::condition_variable probeCondition_;
    std::vector<float> pendingProbe_;
    std::vector<float> activeProbe_;
    std::vector<BYTE> mixedInputBuffer_;
    std::size_t activeProbeOffset_ = 0;
    float pendingProbeAmplitude_ = 0.0F;
    float activeProbeAmplitude_ = 0.0F;
    std::uint64_t nextProbeGeneration_ = 1;
    std::uint64_t pendingProbeGeneration_ = 0;
    std::uint64_t activeProbeGeneration_ = 0;
    std::uint64_t lastStartedProbeGeneration_ = 0;
    std::int64_t lastProbeStartQpcHundredNanoseconds_ = 0;
};
