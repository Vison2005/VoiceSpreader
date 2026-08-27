#include "native_bridge.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <vector>

namespace
{
struct CalibrationCallbackState
{
    std::mutex mutex;
    std::condition_variable condition;
    bool received = false;
    QString outcome;
};

void __cdecl calibrationCompleted(void* context, const wchar_t* resultsJson)
{
    auto* state = static_cast<CalibrationCallbackState*>(context);
    const QJsonDocument document = QJsonDocument::fromJson(
        QString::fromWCharArray(resultsJson).toUtf8());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->received = true;
        state->outcome = document.object().value(QStringLiteral("outcome")).toString();
    }
    state->condition.notify_one();
}

void __cdecl systemAudioPcm(void*,
                            std::uint64_t,
                            std::uint32_t,
                            std::uint16_t,
                            const std::int16_t*,
                            int)
{
}

bool verifyEnumeration(int(__cdecl* enumerate)(wchar_t*, int), const char* label)
{
    const int required = enumerate(nullptr, 0);
    if (required <= 1) {
        std::cerr << label << " did not report a valid JSON buffer size\n";
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required));
    if (enumerate(buffer.data(), static_cast<int>(buffer.size())) != required) {
        std::cerr << label << " changed its required buffer size\n";
        return false;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QString::fromWCharArray(buffer.data()).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        std::cerr << label << " returned invalid JSON\n";
        return false;
    }

    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("devices")).isArray()
        || !object.value(QStringLiteral("error")).isString()) {
        std::cerr << label << " returned an incomplete response\n";
        return false;
    }
    return true;
}

QJsonObject findVirtualCableRenderEndpoint()
{
    const int required = VS_GetRenderDevicesJson(nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required));
    VS_GetRenderDevicesJson(buffer.data(), static_cast<int>(buffer.size()));
    const QJsonArray devices = QJsonDocument::fromJson(
                                   QString::fromWCharArray(buffer.data()).toUtf8())
                                   .object()
                                   .value(QStringLiteral("devices"))
                                   .toArray();
    for (const QJsonValue& value : devices) {
        const QJsonObject device = value.toObject();
        const QString name = device.value(QStringLiteral("name")).toString();
        if (name.contains(QStringLiteral("CABLE Input"), Qt::CaseInsensitive)
            || name.contains(QStringLiteral("VB-Audio Virtual Cable"), Qt::CaseInsensitive)) {
            return device;
        }
    }
    return {};
}

QJsonObject firstRenderEndpoint()
{
    const int required = VS_GetRenderDevicesJson(nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required));
    VS_GetRenderDevicesJson(buffer.data(), static_cast<int>(buffer.size()));
    const QJsonArray devices = QJsonDocument::fromJson(
                                   QString::fromWCharArray(buffer.data()).toUtf8())
                                   .object()
                                   .value(QStringLiteral("devices"))
                                   .toArray();
    return devices.isEmpty() ? QJsonObject() : devices.first().toObject();
}
}

int main()
{
    CalibrationCallbackState calibrationState;
    void* handle = VS_Create(nullptr,
                             nullptr,
                             nullptr,
                             nullptr,
                             &calibrationCompleted,
                             nullptr,
                             &calibrationState);
    if (handle == nullptr) {
        std::cerr << "VS_Create failed\n";
        return 1;
    }

    const bool renderValid = verifyEnumeration(&VS_GetRenderDevicesJson, "render enumeration");
    const bool captureValid = verifyEnumeration(&VS_GetCaptureDevicesJson, "capture enumeration");

    const QJsonObject systemAudioEndpoint = firstRenderEndpoint();
    if (!systemAudioEndpoint.isEmpty()) {
        const std::wstring deviceId = systemAudioEndpoint.value(QStringLiteral("id"))
                                          .toString()
                                          .toStdWString();
        const std::wstring deviceName = systemAudioEndpoint.value(QStringLiteral("name"))
                                            .toString()
                                            .toStdWString();
        if (VS_StartSystemAudioCapture(handle,
                                       deviceId.c_str(),
                                       deviceName.c_str(),
                                       100,
                                       &systemAudioPcm,
                                       nullptr)
            == 0
            || VS_IsSystemAudioCaptureActive(handle) == 0) {
            std::cerr << "system audio loopback capture could not be opened\n";
            VS_Destroy(handle);
            return 1;
        }
        VS_SetSystemAudioCaptureVolume(handle, 80);
        VS_StopSystemAudioCapture(handle);
        if (VS_IsSystemAudioCaptureActive(handle) != 0) {
            std::cerr << "system audio loopback capture remained active after stop\n";
            VS_Destroy(handle);
            return 1;
        }
        std::cout << "system audio loopback integration test passed\n";
    }

    const std::int16_t samples[]{0, 16384, -16384, 32767, -32768};
    VS_SetRemoteMicrophoneConnected(handle, 1, 48000);
    VS_AppendRemoteMicrophonePcm16(handle, 123456, 48000, samples, 5);
    VS_AddRemoteMicrophoneClockSample(handle, 123456, 1'000'000'000ULL);
    VS_SetRemoteMicrophoneConnected(handle, 0, 0);

    const QJsonObject virtualCable = findVirtualCableRenderEndpoint();
    if (!virtualCable.isEmpty()) {
        const std::wstring deviceId = virtualCable.value(QStringLiteral("id"))
                                          .toString()
                                          .toStdWString();
        const std::wstring deviceName = virtualCable.value(QStringLiteral("name"))
                                            .toString()
                                            .toStdWString();
        if (VS_StartRemoteMicrophoneOutput(handle,
                                           deviceId.c_str(),
                                           deviceName.c_str(),
                                           48000,
                                           20,
                                           100)
            == 0) {
            std::cerr << "virtual cable render endpoint could not be opened\n";
            VS_Destroy(handle);
            return 1;
        }
        if (VS_IsRemoteMicrophoneOutputActive(handle) == 0) {
            std::cerr << "virtual cable route did not become active\n";
            VS_Destroy(handle);
            return 1;
        }
        VS_SetRemoteMicrophoneConnected(handle, 1, 48000);
        VS_AppendRemoteMicrophonePcm16(handle, 0, 48000, samples, 5);
        VS_SetRemoteMicrophoneOutputVolume(handle, 80);
        VS_StopRemoteMicrophoneOutput(handle);
        VS_SetRemoteMicrophoneConnected(handle, 0, 0);
        if (VS_IsRemoteMicrophoneOutputActive(handle) != 0) {
            std::cerr << "virtual cable route remained active after stop\n";
            VS_Destroy(handle);
            return 1;
        }
        std::cout << "VB-CABLE render route integration test passed\n";
    } else {
        std::cout << "VB-CABLE render endpoint not installed; integration test skipped\n";
    }

    const wchar_t* calibrationConfiguration = LR"({
        "microphone":{"id":"invalid-microphone","name":"Test microphone","isDefault":false},
        "outputs":[
            {"id":"invalid-output-1","name":"Test output 1","isDefault":false},
            {"id":"invalid-output-2","name":"Test output 2","isDefault":false}
        ]
    })";
    if (VS_StartCalibration(handle, calibrationConfiguration) == 0) {
        std::cerr << "calibration did not start\n";
        VS_Destroy(handle);
        return 1;
    }
    const auto stopStarted = std::chrono::steady_clock::now();
    VS_StopCalibration(handle);
    const auto stopDuration = std::chrono::steady_clock::now() - stopStarted;
    if (stopDuration > std::chrono::milliseconds(100)) {
        std::cerr << "calibration stop request blocked the caller\n";
        VS_Destroy(handle);
        return 1;
    }

    bool terminalOutcomeValid = false;
    {
        std::unique_lock<std::mutex> lock(calibrationState.mutex);
        if (!calibrationState.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                return calibrationState.received;
            })) {
            std::cerr << "calibration did not report a terminal outcome\n";
        } else if (calibrationState.outcome != QStringLiteral("cancelled")) {
            std::cerr << "calibration cancellation reported the wrong outcome\n";
        } else {
            terminalOutcomeValid = true;
        }
    }
    if (!terminalOutcomeValid) {
        VS_Destroy(handle);
        return 1;
    }
    if (VS_IsCalibrating(handle) != 0) {
        std::cerr << "calibration remained active after cancellation\n";
        VS_Destroy(handle);
        return 1;
    }

    VS_Destroy(handle);
    return renderValid && captureValid ? 0 : 1;
}
