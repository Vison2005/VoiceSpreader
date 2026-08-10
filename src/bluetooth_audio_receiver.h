#pragma once

#include <QObject>
#include <QStringList>

#include <memory>

class QThread;

// 封装 Windows AudioPlaybackConnection，使电脑作为蓝牙 A2DP 接收端。
class BluetoothAudioReceiver : public QObject
{
    Q_OBJECT

public:
    explicit BluetoothAudioReceiver(QObject* parent = nullptr);
    ~BluetoothAudioReceiver() override;

public slots:
    void refreshDevices();
    void connectDevice(const QString& deviceId, const QString& deviceName);
    void disconnectDevice();

signals:
    void devicesChanged(const QStringList& deviceIds,
                        const QStringList& deviceNames);
    void busyChanged(bool busy);
    void connectionChanged(bool connected, const QString& deviceName);
    void availabilityChanged(bool available);
    void statusChanged(const QString& message);
    void errorOccurred(const QString& message);

private:
    struct Impl;

    void initializeOnWorker();
    void refreshDevicesOnWorker();
    void connectDeviceOnWorker(const QString& deviceId,
                               const QString& deviceName);
    void disconnectDeviceOnWorker(bool notify);
    void handleRemoteClosedOnWorker(const QString& deviceId,
                                    const QString& deviceName);
    void shutdownOnWorker();

    std::unique_ptr<Impl> impl_;
    QThread* workerThread_ = nullptr;
    QObject* workerContext_ = nullptr;
};
