#pragma once

#include "audio_device.h"

#include <QObject>
#include <QHash>
#include <QString>
#include <QVector>

#include <atomic>
#include <mutex>
#include <memory>
#include <thread>

class OutputWorker;
class RemoteMicrophoneBuffer;

class AudioEngine : public QObject
{
    Q_OBJECT

public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    bool start(const AudioDevice& captureSource,
               const QVector<OutputDeviceSettings>& outputDevices,
               int targetBufferMilliseconds,
               bool preferExclusiveOutputs,
               bool automaticLatencyCompensation,
               const AudioDevice& acousticMicrophone = {},
               bool continuousAcousticTracking = false,
               std::shared_ptr<RemoteMicrophoneBuffer> remoteMicrophone = nullptr);
    void stop();
    void setOutputVolume(const QString& deviceId, int volumePercent);
    void setOutputDelay(const QString& deviceId, int delayMilliseconds);
    void setSynchronizationMargin(int marginMilliseconds);
    bool isActive() const;

signals:
    void statusChanged(const QString& message);
    void errorOccurred(const QString& message);
    void runningChanged(bool running);
    void acousticCorrectionChanged(const QString& deviceId,
                                   int delayMilliseconds,
                                   double driftPpm,
                                   const QString& probeMode,
                                   double confidence);
    void programLevelChanged(double levelDbfs, bool probeAllowed);

private:
    void run(AudioDevice captureSource,
             QVector<OutputDeviceSettings> outputDevices,
             bool preferExclusiveOutputs,
             bool automaticLatencyCompensation,
             AudioDevice acousticMicrophone,
             bool continuousAcousticTracking,
             std::shared_ptr<RemoteMicrophoneBuffer> remoteMicrophone);

    std::atomic_bool stopRequested_{false};
    std::atomic_bool active_{false};
    std::atomic_bool running_{false};
    std::atomic_int requestedSynchronizationMarginMilliseconds_{5};
    std::thread thread_;

    std::mutex activeWorkersMutex_;
    QHash<QString, OutputWorker*> activeWorkers_;
    QHash<QString, int> requestedOutputDelays_;
    std::atomic<float> recentProgramLevel_{0.0F};
    std::atomic_int64_t lastProgramPacketMilliseconds_{0};
};
