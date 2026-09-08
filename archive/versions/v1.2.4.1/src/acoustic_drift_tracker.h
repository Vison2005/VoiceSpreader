#pragma once

#include "audio_device.h"

#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class OutputWorker;
class RemoteMicrophoneBuffer;

struct AcousticTrackedOutput
{
    AudioDevice device;
    OutputWorker* worker = nullptr;
};

class AcousticDriftTracker
{
public:
    using ProgramLevelCallback = std::function<float()>;
    using StatusCallback = std::function<void(const QString&)>;
    using CorrectionCallback = std::function<void(const QString&,
                                                   int,
                                                   double,
                                                   const QString&,
                                                   double)>;

    AcousticDriftTracker(AudioDevice microphone,
                         std::vector<AcousticTrackedOutput> outputs,
                         ProgramLevelCallback programLevelCallback,
                         StatusCallback statusCallback,
                         CorrectionCallback correctionCallback,
                         std::shared_ptr<RemoteMicrophoneBuffer> remoteMicrophone = nullptr);
    ~AcousticDriftTracker();

    AcousticDriftTracker(const AcousticDriftTracker&) = delete;
    AcousticDriftTracker& operator=(const AcousticDriftTracker&) = delete;

    void start();
    void stop();

private:
    void run();
    bool waitInterruptibly(std::chrono::milliseconds duration);

    AudioDevice microphone_;
    std::vector<AcousticTrackedOutput> outputs_;
    ProgramLevelCallback programLevelCallback_;
    StatusCallback statusCallback_;
    CorrectionCallback correctionCallback_;
    std::shared_ptr<RemoteMicrophoneBuffer> remoteMicrophone_;
    std::atomic_bool stopRequested_{false};
    std::thread thread_;
    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
};
