#include "native_bridge.h"

#include "audio_engine.h"
#include "latency_calibrator.h"
#include "remote_microphone_buffer.h"
#include "remote_microphone_output.h"
#include "system_audio_capture.h"
#include "wasapi_device_manager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
QString fromWide(const wchar_t* value)
{
    return value == nullptr ? QString() : QString::fromWCharArray(value);
}

void invokeText(VsTextCallback callback, void* context, const QString& message)
{
    if (callback == nullptr) {
        return;
    }
    const std::wstring wide = message.toStdWString();
    callback(context, wide.c_str());
}

QJsonObject deviceToJson(const AudioDevice& device)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), device.id);
    object.insert(QStringLiteral("name"), device.name);
    object.insert(QStringLiteral("isDefault"), device.isDefault);
    return object;
}

AudioDevice deviceFromJson(const QJsonObject& object)
{
    return {
        object.value(QStringLiteral("id")).toString(),
        object.value(QStringLiteral("name")).toString(),
        object.value(QStringLiteral("isDefault")).toBool(),
    };
}

QJsonObject parseObject(const wchar_t* json, QString* errorMessage)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        fromWide(json).toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("配置格式无效：%1").arg(parseError.errorString());
        }
        return {};
    }
    return document.object();
}

int copyJson(const QJsonObject& object, wchar_t* buffer, int capacity)
{
    const std::wstring json = QString::fromUtf8(
                                  QJsonDocument(object).toJson(QJsonDocument::Compact))
                                  .toStdWString();
    const int required = static_cast<int>(json.size()) + 1;
    if (buffer == nullptr || capacity < required) {
        return required;
    }
    std::copy(json.cbegin(), json.cend(), buffer);
    buffer[json.size()] = L'\0';
    return required;
}

int enumerateDevices(bool render, wchar_t* buffer, int capacity)
{
    QString error;
    const QVector<AudioDevice> devices = render
                                             ? WasapiDeviceManager::enumerateRenderDevices(&error)
                                             : WasapiDeviceManager::enumerateCaptureDevices(&error);
    QJsonArray array;
    for (const AudioDevice& device : devices) {
        array.append(deviceToJson(device));
    }
    QJsonObject result;
    result.insert(QStringLiteral("devices"), array);
    result.insert(QStringLiteral("error"), error);
    return copyJson(result, buffer, capacity);
}

struct BridgeHandle
{
    BridgeHandle(VsTextCallback status,
                 VsTextCallback error,
                 VsStateCallback running,
                 VsLevelCallback level,
                 VsCalibrationCallback calibration,
                 VsAcousticCorrectionCallback acousticCorrection,
                 void* callbackContext)
        : statusCallback(status)
        , errorCallback(error)
        , runningCallback(running)
        , levelCallback(level)
        , calibrationCallback(calibration)
        , acousticCorrectionCallback(acousticCorrection)
        , context(callbackContext)
    {
        remoteMicrophoneOutput = std::make_unique<RemoteMicrophoneOutput>(
            [this](const QString& message) {
                invokeText(statusCallback, context, message);
            });
        systemAudioCapture = std::make_unique<SystemAudioCapture>(
            [this](const QString& message) {
                invokeText(statusCallback, context, message);
            });
        QObject::connect(&engine,
                         &AudioEngine::statusChanged,
                         &engine,
                         [this](const QString& message) {
                             invokeText(statusCallback, context, message);
                         },
                         Qt::DirectConnection);
        QObject::connect(&engine,
                         &AudioEngine::errorOccurred,
                         &engine,
                         [this](const QString& message) {
                             invokeText(errorCallback, context, message);
                         },
                         Qt::DirectConnection);
        QObject::connect(&engine,
                         &AudioEngine::runningChanged,
                         &engine,
                         [this](bool running) {
                             if (runningCallback != nullptr) {
                                 runningCallback(context, running ? 1 : 0);
                             }
                         },
                         Qt::DirectConnection);
        QObject::connect(&engine,
                         &AudioEngine::programLevelChanged,
                         &engine,
                         [this](double levelDbfs, bool probeAllowed) {
                             if (levelCallback != nullptr) {
                                 levelCallback(context,
                                               levelDbfs,
                                               probeAllowed ? 1 : 0);
                             }
                         },
                         Qt::DirectConnection);
        QObject::connect(&engine,
                         &AudioEngine::acousticCorrectionChanged,
                         &engine,
                         [this](const QString& deviceId,
                                int delayMilliseconds,
                                double driftPpm,
                                const QString& probeMode,
                                double confidence) {
                             if (acousticCorrectionCallback != nullptr) {
                                 acousticCorrectionCallback(context,
                                                            deviceId.toStdWString().c_str(),
                                                            delayMilliseconds,
                                                            driftPpm,
                                                            probeMode.toStdWString().c_str(),
                                                            confidence);
                             }
                         },
                         Qt::DirectConnection);

        QObject::connect(&calibrator,
                         &LatencyCalibrator::statusChanged,
                         &calibrator,
                         [this](const QString& message) {
                             invokeText(statusCallback, context, message);
                         },
                         Qt::DirectConnection);
        QObject::connect(&calibrator,
                         &LatencyCalibrator::errorOccurred,
                         &calibrator,
                         [this](const QString& message) {
                             invokeText(errorCallback, context, message);
                         },
                         Qt::DirectConnection);
        QObject::connect(&calibrator,
                         &LatencyCalibrator::finished,
                         &calibrator,
                         [this](bool cancelled, bool failed) {
                             if (calibrationCallback == nullptr) {
                                 return;
                             }
                             QJsonArray array;
                             for (const LatencyCalibrationResult& result : calibrator.results()) {
                                 QJsonObject item;
                                 item.insert(QStringLiteral("device"), deviceToJson(result.device));
                                 item.insert(QStringLiteral("detected"), result.detected);
                                 item.insert(QStringLiteral("measuredLatencyMilliseconds"),
                                             result.measuredLatencyMilliseconds);
                                 item.insert(QStringLiteral("confidence"), result.confidence);
                                 item.insert(QStringLiteral("recommendedDelayMilliseconds"),
                                             result.recommendedDelayMilliseconds);
                                 array.append(item);
                             }
                             QJsonObject payload;
                             payload.insert(QStringLiteral("results"), array);
                             payload.insert(QStringLiteral("outcome"),
                                            cancelled
                                                ? QStringLiteral("cancelled")
                                                : failed ? QStringLiteral("failed")
                                                         : QStringLiteral("completed"));
                             const std::wstring json = QString::fromUtf8(
                                 QJsonDocument(payload).toJson(QJsonDocument::Compact))
                                                           .toStdWString();
                             calibrationCallback(context, json.c_str());
                         },
                         Qt::DirectConnection);
    }

    AudioEngine engine;
    LatencyCalibrator calibrator;
    std::shared_ptr<RemoteMicrophoneBuffer> remoteMicrophone =
        std::make_shared<RemoteMicrophoneBuffer>();
    std::unique_ptr<RemoteMicrophoneOutput> remoteMicrophoneOutput;
    std::unique_ptr<SystemAudioCapture> systemAudioCapture;
    VsTextCallback statusCallback = nullptr;
    VsTextCallback errorCallback = nullptr;
    VsStateCallback runningCallback = nullptr;
    VsLevelCallback levelCallback = nullptr;
    VsCalibrationCallback calibrationCallback = nullptr;
    VsAcousticCorrectionCallback acousticCorrectionCallback = nullptr;
    void* context = nullptr;
};

BridgeHandle* bridge(void* handle)
{
    return static_cast<BridgeHandle*>(handle);
}
}

void* __cdecl VS_Create(VsTextCallback statusCallback,
                        VsTextCallback errorCallback,
                        VsStateCallback runningCallback,
                        VsLevelCallback levelCallback,
                        VsCalibrationCallback calibrationCallback,
                        VsAcousticCorrectionCallback acousticCorrectionCallback,
                        void* context)
{
    try {
        return new BridgeHandle(statusCallback,
                                errorCallback,
                                runningCallback,
                                levelCallback,
                                calibrationCallback,
                                acousticCorrectionCallback,
                                context);
    } catch (...) {
        return nullptr;
    }
}

void __cdecl VS_Destroy(void* handle)
{
    delete bridge(handle);
}

int __cdecl VS_GetRenderDevicesJson(wchar_t* buffer, int capacity)
{
    return enumerateDevices(true, buffer, capacity);
}

int __cdecl VS_GetCaptureDevicesJson(wchar_t* buffer, int capacity)
{
    return enumerateDevices(false, buffer, capacity);
}

int __cdecl VS_Start(void* handle, const wchar_t* configurationJson)
{
    BridgeHandle* value = bridge(handle);
    if (value == nullptr) {
        return 0;
    }
    if (value->calibrator.isActive()) {
        invokeText(value->errorCallback,
                   value->context,
                   QStringLiteral("声学校准进行中，无法启动音频同步"));
        return 0;
    }

    QString error;
    const QJsonObject configuration = parseObject(configurationJson, &error);
    if (!error.isEmpty()) {
        invokeText(value->errorCallback, value->context, error);
        return 0;
    }

    const AudioDevice capture = deviceFromJson(
        configuration.value(QStringLiteral("capture")).toObject());
    const AudioDevice microphone = deviceFromJson(
        configuration.value(QStringLiteral("microphone")).toObject());
    QVector<OutputDeviceSettings> outputs;
    for (const QJsonValue& entry : configuration.value(QStringLiteral("outputs")).toArray()) {
        const QJsonObject object = entry.toObject();
        OutputDeviceSettings settings;
        settings.device = deviceFromJson(object.value(QStringLiteral("device")).toObject());
        settings.volumePercent = object.value(QStringLiteral("volumePercent")).toInt(100);
        settings.extraDelayMilliseconds = object.value(
                                                   QStringLiteral("delayMilliseconds"))
                                                   .toInt();
        outputs.push_back(settings);
    }

    return value->engine.start(
               capture,
               outputs,
               configuration.value(QStringLiteral("bufferMilliseconds")).toInt(5),
               configuration.value(QStringLiteral("exclusiveMode")).toBool(),
               configuration.value(QStringLiteral("automaticLatencyCompensation")).toBool(true),
               microphone,
               configuration.value(QStringLiteral("continuousAcousticTracking")).toBool(),
               configuration.value(QStringLiteral("useRemoteMicrophone")).toBool()
                   ? value->remoteMicrophone
                   : nullptr)
               ? 1
               : 0;
}

void __cdecl VS_Stop(void* handle)
{
    if (BridgeHandle* value = bridge(handle)) {
        value->engine.stop();
    }
}

int __cdecl VS_IsActive(void* handle)
{
    BridgeHandle* value = bridge(handle);
    return value != nullptr && value->engine.isActive() ? 1 : 0;
}

void __cdecl VS_SetOutputVolume(void* handle,
                                const wchar_t* deviceId,
                                int volumePercent)
{
    if (BridgeHandle* value = bridge(handle)) {
        value->engine.setOutputVolume(fromWide(deviceId), volumePercent);
    }
}

void __cdecl VS_SetOutputDelay(void* handle,
                               const wchar_t* deviceId,
                               int delayMilliseconds)
{
    if (BridgeHandle* value = bridge(handle)) {
        value->engine.setOutputDelay(fromWide(deviceId), delayMilliseconds);
    }
}

void __cdecl VS_SetSynchronizationMargin(void* handle, int marginMilliseconds)
{
    if (BridgeHandle* value = bridge(handle)) {
        value->engine.setSynchronizationMargin(marginMilliseconds);
    }
}

int __cdecl VS_StartCalibration(void* handle, const wchar_t* configurationJson)
{
    BridgeHandle* value = bridge(handle);
    if (value == nullptr) {
        return 0;
    }
    if (value->engine.isActive()) {
        invokeText(value->errorCallback,
                   value->context,
                   QStringLiteral("音频同步运行中，无法启动声学校准"));
        return 0;
    }

    QString error;
    const QJsonObject configuration = parseObject(configurationJson, &error);
    if (!error.isEmpty()) {
        invokeText(value->errorCallback, value->context, error);
        return 0;
    }

    const AudioDevice microphone = deviceFromJson(
        configuration.value(QStringLiteral("microphone")).toObject());
    QVector<AudioDevice> outputs;
    for (const QJsonValue& entry : configuration.value(QStringLiteral("outputs")).toArray()) {
        outputs.push_back(deviceFromJson(entry.toObject()));
    }
    return value->calibrator.start(microphone, outputs) ? 1 : 0;
}

void __cdecl VS_StopCalibration(void* handle)
{
    if (BridgeHandle* value = bridge(handle)) {
        value->calibrator.requestStop();
    }
}

int __cdecl VS_IsCalibrating(void* handle)
{
    BridgeHandle* value = bridge(handle);
    return value != nullptr && value->calibrator.isActive() ? 1 : 0;
}

void __cdecl VS_SetRemoteMicrophoneConnected(void* handle,
                                             int connected,
                                             std::uint32_t sampleRate)
{
    if (BridgeHandle* value = bridge(handle)) {
        value->remoteMicrophone->setConnected(connected != 0,
                                              connected != 0 ? sampleRate : 0);
    }
}

void __cdecl VS_AddRemoteMicrophoneClockSample(
    void* handle,
    std::uint64_t frameIndex,
    std::uint64_t monotonicNanoseconds)
{
    if (BridgeHandle* value = bridge(handle)) {
        value->remoteMicrophone->addClockSample(frameIndex,
                                                monotonicNanoseconds);
    }
}

void __cdecl VS_AppendRemoteMicrophonePcm16(
    void* handle,
    std::uint64_t firstFrameIndex,
    std::uint32_t sampleRate,
    const std::int16_t* samples,
    int sampleCount)
{
    BridgeHandle* value = bridge(handle);
    if (value == nullptr || samples == nullptr || sampleCount <= 0
        || sampleRate < 8000 || sampleRate > 192000) {
        return;
    }

    std::vector<float> normalized(static_cast<std::size_t>(sampleCount));
    std::transform(samples,
                   samples + sampleCount,
                   normalized.begin(),
                   [](std::int16_t sample) {
                       return static_cast<float>(sample) / 32768.0F;
                   });
    value->remoteMicrophone->append(firstFrameIndex,
                                    sampleRate,
                                    normalized);
}

void __cdecl VS_PushRemoteMicrophoneOutputPcm16(
    void* handle,
    std::uint32_t sampleRate,
    const std::int16_t* samples,
    int sampleCount)
{
    BridgeHandle* value = bridge(handle);
    if (value == nullptr || value->remoteMicrophoneOutput == nullptr
        || samples == nullptr || sampleCount <= 0) {
        return;
    }

    std::vector<float> normalized(static_cast<std::size_t>(sampleCount));
    std::transform(samples,
                   samples + sampleCount,
                   normalized.begin(),
                   [](std::int16_t sample) {
                       return static_cast<float>(sample) / 32768.0F;
                   });
    if (value->remoteMicrophoneOutput != nullptr) {
        value->remoteMicrophoneOutput->push(sampleRate, normalized);
    }
}

int __cdecl VS_StartRemoteMicrophoneOutput(void* handle,
                                           const wchar_t* deviceId,
                                           const wchar_t* deviceName,
                                           std::uint32_t sampleRate,
                                           int bufferMilliseconds,
                                           int volumePercent)
{
    BridgeHandle* value = bridge(handle);
    if (value == nullptr || value->remoteMicrophoneOutput == nullptr) {
        return 0;
    }
    const AudioDevice device{fromWide(deviceId), fromWide(deviceName), false};
    return value->remoteMicrophoneOutput->start(device,
                                                sampleRate,
                                                bufferMilliseconds,
                                                volumePercent)
               ? 1
               : 0;
}

void __cdecl VS_StopRemoteMicrophoneOutput(void* handle)
{
    if (BridgeHandle* value = bridge(handle);
        value != nullptr && value->remoteMicrophoneOutput != nullptr) {
        value->remoteMicrophoneOutput->stop();
    }
}

int __cdecl VS_IsRemoteMicrophoneOutputActive(void* handle)
{
    BridgeHandle* value = bridge(handle);
    return value != nullptr && value->remoteMicrophoneOutput != nullptr
                   && value->remoteMicrophoneOutput->isActive()
               ? 1
               : 0;
}

void __cdecl VS_SetRemoteMicrophoneOutputVolume(void* handle, int volumePercent)
{
    if (BridgeHandle* value = bridge(handle);
        value != nullptr && value->remoteMicrophoneOutput != nullptr) {
        value->remoteMicrophoneOutput->setVolumePercent(volumePercent);
    }
}

int __cdecl VS_StartSystemAudioCapture(
    void* handle,
    const wchar_t* deviceId,
    const wchar_t* deviceName,
    int volumePercent,
    VsSystemAudioPcmCallback pcmCallback,
    void* callbackContext)
{
    BridgeHandle* value = bridge(handle);
    if (value == nullptr || value->systemAudioCapture == nullptr || pcmCallback == nullptr) {
        return 0;
    }
    const AudioDevice device{fromWide(deviceId), fromWide(deviceName), false};
    return value->systemAudioCapture->start(
               device,
               volumePercent,
               [pcmCallback, callbackContext](std::uint64_t firstFrameIndex,
                                              std::uint32_t sampleRate,
                                              std::uint16_t channels,
                                              const std::int16_t* samples,
                                              std::size_t sampleCount) {
                   pcmCallback(callbackContext,
                               firstFrameIndex,
                               sampleRate,
                               channels,
                               samples,
                               static_cast<int>(sampleCount));
               })
               ? 1
               : 0;
}

void __cdecl VS_StopSystemAudioCapture(void* handle)
{
    if (BridgeHandle* value = bridge(handle);
        value != nullptr && value->systemAudioCapture != nullptr) {
        value->systemAudioCapture->stop();
    }
}

int __cdecl VS_IsSystemAudioCaptureActive(void* handle)
{
    BridgeHandle* value = bridge(handle);
    return value != nullptr && value->systemAudioCapture != nullptr
                   && value->systemAudioCapture->isActive()
               ? 1
               : 0;
}

void __cdecl VS_SetSystemAudioCaptureVolume(void* handle, int volumePercent)
{
    if (BridgeHandle* value = bridge(handle);
        value != nullptr && value->systemAudioCapture != nullptr) {
        value->systemAudioCapture->setVolumePercent(volumePercent);
    }
}
