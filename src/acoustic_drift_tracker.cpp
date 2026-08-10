#include "acoustic_drift_tracker.h"

#include "calibration_signal.h"
#include "output_worker.h"
#include "remote_microphone_buffer.h"
#include "wasapi_helpers.h"

#include <Windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <QHashFunctions>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr double maximumRemoteMeasuredLatencySeconds = 1.5;
constexpr double maximumLocalMeasuredLatencySeconds = 1.5;
constexpr double minimumAcceptedRelativeLatencySeconds = -0.020;
constexpr double minimumProbeConfidence = 0.08;

struct CoTaskMemWaveFormatDeleter
{
    void operator()(WAVEFORMATEX* value) const
    {
        CoTaskMemFree(value);
    }
};

struct CaptureFormat
{
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t bytesPerFrame = 0;
    bool floatingPoint = false;
    bool pcm = false;
};

CaptureFormat inspectCaptureFormat(const WAVEFORMATEX* format)
{
    if (format == nullptr) {
        throw std::invalid_argument("麦克风格式不能为空");
    }
    CaptureFormat info;
    info.sampleRate = format->nSamplesPerSec;
    info.channels = format->nChannels;
    info.bitsPerSample = format->wBitsPerSample;
    info.bytesPerFrame = format->nBlockAlign;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        info.floatingPoint = true;
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
        info.pcm = true;
    } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
               && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        info.floatingPoint = IsEqualGUID(extensible->SubFormat,
                                        KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        info.pcm = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    }
    const bool supportedFloat = info.floatingPoint && info.bitsPerSample == 32;
    const bool supportedPcm = info.pcm
                              && (info.bitsPerSample == 16
                                  || info.bitsPerSample == 24
                                  || info.bitsPerSample == 32);
    if (info.sampleRate == 0 || info.channels == 0 || info.bytesPerFrame == 0
        || (!supportedFloat && !supportedPcm)) {
        throw std::runtime_error("麦克风原生格式不支持连续声学分析");
    }
    return info;
}

float decodeCaptureSample(const BYTE* source, const CaptureFormat& format)
{
    if (format.floatingPoint) {
        float value = 0.0F;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }
    if (format.bitsPerSample == 16) {
        std::int16_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return static_cast<float>(value) / 32768.0F;
    }
    if (format.bitsPerSample == 24) {
        std::int32_t value = static_cast<std::int32_t>(source[0])
                             | (static_cast<std::int32_t>(source[1]) << 8)
                             | (static_cast<std::int32_t>(source[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<std::int32_t>(0xFF000000);
        }
        return static_cast<float>(value) / 8388608.0F;
    }
    std::int32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return static_cast<float>(static_cast<double>(value) / 2147483648.0);
}

double median(std::deque<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 0
               ? (values[middle - 1] + values[middle]) * 0.5
               : values[middle];
}

enum class ProbeMode
{
    unknown,
    ultrasonic,
    spreadSpectrum
};

QString modeName(ProbeMode mode)
{
    switch (mode) {
    case ProbeMode::ultrasonic:
        return QStringLiteral("超声");
    case ProbeMode::spreadSpectrum:
        return QStringLiteral("低电平扩频");
    default:
        return QStringLiteral("自检");
    }
}

struct TrackerState
{
    AcousticTrackedOutput output;
    ProbeMode mode = ProbeMode::unknown;
    float ultrasonicAmplitude = 0.0030F;
    float spreadAmplitude = 0.0012F;
    int consecutiveFailures = 0;
    std::deque<double> underlyingLatencyHistory;
    bool measuredThisCycle = false;
    double lastConfidence = 0.0;
    double smoothedDriftPpm = 0.0;
    double previousRelativeLatency = 0.0;
    bool hasPreviousRelativeLatency = false;
    bool hasInitialAlignment = false;
};

struct Measurement
{
    bool detected = false;
    double latencyMilliseconds = 0.0;
    double confidence = 0.0;
};
}

AcousticDriftTracker::AcousticDriftTracker(
    AudioDevice microphone,
    std::vector<AcousticTrackedOutput> outputs,
    ProgramLevelCallback programLevelCallback,
    StatusCallback statusCallback,
    CorrectionCallback correctionCallback,
    std::shared_ptr<RemoteMicrophoneBuffer> remoteMicrophone)
    : microphone_(std::move(microphone))
    , outputs_(std::move(outputs))
    , programLevelCallback_(std::move(programLevelCallback))
    , statusCallback_(std::move(statusCallback))
    , correctionCallback_(std::move(correctionCallback))
    , remoteMicrophone_(std::move(remoteMicrophone))
{
}

AcousticDriftTracker::~AcousticDriftTracker()
{
    stop();
}

void AcousticDriftTracker::start()
{
    stopRequested_ = false;
    thread_ = std::thread(&AcousticDriftTracker::run, this);
}

void AcousticDriftTracker::stop()
{
    stopRequested_ = true;
    waitCondition_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool AcousticDriftTracker::waitInterruptibly(std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lock(waitMutex_);
    return waitCondition_.wait_for(lock, duration, [this] {
        return stopRequested_.load();
    });
}

void AcousticDriftTracker::run()
{
    ComPtr<IAudioClient> audioClient;
    bool captureStarted = false;
    try {
        ComInitializer com(COINIT_MULTITHREADED);
        MmcssRegistration mmcss;

        ComPtr<IAudioCaptureClient> captureClient;
        UniqueHandle captureEvent;
        CaptureFormat captureFormat;
        const bool remoteMicrophoneRequested = remoteMicrophone_ != nullptr;
        if (remoteMicrophoneRequested && !remoteMicrophone_->isConnected()) {
            if (statusCallback_) {
                statusCallback_(QStringLiteral("自同步等待手机麦克风数据"));
            }
            while (!stopRequested_ && !remoteMicrophone_->isConnected()) {
                if (waitInterruptibly(std::chrono::milliseconds(100))) {
                    return;
                }
            }
        }
        const bool useRemoteMicrophone = remoteMicrophoneRequested
                                         && remoteMicrophone_->isConnected();
        if (useRemoteMicrophone) {
            captureFormat.sampleRate = remoteMicrophone_->sampleRate();
            captureFormat.channels = 1;
            captureFormat.bitsPerSample = 32;
            captureFormat.bytesPerFrame = sizeof(float);
            captureFormat.floatingPoint = true;
            if (captureFormat.sampleRate == 0) {
                throw std::runtime_error("手机麦克风尚未开始发送音频");
            }
        } else {
            ComPtr<IMMDeviceEnumerator> enumerator;
            checkHresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator)),
                         "创建连续声学校准设备枚举器");
            ComPtr<IMMDevice> endpoint;
            const std::wstring microphoneId = toWideString(microphone_.id);
            checkHresult(enumerator->GetDevice(microphoneId.c_str(), &endpoint),
                         "打开连续声学校准麦克风");
            checkHresult(endpoint->Activate(
                             __uuidof(IAudioClient),
                             CLSCTX_ALL,
                             nullptr,
                             reinterpret_cast<void**>(audioClient.GetAddressOf())),
                         "激活连续声学校准麦克风");

            WAVEFORMATEX* rawFormat = nullptr;
            checkHresult(audioClient->GetMixFormat(&rawFormat),
                         "获取连续声学校准麦克风格式");
            std::unique_ptr<WAVEFORMATEX, CoTaskMemWaveFormatDeleter> captureWaveFormat(
                rawFormat);
            captureFormat = inspectCaptureFormat(captureWaveFormat.get());
            checkHresult(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                                     | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                                 0,
                                                 0,
                                                 captureWaveFormat.get(),
                                                 nullptr),
                         "初始化连续声学校准麦克风");

            captureEvent = UniqueHandle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            if (!captureEvent) {
                checkHresult(HRESULT_FROM_WIN32(GetLastError()), "创建连续声学捕获事件");
            }
            checkHresult(audioClient->SetEventHandle(captureEvent.get()),
                         "设置连续声学捕获事件");
            checkHresult(audioClient->GetService(IID_PPV_ARGS(&captureClient)),
                         "获取连续声学捕获客户端");
            checkHresult(audioClient->Start(), "启动连续声学麦克风");
            captureStarted = true;
        }

        std::vector<TrackerState> states;
        states.reserve(outputs_.size());
        for (const AcousticTrackedOutput& output : outputs_) {
            if (output.worker != nullptr) {
                TrackerState state;
                state.output = output;
                states.push_back(std::move(state));
            }
        }
        if (states.size() < 2) {
            throw std::runtime_error("连续声学跟踪至少需要两个可用输出");
        }

        if (statusCallback_) {
            statusCallback_(useRemoteMicrophone
                                ? QStringLiteral("自适应声学跟踪已使用手机麦克风：网络延迟通过手机采样序号抵消")
                                : QStringLiteral("自适应声学跟踪已启动：优先超声，自检失败后自动切换低电平扩频"));
        }

        auto drainCapture = [&](std::vector<float>* recording,
                                std::uint64_t* firstQpc) {
            if (useRemoteMicrophone) {
                return;
            }
            UINT32 packetFrames = 0;
            checkHresult(captureClient->GetNextPacketSize(&packetFrames),
                         "读取连续声学麦克风包大小");
            while (packetFrames > 0) {
                BYTE* source = nullptr;
                UINT32 frameCount = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                checkHresult(captureClient->GetBuffer(&source,
                                                      &frameCount,
                                                      &flags,
                                                      &devicePosition,
                                                      &qpcPosition),
                             "读取连续声学麦克风数据");
                if (recording != nullptr
                    && (qpcPosition == 0
                        || (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0)) {
                    checkHresult(captureClient->ReleaseBuffer(frameCount),
                                 "释放无效时间戳的连续声学麦克风数据");
                    checkHresult(captureClient->GetNextPacketSize(&packetFrames),
                                 "读取后续连续声学麦克风包");
                    continue;
                }
                if (recording != nullptr) {
                    if (firstQpc != nullptr && *firstQpc == 0) {
                        *firstQpc = qpcPosition;
                    }
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    const std::size_t bytesPerSample = captureFormat.bitsPerSample / 8;
                    for (UINT32 frame = 0; frame < frameCount; ++frame) {
                        float mono = 0.0F;
                        if (!silent && source != nullptr) {
                            const BYTE* frameData = source
                                                    + static_cast<std::size_t>(frame)
                                                          * captureFormat.bytesPerFrame;
                            for (std::uint16_t channel = 0;
                                 channel < captureFormat.channels;
                                 ++channel) {
                                mono += decodeCaptureSample(frameData
                                                                + channel * bytesPerSample,
                                                            captureFormat);
                            }
                            mono /= captureFormat.channels;
                        }
                        recording->push_back(mono);
                    }
                }
                checkHresult(captureClient->ReleaseBuffer(frameCount),
                             "释放连续声学麦克风数据");
                checkHresult(captureClient->GetNextPacketSize(&packetFrames),
                             "读取后续连续声学麦克风包");
            }
        };

        auto measure = [&](TrackerState& state, ProbeMode mode) {
            Measurement result;
            const float programLevel = programLevelCallback_
                                           ? programLevelCallback_()
                                           : 0.0F;
            // 所有探针都必须由实时节目声掩蔽；静音或极低电平时不允许调度。
            if (programLevel < 0.006F) {
                return result;
            }
            const std::uint32_t outputRate = state.output.worker->inputSampleRate();
            const std::uint32_t seed = qHash(state.output.device.id) ^ 0x51A7E3u;
            const std::vector<float> outputProbe = mode == ProbeMode::ultrasonic
                                                       ? generateUltrasonicProbe(outputRate)
                                                       : generateSpreadSpectrumProbe(outputRate,
                                                                                     seed);
            const std::vector<float> referenceProbe = mode == ProbeMode::ultrasonic
                                                          ? generateUltrasonicProbe(
                                                                captureFormat.sampleRate)
                                                          : generateSpreadSpectrumProbe(
                                                                captureFormat.sampleRate,
                                                                seed);
            if (outputProbe.empty() || referenceProbe.empty()) {
                return result;
            }

            const float amplitude = mode == ProbeMode::ultrasonic
                                        ? std::min(state.ultrasonicAmplitude,
                                                   std::clamp(programLevel * 0.10F,
                                                              0.0012F,
                                                              0.0060F))
                                        : std::min(state.spreadAmplitude,
                                                   std::clamp(programLevel * 0.04F,
                                                              0.0006F,
                                                              0.0030F));

            const std::uint64_t remoteCaptureStart = useRemoteMicrophone
                                                         ? remoteMicrophone_
                                                               ->latestFrameIndex()
                                                         : 0;
            // 本机麦克风先丢弃旧包；手机流保留绝对采样序号，无需依赖网络到达时刻。
            if (!useRemoteMicrophone) {
                drainCapture(nullptr, nullptr);
            }
            const std::uint64_t generation = state.output.worker->scheduleAcousticProbe(
                outputProbe,
                amplitude);
            if (generation == 0) {
                return result;
            }
            std::int64_t probeStartQpc = 0;
            if (!state.output.worker->waitForAcousticProbeStart(
                    generation,
                    std::chrono::milliseconds(1500),
                    &probeStartQpc)) {
                state.output.worker->cancelAcousticProbe(generation);
                return result;
            }

            const double captureSeconds = std::clamp(
                state.output.worker->targetBufferMilliseconds() / 1000.0
                    + state.output.worker->streamLatencyMilliseconds() / 1000.0
                    + 0.55,
                0.75,
                2.8);
            if (useRemoteMicrophone) {
                const std::uint64_t frameAtProbeStart = remoteMicrophone_
                                                            ->latestFrameIndex();
                const std::uint64_t requiredFrame = frameAtProbeStart
                                                    + static_cast<std::uint64_t>(
                                                        std::ceil(captureSeconds
                                                                  * captureFormat.sampleRate));
                if (!remoteMicrophone_->waitUntilFrame(
                        requiredFrame,
                        std::chrono::milliseconds(
                            static_cast<int>(captureSeconds * 1000.0) + 2500),
                        &stopRequested_)) {
                    return result;
                }
                const RemoteAudioSnapshot snapshot = remoteMicrophone_->snapshotFrom(
                    remoteCaptureStart);
                if (snapshot.samples.empty() || snapshot.sampleRate == 0) {
                    return result;
                }
                const double availableSeconds = static_cast<double>(snapshot.samples.size())
                                                / snapshot.sampleRate;
                const ProbeDetection detection = detectKnownProbe(
                    snapshot.samples,
                    snapshot.sampleRate,
                    referenceProbe,
                    0.0,
                    0.0,
                    std::min(availableSeconds, maximumRemoteMeasuredLatencySeconds),
                    minimumProbeConfidence,
                    mode == ProbeMode::ultrasonic
                        ? std::max(18000.0,
                                   std::min(19800.0,
                                            snapshot.sampleRate * 0.5 - 2300.0))
                        : std::min(15800.0, snapshot.sampleRate * 0.34) - 2300.0,
                    mode == ProbeMode::ultrasonic
                        ? std::min(22500.0, snapshot.sampleRate * 0.5 - 100.0)
                        : std::min(15800.0, snapshot.sampleRate * 0.34) + 2300.0);
                if (!detection.detected
                    || detection.relativeDelaySeconds < minimumAcceptedRelativeLatencySeconds
                    || detection.relativeDelaySeconds > maximumRemoteMeasuredLatencySeconds) {
                    result.confidence = detection.confidence;
                    return result;
                }
                const double arrivalFrame = snapshot.firstFrameIndex
                                            + detection.arrivalSeconds
                                                  * snapshot.sampleRate;
                // 手机帧号从本次录音的 0 开始，不能直接与 Windows QPC 相减。
                // 使用探针开始后经过的手机采样帧数；网络延迟作为公共偏移保留，
                // 在比较多个输出设备时可以抵消。
                if (arrivalFrame < static_cast<double>(frameAtProbeStart)) {
                    result.confidence = detection.confidence;
                    return result;
                }
                result.detected = true;
                result.latencyMilliseconds = (arrivalFrame
                                              - static_cast<double>(frameAtProbeStart))
                                             * 1000.0 / snapshot.sampleRate;
                result.confidence = detection.confidence;
                return result;
            }

            std::vector<float> recording;
            recording.reserve(static_cast<std::size_t>(captureSeconds
                                                        * captureFormat.sampleRate));
            std::uint64_t firstCaptureQpc = 0;
            const auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::duration<double>(captureSeconds);
            while (!stopRequested_
                   && std::chrono::steady_clock::now() < deadline) {
                WaitForSingleObject(captureEvent.get(), 20);
                drainCapture(&recording, &firstCaptureQpc);
            }
            if (firstCaptureQpc == 0 || probeStartQpc <= 0) {
                return result;
            }

            const double expectedStartSeconds = static_cast<double>(
                                                    probeStartQpc
                                                    - static_cast<std::int64_t>(
                                                        firstCaptureQpc))
                                                / 10000000.0;
            const ProbeDetection detection = detectKnownProbe(
                recording,
                captureFormat.sampleRate,
                referenceProbe,
                expectedStartSeconds,
                0.08,
                std::min(captureSeconds + 0.08,
                         maximumLocalMeasuredLatencySeconds),
                minimumProbeConfidence,
                mode == ProbeMode::ultrasonic
                    ? std::max(18000.0,
                               std::min(19800.0,
                                        captureFormat.sampleRate * 0.5 - 2300.0))
                    : std::min(15800.0, captureFormat.sampleRate * 0.34) - 2300.0,
                mode == ProbeMode::ultrasonic
                    ? std::min(22500.0,
                               captureFormat.sampleRate * 0.5 - 100.0)
                    : std::min(15800.0, captureFormat.sampleRate * 0.34) + 2300.0);
            result.detected = detection.detected
                              && detection.relativeDelaySeconds
                                     >= minimumAcceptedRelativeLatencySeconds
                              && detection.relativeDelaySeconds
                                     <= maximumLocalMeasuredLatencySeconds;
            result.latencyMilliseconds = detection.relativeDelaySeconds * 1000.0;
            result.confidence = detection.confidence;
            return result;
        };

        if (waitInterruptibly(std::chrono::seconds(2))) {
            return;
        }

        auto previousCycleTime = std::chrono::steady_clock::now();
        while (!stopRequested_) {
            for (TrackerState& state : states) {
                state.measuredThisCycle = false;
                ProbeMode attemptedMode = state.mode == ProbeMode::spreadSpectrum
                                              ? ProbeMode::spreadSpectrum
                                              : ProbeMode::ultrasonic;
                Measurement measurement = measure(state, attemptedMode);
                if (!measurement.detected && state.mode == ProbeMode::unknown) {
                    attemptedMode = ProbeMode::spreadSpectrum;
                    measurement = measure(state, attemptedMode);
                }

                if (measurement.detected) {
                    state.mode = attemptedMode;
                    state.consecutiveFailures = 0;
                    state.lastConfidence = measurement.confidence;
                    const double underlyingLatency = measurement.latencyMilliseconds
                                                     - state.output.worker
                                                           ->acousticDelayMilliseconds();
                    state.underlyingLatencyHistory.push_back(underlyingLatency);
                    while (state.underlyingLatencyHistory.size() > 3) {
                        state.underlyingLatencyHistory.pop_front();
                    }
                    state.measuredThisCycle = true;
                    if (state.mode == ProbeMode::spreadSpectrum) {
                        if (measurement.confidence > 0.20) {
                            state.spreadAmplitude = std::max(0.0006F,
                                                             state.spreadAmplitude
                                                                 * 0.92F);
                        }
                    } else if (measurement.confidence > 0.20) {
                        state.ultrasonicAmplitude = std::max(0.0012F,
                                                            state.ultrasonicAmplitude
                                                                * 0.94F);
                    }
                } else {
                    ++state.consecutiveFailures;
                    state.lastConfidence = measurement.confidence;
                    if (attemptedMode == ProbeMode::ultrasonic) {
                        state.ultrasonicAmplitude = std::min(0.0060F,
                                                            state.ultrasonicAmplitude
                                                                * 1.35F);
                        if (state.consecutiveFailures >= 2) {
                            state.mode = ProbeMode::spreadSpectrum;
                            state.consecutiveFailures = 0;
                        }
                    } else {
                        state.spreadAmplitude = std::min(0.0030F,
                                                        state.spreadAmplitude * 1.25F);
                    }
                }

                if (correctionCallback_) {
                    QString displayMode;
                    if (measurement.detected) {
                        displayMode = modeName(state.mode)
                                      + (state.underlyingLatencyHistory.size() < 2
                                             ? QStringLiteral("，确认中")
                                             : QString());
                    } else if (attemptedMode == ProbeMode::spreadSpectrum
                               && programLevelCallback_
                               && programLevelCallback_() < 0.006F) {
                        displayMode = QStringLiteral("等待节目声");
                    } else {
                        displayMode = modeName(attemptedMode) + QStringLiteral("，未检出");
                    }
                    correctionCallback_(state.output.device.id,
                                        state.output.worker->acousticDelayMilliseconds(),
                                        state.smoothedDriftPpm,
                                        displayMode,
                                        state.lastConfidence);
                }
            }

            std::vector<TrackerState*> validStates;
            for (TrackerState& state : states) {
                if (state.measuredThisCycle
                    && state.underlyingLatencyHistory.size() >= 3) {
                    validStates.push_back(&state);
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const double elapsedSeconds = std::max(
                0.1,
                std::chrono::duration<double>(now - previousCycleTime).count());
            previousCycleTime = now;
            if (validStates.size() >= 2) {
                double maximumObservedLatency = -std::numeric_limits<double>::infinity();
                std::vector<double> observedLatencies;
                observedLatencies.reserve(validStates.size());
                for (TrackerState* state : validStates) {
                    const double observed = median(state->underlyingLatencyHistory)
                                            + state->output.worker
                                                  ->acousticDelayMilliseconds();
                    observedLatencies.push_back(observed);
                    maximumObservedLatency = std::max(maximumObservedLatency, observed);
                }

                std::vector<double> desiredDelays;
                desiredDelays.reserve(validStates.size());
                double minimumDesiredDelay = std::numeric_limits<double>::infinity();
                for (std::size_t index = 0; index < validStates.size(); ++index) {
                    const double desired = validStates[index]->output.worker
                                               ->acousticDelayMilliseconds()
                                           + maximumObservedLatency
                                           - observedLatencies[index];
                    desiredDelays.push_back(desired);
                    minimumDesiredDelay = std::min(minimumDesiredDelay, desired);
                }
                // 保留少量可回退余量，设备由慢变快时也能通过减少本层延迟追踪。
                if (minimumDesiredDelay < 6.0) {
                    for (double& desired : desiredDelays) {
                        desired += 6.0 - minimumDesiredDelay;
                    }
                }

                const double referenceUnderlying = median(
                    validStates.front()->underlyingLatencyHistory);
                bool establishedInitialAlignment = false;
                for (std::size_t index = 0; index < validStates.size(); ++index) {
                    TrackerState& state = *validStates[index];
                    const int currentDelay = state.output.worker
                                                 ->acousticDelayMilliseconds();
                    int newDelay = currentDelay;
                    if (!state.hasInitialAlignment) {
                        // 初始测量也必须限速，避免一次误检直接跳到数百毫秒。
                        const int desired = std::clamp(
                            static_cast<int>(std::lround(desiredDelays[index])),
                            0,
                            500);
                        const int error = desired - currentDelay;
                        constexpr int initialStepMilliseconds = 20;
                        if (std::abs(error) <= initialStepMilliseconds) {
                            newDelay = desired;
                            state.hasInitialAlignment = true;
                            establishedInitialAlignment = true;
                        } else {
                            newDelay = std::clamp(
                                currentDelay
                                    + (error > 0 ? initialStepMilliseconds
                                                 : -initialStepMilliseconds),
                                0,
                                500);
                        }
                    } else {
                        const double error = desiredDelays[index] - currentDelay;
                        int step = 0;
                        if (std::abs(error) >= 0.75) {
                            step = std::clamp(
                                static_cast<int>(std::lround(error)), -2, 2);
                        }
                        newDelay = std::clamp(currentDelay + step, 0, 500);
                    }
                    state.output.worker->setAcousticDelayMilliseconds(newDelay);

                    const double relativeUnderlying = median(
                                                          state.underlyingLatencyHistory)
                                                      - referenceUnderlying;
                    if (state.hasPreviousRelativeLatency) {
                        const double instantaneousPpm = (relativeUnderlying
                                                         - state.previousRelativeLatency)
                                                        / elapsedSeconds * 1000.0;
                        state.smoothedDriftPpm = state.smoothedDriftPpm * 0.75
                                                 + instantaneousPpm * 0.25;
                    }
                    state.previousRelativeLatency = relativeUnderlying;
                    state.hasPreviousRelativeLatency = true;
                    if (correctionCallback_) {
                        correctionCallback_(state.output.device.id,
                                            newDelay,
                                            state.smoothedDriftPpm,
                                            modeName(state.mode),
                                             state.lastConfidence);
                    }
                }
                if (establishedInitialAlignment && statusCallback_) {
                    statusCallback_(QStringLiteral("自同步已根据三轮有效探针建立初始延迟基准；后续将小步跟踪设备漂移"));
                }
            } else if (statusCallback_) {
                statusCallback_(QStringLiteral("声学跟踪等待有效探针：静音或节目电平过低时所有探针都会暂停"));
            }

            const bool waitingForInitialAlignment = std::any_of(
                states.cbegin(), states.cend(), [](const TrackerState& state) {
                    return !state.hasInitialAlignment;
                });
            const auto interval = waitingForInitialAlignment
                                      ? std::chrono::seconds(2)
                                      : std::chrono::seconds(12);
            if (waitInterruptibly(interval)) {
                break;
            }
        }

        if (captureStarted && audioClient != nullptr) {
            audioClient->Stop();
            captureStarted = false;
        }
    } catch (const std::exception& exception) {
        if (captureStarted && audioClient != nullptr) {
            audioClient->Stop();
        }
        if (!stopRequested_ && statusCallback_) {
            statusCallback_(QStringLiteral("连续声学跟踪已停用：%1")
                                .arg(QString::fromUtf8(exception.what())));
        }
    }
}
