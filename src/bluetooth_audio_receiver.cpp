#include "bluetooth_audio_receiver.h"

#include <QMetaObject>
#include <QThread>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/base.h>

namespace
{
using AudioPlaybackConnection =
    winrt::Windows::Media::Audio::AudioPlaybackConnection;
using AudioPlaybackConnectionOpenResultStatus =
    winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus;
using AudioPlaybackConnectionState =
    winrt::Windows::Media::Audio::AudioPlaybackConnectionState;
using DeviceInformation =
    winrt::Windows::Devices::Enumeration::DeviceInformation;
using DeviceInformationKind =
    winrt::Windows::Devices::Enumeration::DeviceInformationKind;

constexpr wchar_t parentAepIdProperty[] =
    L"System.Devices.AepService.AepId";
constexpr wchar_t parentAepContainerIdProperty[] =
    L"System.Devices.AepService.ContainerId";
constexpr wchar_t deviceContainerIdProperty[] =
    L"System.Devices.ContainerId";

QString fromHString(const winrt::hstring& value)
{
    return QString::fromWCharArray(value.c_str(),
                                   static_cast<int>(value.size()));
}

QString formatHResult(const winrt::hresult_error& error)
{
    const QString description = fromHString(error.message()).trimmed();
    return description.isEmpty()
               ? QStringLiteral("Windows 错误 0x%1")
                     .arg(static_cast<quint32>(error.code().value),
                          8,
                          16,
                          QLatin1Char('0'))
                     .toUpper()
               : description;
}

QString openFailureMessage(AudioPlaybackConnectionOpenResultStatus status)
{
    switch (status) {
    case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
        return QStringLiteral(
            "连接超时。请在手机的蓝牙音频输出列表中选择这台电脑后重试。");
    case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
        return QStringLiteral(
            "Windows 拒绝了蓝牙音频连接，请检查蓝牙权限和设备配对状态。");
    case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
        return QStringLiteral("Windows 无法打开该手机的蓝牙音频连接。");
    case AudioPlaybackConnectionOpenResultStatus::Success:
        break;
    }
    return QStringLiteral("Windows 无法打开该手机的蓝牙音频连接。");
}

QString resolveDeviceName(const DeviceInformation& service)
{
    QString name = fromHString(service.Name()).trimmed();
    try {
        const auto properties = service.Properties();
        const auto noExtraProperties =
            winrt::single_threaded_vector<winrt::hstring>();
        const winrt::hstring deviceContainerPropertyName(
            deviceContainerIdProperty);
        if (properties.HasKey(deviceContainerPropertyName)) {
            const winrt::guid containerId =
                winrt::unbox_value_or<winrt::guid>(
                    properties.Lookup(deviceContainerPropertyName), {});
            if (containerId != winrt::guid{}) {
                const DeviceInformation container =
                    DeviceInformation::CreateFromIdAsync(
                        winrt::to_hstring(containerId),
                        noExtraProperties,
                        DeviceInformationKind::DeviceContainer)
                        .get();
                if (container) {
                    const QString containerName =
                        fromHString(container.Name()).trimmed();
                    if (!containerName.isEmpty()) {
                        return containerName;
                    }
                }
            }
        }

        const winrt::hstring containerPropertyName(
            parentAepContainerIdProperty);
        if (properties.HasKey(containerPropertyName)) {
            const winrt::guid containerId =
                winrt::unbox_value_or<winrt::guid>(
                    properties.Lookup(containerPropertyName), {});
            if (containerId != winrt::guid{}) {
                const DeviceInformation container =
                    DeviceInformation::CreateFromIdAsync(
                        winrt::to_hstring(containerId),
                        noExtraProperties,
                        DeviceInformationKind::AssociationEndpointContainer)
                        .get();
                if (container) {
                    const QString containerName =
                        fromHString(container.Name()).trimmed();
                    if (!containerName.isEmpty()) {
                        return containerName;
                    }
                }
            }
        }

        const winrt::hstring propertyName(parentAepIdProperty);
        if (!properties.HasKey(propertyName)) {
            return name;
        }
        const winrt::hstring parentId =
            winrt::unbox_value_or<winrt::hstring>(
                properties.Lookup(propertyName), {});
        if (parentId.empty()) {
            return name;
        }

        const DeviceInformation parent =
            DeviceInformation::CreateFromIdAsync(
                parentId,
                noExtraProperties,
                DeviceInformationKind::AssociationEndpoint)
                .get();
        if (parent) {
            const QString parentName = fromHString(parent.Name()).trimmed();
            if (!parentName.isEmpty()) {
                name = parentName;
            }
        }
    } catch (const winrt::hresult_error&) {
        // 某些蓝牙驱动不公开父 AEP；此时保留服务自身的显示名。
    }
    return name;
}
}

struct BluetoothAudioReceiver::Impl
{
    bool apartmentInitialized = false;
    AudioPlaybackConnection connection{nullptr};
    winrt::event_token stateChangedToken{};
    bool hasStateChangedToken = false;
    QString connectedDeviceId;
    QString connectedDeviceName;
};

BluetoothAudioReceiver::BluetoothAudioReceiver(QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>())
    , workerThread_(new QThread(this))
    , workerContext_(new QObject())
{
    workerThread_->setObjectName(QStringLiteral("BluetoothAudioReceiver"));
    workerContext_->moveToThread(workerThread_);
    connect(workerThread_, &QThread::finished,
            workerContext_, &QObject::deleteLater);
    workerThread_->start();
}

BluetoothAudioReceiver::~BluetoothAudioReceiver()
{
    if (workerThread_ != nullptr && workerThread_->isRunning()) {
        QMetaObject::invokeMethod(
            workerContext_,
            [this] { shutdownOnWorker(); },
            Qt::BlockingQueuedConnection);
        workerThread_->quit();
        workerThread_->wait();
    }
}

void BluetoothAudioReceiver::refreshDevices()
{
    if (workerContext_ == nullptr || !workerThread_->isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        workerContext_,
        [this] { refreshDevicesOnWorker(); },
        Qt::QueuedConnection);
}

void BluetoothAudioReceiver::connectDevice(const QString& deviceId,
                                           const QString& deviceName)
{
    if (deviceId.isEmpty() || workerContext_ == nullptr
        || !workerThread_->isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        workerContext_,
        [this, deviceId, deviceName] {
            connectDeviceOnWorker(deviceId, deviceName);
        },
        Qt::QueuedConnection);
}

void BluetoothAudioReceiver::disconnectDevice()
{
    if (workerContext_ == nullptr || !workerThread_->isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        workerContext_,
        [this] { disconnectDeviceOnWorker(true); },
        Qt::QueuedConnection);
}

void BluetoothAudioReceiver::initializeOnWorker()
{
    if (impl_->apartmentInitialized) {
        return;
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    impl_->apartmentInitialized = true;
}

void BluetoothAudioReceiver::refreshDevicesOnWorker()
{
    emit busyChanged(true);
    try {
        initializeOnWorker();
        const winrt::hstring selector = AudioPlaybackConnection::GetDeviceSelector();
        const auto requestedProperties =
            winrt::single_threaded_vector<winrt::hstring>();
        requestedProperties.Append(winrt::hstring(parentAepIdProperty));
        requestedProperties.Append(
            winrt::hstring(parentAepContainerIdProperty));
        requestedProperties.Append(
            winrt::hstring(deviceContainerIdProperty));
        const auto devices =
            DeviceInformation::FindAllAsync(selector, requestedProperties).get();

        QStringList deviceIds;
        QStringList deviceNames;
        deviceIds.reserve(static_cast<int>(devices.Size()));
        deviceNames.reserve(static_cast<int>(devices.Size()));
        for (const DeviceInformation& device : devices) {
            deviceIds.push_back(fromHString(device.Id()));
            QString name = resolveDeviceName(device);
            if (name.isEmpty()) {
                name = QStringLiteral("未命名蓝牙设备");
            }
            deviceNames.push_back(name);
        }

        emit availabilityChanged(true);
        emit devicesChanged(deviceIds, deviceNames);
        emit statusChanged(
            deviceIds.isEmpty()
                ? QStringLiteral("未找到可接收音频的已配对手机。")
                : QStringLiteral("发现 %1 台可用蓝牙音频设备。")
                      .arg(deviceIds.size()));
    } catch (const winrt::hresult_error& error) {
        emit availabilityChanged(false);
        emit devicesChanged({}, {});
        emit errorOccurred(
            QStringLiteral("A2DP 接收功能不可用：%1")
                .arg(formatHResult(error)));
    }
    emit busyChanged(false);
}

void BluetoothAudioReceiver::connectDeviceOnWorker(
    const QString& deviceId,
    const QString& deviceName)
{
    emit busyChanged(true);
    try {
        initializeOnWorker();
        disconnectDeviceOnWorker(false);

        AudioPlaybackConnection connection =
            AudioPlaybackConnection::TryCreateFromId(
                winrt::hstring(deviceId.toStdWString()));
        if (!connection) {
            emit errorOccurred(
                QStringLiteral("设备不再提供 A2DP 音频连接，请重新配对后再试。"));
            emit busyChanged(false);
            return;
        }

        impl_->connectedDeviceId = deviceId;
        impl_->connectedDeviceName = deviceName;
        const QString callbackDeviceId = deviceId;
        const QString callbackDeviceName = deviceName;
        impl_->stateChangedToken = connection.StateChanged(
            [this, callbackDeviceId, callbackDeviceName](
                const AudioPlaybackConnection& sender,
                const winrt::Windows::Foundation::IInspectable&) {
                if (sender.State() == AudioPlaybackConnectionState::Opened) {
                    emit connectionChanged(true, callbackDeviceName);
                    return;
                }
                QMetaObject::invokeMethod(
                    workerContext_,
                    [this, callbackDeviceId, callbackDeviceName] {
                        handleRemoteClosedOnWorker(callbackDeviceId,
                                                   callbackDeviceName);
                    },
                    Qt::QueuedConnection);
            });
        impl_->hasStateChangedToken = true;
        impl_->connection = connection;

        connection.StartAsync().get();
        const auto result = connection.OpenAsync().get();
        if (result.Status()
            != AudioPlaybackConnectionOpenResultStatus::Success) {
            const QString message = openFailureMessage(result.Status());
            disconnectDeviceOnWorker(false);
            emit errorOccurred(message);
            emit busyChanged(false);
            return;
        }

        emit connectionChanged(true, deviceName);
        emit statusChanged(
            QStringLiteral("已将 %1 的声音接收到 Windows；音频将从当前系统输出设备播放。")
                .arg(deviceName));
    } catch (const winrt::hresult_error& error) {
        disconnectDeviceOnWorker(false);
        emit errorOccurred(
            QStringLiteral("连接蓝牙音频失败：%1")
                .arg(formatHResult(error)));
    }
    emit busyChanged(false);
}

void BluetoothAudioReceiver::disconnectDeviceOnWorker(bool notify)
{
    const QString previousName = impl_->connectedDeviceName;
    if (impl_->connection) {
        if (impl_->hasStateChangedToken) {
            impl_->connection.StateChanged(impl_->stateChangedToken);
            impl_->hasStateChangedToken = false;
        }
        try {
            impl_->connection.Close();
        } catch (const winrt::hresult_error&) {
            // 设备可能已经离线；释放最后一个引用即可停用接收器。
        }
        impl_->connection = nullptr;
    }
    impl_->connectedDeviceId.clear();
    impl_->connectedDeviceName.clear();

    if (notify) {
        emit connectionChanged(false, {});
        emit statusChanged(
            previousName.isEmpty()
                ? QStringLiteral("蓝牙音频接收已关闭。")
                : QStringLiteral("已断开 %1 的蓝牙音频。")
                      .arg(previousName));
    }
}

void BluetoothAudioReceiver::handleRemoteClosedOnWorker(
    const QString& deviceId,
    const QString& deviceName)
{
    if (impl_->connectedDeviceId != deviceId) {
        return;
    }
    disconnectDeviceOnWorker(false);
    emit connectionChanged(false, {});
    emit statusChanged(
        QStringLiteral("%1 已断开蓝牙音频连接。").arg(deviceName));
}

void BluetoothAudioReceiver::shutdownOnWorker()
{
    disconnectDeviceOnWorker(false);
    if (impl_->apartmentInitialized) {
        winrt::uninit_apartment();
        impl_->apartmentInitialized = false;
    }
}
