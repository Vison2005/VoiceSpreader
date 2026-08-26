#include "audio_engine.h"

#include "acoustic_drift_tracker.h"
#include "output_worker.h"
#include "remote_microphone_buffer.h"
#include "wasapi_helpers.h"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
struct CoTaskMemWaveFormatDeleter
{
    void operator()(WAVEFORMATEX* value) const
    {
        CoTaskMemFree(value);
    }
};

std::int64_t steadyMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr float programGateRms = 0.006F;
constexpr std::int64_t programGateHoldMilliseconds = 350;

float packetRms(const BYTE* data, UINT32 frameCount, const WAVEFORMATEX* format)
{
    if (data == nullptr || frameCount == 0 || format == nullptr
        || format->nChannels == 0 || format->nBlockAlign == 0) {
        return 0.0F;
    }
    bool floatingPoint = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcm = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        floatingPoint = IsEqualGUID(extensible->SubFormat,
                                    KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        pcm = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    }
    const std::size_t bytesPerSample = format->wBitsPerSample / 8;
    if ((!floatingPoint && !pcm) || bytesPerSample == 0) {
        return 0.0F;
    }

    double energy = 0.0;
    std::uint64_t sampleCount = 0;
    for (UINT32 frame = 0; frame < frameCount; frame += 2) {
        const BYTE* frameData = data
                                + static_cast<std::size_t>(frame) * format->nBlockAlign;
        for (WORD channel = 0; channel < format->nChannels; ++channel) {
            const BYTE* sample = frameData + channel * bytesPerSample;
            float value = 0.0F;
            if (floatingPoint && format->wBitsPerSample == 32) {
                std::memcpy(&value, sample, sizeof(value));
            } else if (pcm && format->wBitsPerSample == 16) {
                std::int16_t encoded = 0;
                std::memcpy(&encoded, sample, sizeof(encoded));
                value = static_cast<float>(encoded) / 32768.0F;
            } else if (pcm && format->wBitsPerSample == 24) {
                std::int32_t encoded = static_cast<std::int32_t>(sample[0])
                                       | (static_cast<std::int32_t>(sample[1]) << 8)
                                       | (static_cast<std::int32_t>(sample[2]) << 16);
                if ((encoded & 0x00800000) != 0) {
                    encoded |= static_cast<std::int32_t>(0xFF000000);
                }
                value = static_cast<float>(encoded) / 8388608.0F;
            } else if (pcm && format->wBitsPerSample == 32) {
                std::int32_t encoded = 0;
                std::memcpy(&encoded, sample, sizeof(encoded));
                value = static_cast<float>(static_cast<double>(encoded) / 2147483648.0);
            }
            energy += static_cast<double>(value) * value;
            ++sampleCount;
        }
    }
    return sampleCount == 0
               ? 0.0F
               : static_cast<float>(std::sqrt(energy / sampleCount));
}
}

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
{
}

AudioEngine::~AudioEngine()
{
    stop();
}

bool AudioEngine::start(const AudioDevice& captureSource,
                        const QVector<OutputDeviceSettings>& outputDevices,
                        int targetBufferMilliseconds,
                        bool preferExclusiveOutputs,
                        bool automaticLatencyCompensation,
                        const AudioDevice& acousticMicrophone,
                        bool continuousAcousticTracking,
                        std::shared_ptr<RemoteMicrophoneBuffer> remoteMicrophone)
{
    if (active_.exchange(true)) {
        return false;
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    requestedSynchronizationMarginMilliseconds_ = std::clamp(
        targetBufferMilliseconds,
        2,
        100);

    {
        std::lock_guard<std::mutex> lock(activeWorkersMutex_);
        activeWorkers_.clear();
        requestedOutputDelays_.clear();
        for (const OutputDeviceSettings& output : outputDevices) {
            requestedOutputDelays_.insert(
                output.device.id,
                std::clamp(output.extraDelayMilliseconds, 0, 2000));
        }
    }

    stopRequested_ = false;
    recentProgramLevel_ = 0.0F;
    lastProgramPacketMilliseconds_ = 0;
    thread_ = std::thread(&AudioEngine::run,
                          this,
                          captureSource,
                          outputDevices,
                          preferExclusiveOutputs,
                          automaticLatencyCompensation,
                          acousticMicrophone,
                          continuousAcousticTracking,
                          std::move(remoteMicrophone));
    return true;
}

void AudioEngine::setOutputVolume(const QString& deviceId, int volumePercent)
{
    std::lock_guard<std::mutex> lock(activeWorkersMutex_);
    const auto iterator = activeWorkers_.constFind(deviceId);
    if (iterator != activeWorkers_.constEnd() && iterator.value() != nullptr) {
        iterator.value()->setVolumePercent(volumePercent);
    }
}

void AudioEngine::setOutputDelay(const QString& deviceId, int delayMilliseconds)
{
    const int limitedDelay = std::clamp(delayMilliseconds, 0, 2000);
    std::lock_guard<std::mutex> lock(activeWorkersMutex_);
    requestedOutputDelays_.insert(deviceId, limitedDelay);
    const auto iterator = activeWorkers_.constFind(deviceId);
    if (iterator != activeWorkers_.constEnd() && iterator.value() != nullptr) {
        iterator.value()->setManualDelayMilliseconds(limitedDelay);
    }
}

void AudioEngine::setSynchronizationMargin(int marginMilliseconds)
{
    const int limitedMargin = std::clamp(marginMilliseconds, 2, 100);
    requestedSynchronizationMarginMilliseconds_ = limitedMargin;

    std::lock_guard<std::mutex> lock(activeWorkersMutex_);
    for (OutputWorker* worker : activeWorkers_) {
        if (worker != nullptr) {
            worker->setSynchronizationMarginMilliseconds(limitedMargin);
        }
    }
}

void AudioEngine::stop()
{
    stopRequested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool AudioEngine::isActive() const
{
    return active_.load();
}

void AudioEngine::run(AudioDevice captureSource,
                      QVector<OutputDeviceSettings> outputDevices,
                      bool preferExclusiveOutputs,
                      bool automaticLatencyCompensation,
                      AudioDevice acousticMicrophone,
                      bool continuousAcousticTracking,
                      std::shared_ptr<RemoteMicrophoneBuffer> remoteMicrophone)
{
    std::vector<std::unique_ptr<OutputWorker>> outputWorkers;
    std::unique_ptr<AcousticDriftTracker> acousticTracker;
    ComPtr<IAudioClient> captureAudioClient;

    try {
        ComInitializer com(COINIT_MULTITHREADED);
        MmcssRegistration mmcss;

        emit statusChanged(QStringLiteral("正在打开系统声音捕获源：%1").arg(captureSource.name));

        ComPtr<IMMDeviceEnumerator> enumerator;
        checkHresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator)),
                     "创建捕获设备枚举器");

        ComPtr<IMMDevice> captureEndpoint;
        const std::wstring captureId = toWideString(captureSource.id);
        checkHresult(enumerator->GetDevice(captureId.c_str(), &captureEndpoint), "打开捕获源设备");
        checkHresult(captureEndpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                              reinterpret_cast<void**>(captureAudioClient.GetAddressOf())),
                     "激活捕获 IAudioClient");

        WAVEFORMATEX* rawFormat = nullptr;
        checkHresult(captureAudioClient->GetMixFormat(&rawFormat), "获取捕获源混音格式");
        std::unique_ptr<WAVEFORMATEX, CoTaskMemWaveFormatDeleter> mixFormat(rawFormat);
        const std::vector<BYTE> formatCopy = copyWaveFormat(mixFormat.get());

        const DWORD captureFlags = AUDCLNT_STREAMFLAGS_LOOPBACK
                                   | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        checkHresult(captureAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                    captureFlags,
                                                    0,
                                                    0,
                                                    mixFormat.get(),
                                                    nullptr),
                     "初始化 WASAPI Loopback");

        UniqueHandle captureEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!captureEvent) {
            checkHresult(HRESULT_FROM_WIN32(GetLastError()), "创建捕获事件");
        }
        checkHresult(captureAudioClient->SetEventHandle(captureEvent.get()), "设置捕获事件");

        ComPtr<IAudioCaptureClient> captureClient;
        checkHresult(captureAudioClient->GetService(IID_PPV_ARGS(&captureClient)),
                     "获取 IAudioCaptureClient");

        outputWorkers.reserve(static_cast<std::size_t>(outputDevices.size()));
        for (const OutputDeviceSettings& output : outputDevices) {
            auto worker = std::make_unique<OutputWorker>(
                output.device,
                formatCopy,
                requestedSynchronizationMarginMilliseconds_.load(),
                output.volumePercent,
                preferExclusiveOutputs,
                [this](const QString& message) {
                    emit statusChanged(message);
                },
                true);
            {
                std::lock_guard<std::mutex> lock(activeWorkersMutex_);
                worker->setManualDelayMilliseconds(
                    requestedOutputDelays_.value(output.device.id,
                                                 output.extraDelayMilliseconds));
            }
            worker->start();
            outputWorkers.push_back(std::move(worker));
        }

        std::size_t activeOutputCount = 0;
        for (int index = 0; index < static_cast<int>(outputWorkers.size()); ++index) {
            auto& worker = outputWorkers[static_cast<std::size_t>(index)];
            if (worker->waitUntilInitialized(std::chrono::seconds(5))) {
                ++activeOutputCount;
                {
                    std::lock_guard<std::mutex> lock(activeWorkersMutex_);
                    const QString& deviceId = outputDevices.at(index).device.id;
                    activeWorkers_.insert(deviceId, worker.get());
                    worker->setSynchronizationMarginMilliseconds(
                        requestedSynchronizationMarginMilliseconds_.load());
                    worker->setManualDelayMilliseconds(
                        requestedOutputDelays_.value(
                            deviceId,
                            outputDevices.at(index).extraDelayMilliseconds));
                }
            } else {
                const QString failure = worker->failureMessage().isEmpty()
                                            ? QStringLiteral("初始化超时")
                                            : worker->failureMessage();
                emit statusChanged(QStringLiteral("跳过一个不可用输出：%1").arg(failure));
            }
        }

        if (activeOutputCount == 0) {
            throw std::runtime_error("没有输出设备成功启动");
        }

        // 声学自同步会直接测量扬声器到麦克风的总路径；同时叠加静态
        // GetStreamLatency 补偿会把同一段差异重复加入，先让两种控制器互斥。
        if (automaticLatencyCompensation && !continuousAcousticTracking) {
            double maximumEffectiveLatencyMilliseconds = 0.0;
            QHash<QString, double> effectiveLatencies;
            QHash<QString, int> manualDelays;
            {
                std::lock_guard<std::mutex> lock(activeWorkersMutex_);
                manualDelays = requestedOutputDelays_;
            }
            for (const auto& worker : outputWorkers) {
                if (worker->initializedSuccessfully()) {
                    const QString deviceId = worker->device().id;
                    const double effectiveLatency = worker->streamLatencyMilliseconds()
                                                    + manualDelays.value(deviceId);
                    effectiveLatencies.insert(deviceId, effectiveLatency);
                    maximumEffectiveLatencyMilliseconds = std::max(
                        maximumEffectiveLatencyMilliseconds,
                        effectiveLatency);
                }
            }

            for (int index = 0; index < static_cast<int>(outputWorkers.size()); ++index) {
                const auto& worker = outputWorkers[static_cast<std::size_t>(index)];
                if (!worker->initializedSuccessfully()) {
                    continue;
                }
                const double effectiveLatency = effectiveLatencies.value(
                    worker->device().id,
                    worker->streamLatencyMilliseconds());
                const int compensationMilliseconds = static_cast<int>(std::lround(
                    std::max(0.0,
                             maximumEffectiveLatencyMilliseconds - effectiveLatency)));
                worker->setAutomaticDelayMilliseconds(compensationMilliseconds);
                if (compensationMilliseconds > 0) {
                    emit statusChanged(QStringLiteral("自动补偿：%1 增加 %2 ms，使输出设备彼此对齐")
                                           .arg(outputDevices.at(index).device.name)
                                           .arg(compensationMilliseconds));
                }
            }
        } else if (automaticLatencyCompensation && continuousAcousticTracking) {
            emit statusChanged(QStringLiteral(
                "已启用声学自同步，跳过静态流延迟自动补偿，避免重复叠加"));
        }

        checkHresult(captureAudioClient->Start(), "启动 WASAPI Loopback");

        const bool remoteMicrophoneRequested = remoteMicrophone != nullptr;
        if (continuousAcousticTracking
            && (remoteMicrophoneRequested || !acousticMicrophone.id.isEmpty())
            && activeOutputCount >= 2) {
            std::vector<AcousticTrackedOutput> trackedOutputs;
            for (int index = 0; index < static_cast<int>(outputWorkers.size()); ++index) {
                OutputWorker* worker = outputWorkers[static_cast<std::size_t>(index)].get();
                if (worker->initializedSuccessfully()) {
                    trackedOutputs.push_back({outputDevices.at(index).device, worker});
                }
            }
            acousticTracker = std::make_unique<AcousticDriftTracker>(
                acousticMicrophone,
                std::move(trackedOutputs),
                [this] {
                    if (steadyMilliseconds() - lastProgramPacketMilliseconds_.load()
                            > programGateHoldMilliseconds
                        || recentProgramLevel_.load() < programGateRms) {
                        return 0.0F;
                    }
                    return recentProgramLevel_.load();
                },
                [this](const QString& message) {
                    emit statusChanged(message);
                },
                [this](const QString& deviceId,
                       int delayMilliseconds,
                       double driftPpm,
                       const QString& probeMode,
                       double confidence) {
                    emit acousticCorrectionChanged(deviceId,
                                                   delayMilliseconds,
                                                   driftPpm,
                                                   probeMode,
                                                   confidence);
                },
                 remoteMicrophoneRequested ? remoteMicrophone : nullptr);
            acousticTracker->start();
        } else if (continuousAcousticTracking) {
            emit statusChanged(QStringLiteral("连续声学跟踪未启动：需要麦克风和至少两个可用输出"));
        }

        running_ = true;
        emit runningChanged(true);
        emit statusChanged(QStringLiteral("正在分发系统声音，共 %1 个输出设备，格式 %2 Hz / %3 声道")
                               .arg(activeOutputCount)
                               .arg(mixFormat->nSamplesPerSec)
                               .arg(mixFormat->nChannels));

        std::int64_t lastLevelBroadcastMilliseconds = 0;
        auto broadcastProgramLevel = [&](std::int64_t now) {
            if (now - lastLevelBroadcastMilliseconds < 120) {
                return;
            }
            const bool allowed = now - lastProgramPacketMilliseconds_.load()
                                     <= programGateHoldMilliseconds
                                 && recentProgramLevel_.load() >= programGateRms;
            const float reportedLevel = allowed ? recentProgramLevel_.load() : 0.0F;
            const double dbfs = reportedLevel > 0.000001F
                                    ? 20.0 * std::log10(reportedLevel)
                                    : -120.0;
            emit programLevelChanged(dbfs, allowed);
            lastLevelBroadcastMilliseconds = now;
        };

        while (!stopRequested_) {
            const DWORD waitResult = WaitForSingleObject(captureEvent.get(), 100);
            if (waitResult == WAIT_TIMEOUT) {
                broadcastProgramLevel(steadyMilliseconds());
                continue;
            }
            if (waitResult != WAIT_OBJECT_0) {
                checkHresult(HRESULT_FROM_WIN32(GetLastError()), "等待捕获事件");
            }

            UINT32 packetFrames = 0;
            checkHresult(captureClient->GetNextPacketSize(&packetFrames), "读取捕获包大小");

            while (packetFrames > 0) {
                BYTE* sourceData = nullptr;
                UINT32 frameCount = 0;
                DWORD flags = 0;
                checkHresult(captureClient->GetBuffer(&sourceData,
                                                      &frameCount,
                                                      &flags,
                                                      nullptr,
                                                      nullptr),
                             "获取捕获数据");

                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const float rms = silent
                                      ? 0.0F
                                      : packetRms(sourceData, frameCount, mixFormat.get());
                const float previousLevel = recentProgramLevel_.load();
                const float smoothing = rms >= previousLevel ? 0.55F : 0.15F;
                recentProgramLevel_ = previousLevel
                                      + (rms - previousLevel) * smoothing;
                const std::int64_t now = steadyMilliseconds();
                const bool programActive = !silent && rms >= programGateRms;
                if (programActive) {
                    lastProgramPacketMilliseconds_ = now;
                }
                for (auto& worker : outputWorkers) {
                    if (worker->initializedSuccessfully()) {
                        worker->push(sourceData,
                                     frameCount,
                                     silent,
                                     programActive);
                    }
                }
                broadcastProgramLevel(now);

                checkHresult(captureClient->ReleaseBuffer(frameCount), "释放捕获数据");
                checkHresult(captureClient->GetNextPacketSize(&packetFrames), "读取后续捕获包大小");
            }
        }

        if (acousticTracker) {
            acousticTracker->stop();
        }
        captureAudioClient->Stop();

        for (int index = 0; index < static_cast<int>(outputWorkers.size()); ++index) {
            const auto& worker = outputWorkers[static_cast<std::size_t>(index)];
            if (!worker->initializedSuccessfully()) {
                continue;
            }
            emit statusChanged(QStringLiteral("同步统计：%1，时钟修正 %2 帧，欠载 %3 帧，输入溢出 %4 帧，缓存恢复 %5 次")
                                   .arg(outputDevices.at(index).device.name)
                                   .arg(worker->correctedFrames())
                                   .arg(worker->underrunFrames())
                                   .arg(worker->inputOverflowFrames())
                                   .arg(worker->bufferRecoveryCount()));
        }
    } catch (const std::exception& exception) {
        if (acousticTracker) {
            acousticTracker->stop();
        }
        emit errorOccurred(QString::fromUtf8(exception.what()));
    }

    {
        std::lock_guard<std::mutex> lock(activeWorkersMutex_);
        activeWorkers_.clear();
        requestedOutputDelays_.clear();
    }

    for (auto& worker : outputWorkers) {
        worker->stop();
    }

    const bool wasRunning = running_.exchange(false);
    active_ = false;
    if (wasRunning) {
        emit runningChanged(false);
        emit statusChanged(QStringLiteral("音频分发已停止"));
    } else {
        emit runningChanged(false);
    }
}
