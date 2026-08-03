#pragma once

#include "audio_device.h"

#include <QObject>
#include <QVector>

#include <atomic>
#include <mutex>
#include <thread>

struct LatencyCalibrationResult
{
    AudioDevice device;
    bool detected = false;
    double measuredLatencyMilliseconds = 0.0;
    double confidence = 0.0;
    int recommendedDelayMilliseconds = 0;
};

class LatencyCalibrator : public QObject
{
    Q_OBJECT

public:
    explicit LatencyCalibrator(QObject* parent = nullptr);
    ~LatencyCalibrator() override;

    bool start(const AudioDevice& microphone, const QVector<AudioDevice>& outputDevices);
    void stop();
    bool isActive() const;
    QVector<LatencyCalibrationResult> results() const;

signals:
    void statusChanged(const QString& message);
    void errorOccurred(const QString& message);
    void runningChanged(bool running);
    void finished();

private:
    void run(AudioDevice microphone, QVector<AudioDevice> outputDevices);

    std::atomic_bool stopRequested_{false};
    std::atomic_bool active_{false};
    std::thread thread_;

    mutable std::mutex resultsMutex_;
    QVector<LatencyCalibrationResult> results_;
};
