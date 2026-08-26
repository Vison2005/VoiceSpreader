#include "native_bridge.h"

#include <QJsonDocument>
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
    const std::int16_t samples[]{0, 16384, -16384, 32767, -32768};
    VS_SetRemoteMicrophoneConnected(handle, 1, 48000);
    VS_AppendRemoteMicrophonePcm16(handle, 123456, 48000, samples, 5);
    VS_AddRemoteMicrophoneClockSample(handle, 123456, 1'000'000'000ULL);
    VS_SetRemoteMicrophoneConnected(handle, 0, 0);

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
