#include "latency_calibrator.h"

#include "calibration_signal.h"
#include "output_worker.h"
#include "wasapi_helpers.h"

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr std::uint32_t calibrationSampleRate = 48000;
constexpr std::size_t calibrationChannels = 2;
constexpr std::size_t chunkFrames = 480;
constexpr double preambleSeconds = 0.50;
constexpr double sweepDurationSeconds = 0.80;
constexpr double sweepStartFrequencyHz = 100.0;
constexpr double sweepEndFrequencyHz = 18000.0;
constexpr double deviceSlotSeconds = 2.00;
constexpr double postambleSeconds = 1.50;
constexpr double captureTailSeconds = 0.20;
constexpr int calibrationRepetitions = 2;
constexpr float sweepAmplitude = 0.18F;
constexpr double maximumMeasuredLatencySeconds = 1.20;
constexpr double maximumRepeatSpreadMilliseconds = 2.0;

QString detectionStatusText(ProbeDetectionStatus status)
{
    switch (status) {
    case ProbeDetectionStatus::Detected:
        return QStringLiteral("通过");
    case ProbeDetectionStatus::LowConfidence:
        return QStringLiteral("峰值置信度不足");
    case ProbeDetectionStatus::NoUsableEnergy:
        return QStringLiteral("反卷积后无有效能量");
    case ProbeDetectionStatus::RecordingTooShort:
        return QStringLiteral("录音长度不足");
    case ProbeDetectionStatus::InvalidInput:
        return QStringLiteral("检测输入无效");
    }
    return QStringLiteral("未知状态");
}

double recordingPeakDbfs(const std::vector<float>& recording,
                         std::uint32_t sampleRate,
                         double firstSeconds,
                         double lastSeconds)
{
    const std::size_t firstFrame = std::min<std::size_t>(
        recording.size(),
        static_cast<std::size_t>(std::max(0.0, firstSeconds) * sampleRate));
    const std::size_t lastFrame = std::min<std::size_t>(
        recording.size(),
        static_cast<std::size_t>(std::max(0.0, lastSeconds) * sampleRate));
    float peak = 0.0F;
    for (std::size_t frame = firstFrame; frame < lastFrame; ++frame) {
        peak = std::max(peak, std::abs(recording[frame]));
    }
    return peak > std::numeric_limits<float>::epsilon()
               ? 20.0 * std::log10(static_cast<double>(peak))
               : -120.0;
}

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
        throw std::runtime_error("麦克风的原生格式不支持内置分析");
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

std::vector<BYTE> calibrationWaveFormat()
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(calibrationChannels);
    format.nSamplesPerSec = calibrationSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(float));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    std::vector<BYTE> bytes(sizeof(format));
    std::memcpy(bytes.data(), &format, sizeof(format));
    return bytes;
}
}

LatencyCalibrator::LatencyCalibrator(QObject* parent)
    : QObject(parent)
{
}

LatencyCalibrator::~LatencyCalibrator()
{
    stop();
}

bool LatencyCalibrator::start(const AudioDevice& microphone,
                              const QVector<AudioDevice>& outputDevices)
{
    if (active_.exchange(true)) {
        return false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        results_.clear();
    }
    stopRequested_ = false;
    emit runningChanged(true);
    thread_ = std::thread(&LatencyCalibrator::run, this, microphone, outputDevices);
    return true;
}

void LatencyCalibrator::stop()
{
    requestStop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void LatencyCalibrator::requestStop()
{
    stopRequested_ = true;
}

bool LatencyCalibrator::isActive() const
{
    return active_.load();
}

QVector<LatencyCalibrationResult> LatencyCalibrator::results() const
{
    std::lock_guard<std::mutex> lock(resultsMutex_);
    return results_;
}

void LatencyCalibrator::run(AudioDevice microphone, QVector<AudioDevice> outputDevices)
{
    std::vector<std::unique_ptr<OutputWorker>> workers;
    ComPtr<IAudioClient> captureAudioClient;
    bool captureStarted = false;
    bool failed = false;

    try {
        ComInitializer com(COINIT_MULTITHREADED);
        MmcssRegistration mmcss;

        emit statusChanged(QStringLiteral("声学校准：正在打开麦克风 %1").arg(microphone.name));

        ComPtr<IMMDeviceEnumerator> enumerator;
        checkHresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator)),
                     "创建校准设备枚举器");

        ComPtr<IMMDevice> microphoneEndpoint;
        const std::wstring microphoneId = toWideString(microphone.id);
        checkHresult(enumerator->GetDevice(microphoneId.c_str(), &microphoneEndpoint),
                     "打开校准麦克风");
        checkHresult(microphoneEndpoint->Activate(
                         __uuidof(IAudioClient),
                         CLSCTX_ALL,
                         nullptr,
                         reinterpret_cast<void**>(captureAudioClient.GetAddressOf())),
                     "激活麦克风 IAudioClient");

        WAVEFORMATEX* rawCaptureFormat = nullptr;
        checkHresult(captureAudioClient->GetMixFormat(&rawCaptureFormat),
                     "获取麦克风原生格式");
        const std::vector<BYTE> captureFormatBytes = copyWaveFormat(rawCaptureFormat);
        CoTaskMemFree(rawCaptureFormat);
        const auto* captureWaveFormat = reinterpret_cast<const WAVEFORMATEX*>(
            captureFormatBytes.data());
        const CaptureFormat captureFormat = inspectCaptureFormat(captureWaveFormat);

        const DWORD captureFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                   | AUDCLNT_STREAMFLAGS_NOPERSIST;
        checkHresult(captureAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                    captureFlags,
                                                    0,
                                                    0,
                                                    captureWaveFormat,
                                                    nullptr),
                     "初始化校准麦克风");

        UniqueHandle captureEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!captureEvent) {
            checkHresult(HRESULT_FROM_WIN32(GetLastError()), "创建麦克风捕获事件");
        }
        checkHresult(captureAudioClient->SetEventHandle(captureEvent.get()),
                     "设置麦克风捕获事件");

        ComPtr<IAudioCaptureClient> captureClient;
        checkHresult(captureAudioClient->GetService(IID_PPV_ARGS(&captureClient)),
                     "获取麦克风 IAudioCaptureClient");

        const std::vector<BYTE> probeFormat = calibrationWaveFormat();
        workers.reserve(static_cast<std::size_t>(outputDevices.size()));
        QVector<AudioDevice> activeDevices;
        for (const AudioDevice& device : outputDevices) {
            auto worker = std::make_unique<OutputWorker>(
                device,
                probeFormat,
                20,
                100,
                false,
                [this](const QString& message) {
                    emit statusChanged(message);
                },
                false);
            worker->start();
            if (worker->waitUntilInitialized(std::chrono::seconds(5))) {
                activeDevices.push_back(device);
                workers.push_back(std::move(worker));
            } else {
                const QString failure = worker->failureMessage().isEmpty()
                                            ? QStringLiteral("初始化超时")
                                            : worker->failureMessage();
                emit statusChanged(QStringLiteral("校准跳过 %1：%2")
                                       .arg(device.name, failure));
                worker->stop();
            }
        }

        if (activeDevices.size() < 2) {
            throw std::runtime_error("至少需要两个输出设备成功启动才能进行相对校准");
        }

        const double captureSweepEndFrequency = std::min(
            sweepEndFrequencyHz,
            static_cast<double>(captureFormat.sampleRate) * 0.45);
        const std::vector<float> outputSweep = generateExponentialSineSweep(
            calibrationSampleRate,
            sweepDurationSeconds,
            sweepStartFrequencyHz,
            captureSweepEndFrequency);
        const std::vector<float> captureReferenceSweep = generateExponentialSineSweep(
            captureFormat.sampleRate,
            sweepDurationSeconds,
            sweepStartFrequencyHz,
            captureSweepEndFrequency);
        if (outputSweep.empty() || captureReferenceSweep.empty()) {
            throw std::runtime_error("无法为当前输出或麦克风采样率生成 ESS 扫频");
        }

        // 先正序、再逆序逐设备测量。每台设备两次测量的平均发送时刻相同，
        // 可以抵消播放与录音异步时钟造成的一阶线性时间漂移。
        std::vector<std::size_t> sweepDeviceOrder;
        sweepDeviceOrder.reserve(static_cast<std::size_t>(activeDevices.size())
                                 * calibrationRepetitions);
        for (std::size_t index = 0; index < workers.size(); ++index) {
            sweepDeviceOrder.push_back(index);
        }
        for (std::size_t index = workers.size(); index > 0; --index) {
            sweepDeviceOrder.push_back(index - 1);
        }
        const std::size_t preambleFrames = static_cast<std::size_t>(
            std::llround(preambleSeconds * calibrationSampleRate));
        const std::size_t slotFrames = static_cast<std::size_t>(
            std::llround(deviceSlotSeconds * calibrationSampleRate));
        const std::size_t postambleFrames = static_cast<std::size_t>(
            std::llround(postambleSeconds * calibrationSampleRate));
        const std::size_t slotCount = sweepDeviceOrder.size();
        const std::size_t totalTimelineFrames = preambleFrames
                                                + slotFrames * slotCount
                                                + postambleFrames;
        const double timelineSeconds = static_cast<double>(totalTimelineFrames)
                                       / calibrationSampleRate;

        std::vector<std::vector<float>> outputChunks(
            workers.size(),
            std::vector<float>(chunkFrames * calibrationChannels, 0.0F));
        std::vector<float> microphoneRecording;
        microphoneRecording.reserve(static_cast<std::size_t>(
            (timelineSeconds + captureTailSeconds) * captureFormat.sampleRate));

        auto feedChunk = [&](std::size_t timelineFrame) {
            const std::size_t framesThisChunk = std::min(chunkFrames,
                                                         totalTimelineFrames
                                                             - timelineFrame);
            for (std::size_t deviceIndex = 0; deviceIndex < workers.size(); ++deviceIndex) {
                auto& chunk = outputChunks[deviceIndex];
                std::fill(chunk.begin(), chunk.end(), 0.0F);
                for (std::size_t slot = 0; slot < sweepDeviceOrder.size(); ++slot) {
                    if (sweepDeviceOrder[slot] != deviceIndex) {
                        continue;
                    }
                    const std::size_t probeStart = preambleFrames
                                                   + slot * slotFrames;
                    for (std::size_t frame = 0; frame < framesThisChunk; ++frame) {
                        const std::size_t absoluteFrame = timelineFrame + frame;
                        if (absoluteFrame < probeStart
                            || absoluteFrame >= probeStart
                                                   + outputSweep.size()) {
                            continue;
                        }
                        const float value = outputSweep[absoluteFrame - probeStart]
                                            * sweepAmplitude;
                        chunk[frame * calibrationChannels] += value;
                        chunk[frame * calibrationChannels + 1] += value;
                    }
                }
                workers[deviceIndex]->push(
                    reinterpret_cast<const BYTE*>(chunk.data()),
                    framesThisChunk,
                    false);
            }
        };

        auto drainMicrophone = [&]() {
            UINT32 packetFrames = 0;
            checkHresult(captureClient->GetNextPacketSize(&packetFrames),
                         "读取麦克风包大小");
            while (packetFrames > 0) {
                BYTE* source = nullptr;
                UINT32 frameCount = 0;
                DWORD flags = 0;
                checkHresult(captureClient->GetBuffer(&source,
                                                      &frameCount,
                                                      &flags,
                                                      nullptr,
                                                      nullptr),
                             "读取麦克风数据");
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const std::size_t bytesPerSample = captureFormat.bitsPerSample / 8;
                for (UINT32 frame = 0; frame < frameCount; ++frame) {
                    float mono = 0.0F;
                    if (!silent) {
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
                    microphoneRecording.push_back(mono);
                }
                checkHresult(captureClient->ReleaseBuffer(frameCount),
                             "释放麦克风数据");
                checkHresult(captureClient->GetNextPacketSize(&packetFrames),
                             "读取后续麦克风包大小");
            }
        };

        checkHresult(captureAudioClient->Start(), "启动麦克风捕获");
        captureStarted = true;
        emit statusChanged(
            QStringLiteral("ESS 声学校准开始：%1 台设备将依次正反序扫频，各测量 %2 次")
                .arg(activeDevices.size())
                .arg(calibrationRepetitions));

        const std::size_t leadFrames = calibrationSampleRate / 10;
        std::size_t fedFrames = 0;
        while (fedFrames < std::min(leadFrames, totalTimelineFrames)) {
            feedChunk(fedFrames);
            fedFrames += std::min(chunkFrames, totalTimelineFrames - fedFrames);
        }

        const auto startTime = std::chrono::steady_clock::now();
        while (!stopRequested_) {
            const double elapsedSeconds = std::chrono::duration<double>(
                                              std::chrono::steady_clock::now() - startTime)
                                              .count();
            const std::size_t desiredFrames = std::min<std::size_t>(
                totalTimelineFrames,
                leadFrames
                    + static_cast<std::size_t>(elapsedSeconds * calibrationSampleRate));
            while (fedFrames < desiredFrames) {
                feedChunk(fedFrames);
                fedFrames += std::min(chunkFrames, totalTimelineFrames - fedFrames);
            }

            WaitForSingleObject(captureEvent.get(), 5);
            drainMicrophone();

            if (elapsedSeconds >= timelineSeconds + captureTailSeconds) {
                break;
            }
        }

        captureAudioClient->Stop();
        captureStarted = false;
        drainMicrophone();

        for (auto& worker : workers) {
            worker->stop();
        }

        if (stopRequested_) {
            emit statusChanged(QStringLiteral("声学校准已取消"));
        } else {
            QVector<LatencyCalibrationResult> measuredResults;
            measuredResults.reserve(activeDevices.size());
            double maximumLatencyMilliseconds = -std::numeric_limits<double>::infinity();
            int detectedCount = 0;

            for (int index = 0; index < activeDevices.size(); ++index) {
                if (stopRequested_) {
                    throw std::runtime_error("calibration cancelled");
                }
                std::vector<SweepImpulseResponseAnalysis> analyses;
                analyses.reserve(calibrationRepetitions);
                int measurementNumber = 0;

                for (std::size_t slot = 0; slot < sweepDeviceOrder.size(); ++slot) {
                    if (stopRequested_) {
                        throw std::runtime_error("calibration cancelled");
                    }
                    if (sweepDeviceOrder[slot] != static_cast<std::size_t>(index)) {
                        continue;
                    }
                    ++measurementNumber;
                    const double expectedStartSeconds = preambleSeconds
                                                        + slot * deviceSlotSeconds;
                    SweepImpulseResponseAnalysis analysis =
                        analyzeExponentialSweepImpulseResponse(
                        microphoneRecording,
                        captureFormat.sampleRate,
                        captureReferenceSweep,
                        expectedStartSeconds,
                        0.05,
                        maximumMeasuredLatencySeconds);
                    const auto strongestPeak = std::max_element(
                        analysis.peaks.cbegin(),
                        analysis.peaks.cend(),
                        [](const ImpulseResponsePeak& left,
                           const ImpulseResponsePeak& right) {
                            return left.relativeToStrongest < right.relativeToStrongest;
                        });
                    const double peakDbfs = recordingPeakDbfs(
                        microphoneRecording,
                        captureFormat.sampleRate,
                        expectedStartSeconds,
                        expectedStartSeconds + sweepDurationSeconds
                            + maximumMeasuredLatencySeconds);
                    emit statusChanged(
                        QStringLiteral(
                            "ESS 诊断：%1，第 %2 轮，录音峰值 %3 dBFS，候选 %4 个，主峰 IR %5 ms、SNR %6 dB，%7")
                            .arg(activeDevices.at(index).name)
                            .arg(measurementNumber)
                            .arg(peakDbfs, 0, 'f', 1)
                            .arg(analysis.peaks.size())
                            .arg(strongestPeak != analysis.peaks.cend()
                                     ? strongestPeak->delaySeconds * 1000.0
                                     : 0.0,
                                 0,
                                 'f',
                                 2)
                            .arg(strongestPeak != analysis.peaks.cend()
                                     ? strongestPeak->signalToNoiseDb
                                     : 0.0,
                                 0,
                                 'f',
                                 1)
                            .arg(detectionStatusText(analysis.status)));
                    analyses.push_back(std::move(analysis));
                }

                LatencyCalibrationResult result;
                result.device = activeDevices.at(index);
                ProbeDetection consistentDetection;
                if (analyses.size() == calibrationRepetitions) {
                    consistentDetection = selectConsistentImpulseResponsePeakPair(
                        analyses[0],
                        analyses[1],
                        maximumRepeatSpreadMilliseconds / 1000.0);
                }
                result.detected = consistentDetection.detected;
                result.measuredLatencyMilliseconds =
                    consistentDetection.relativeDelaySeconds * 1000.0;
                result.confidence = consistentDetection.confidence;
                if (result.detected) {
                    ++detectedCount;
                    maximumLatencyMilliseconds = std::max(maximumLatencyMilliseconds,
                                                          result.measuredLatencyMilliseconds);
                }
                emit statusChanged(
                    QStringLiteral("ESS 一致峰匹配：%1，%2，时延 %3 ms，两轮差 %4 ms")
                        .arg(result.device.name)
                        .arg(result.detected ? QStringLiteral("通过")
                                             : QStringLiteral("未找到一致峰对"))
                        .arg(result.measuredLatencyMilliseconds, 0, 'f', 2)
                        .arg(result.detected
                                 ? consistentDetection.consistencySpreadSeconds * 1000.0
                                 : -1.0,
                             0,
                             'f',
                             2));
                measuredResults.push_back(result);
            }

            if (detectedCount < 2) {
                throw std::runtime_error(
                    "至少两台设备未找到跨轮一致的脉冲峰；请查看 ESS 诊断中的录音峰值、候选数和主峰位置");
            }

            for (LatencyCalibrationResult& result : measuredResults) {
                if (stopRequested_) {
                    throw std::runtime_error("calibration cancelled");
                }
                if (!result.detected) {
                    continue;
                }
                result.recommendedDelayMilliseconds = std::clamp(
                    static_cast<int>(std::lround(maximumLatencyMilliseconds
                                                 - result.measuredLatencyMilliseconds)),
                    0,
                    2000);
                emit statusChanged(
                    QStringLiteral("校准结果：%1，相对测量 %2 ms，置信度 %3%，建议补偿 %4 ms")
                        .arg(result.device.name)
                        .arg(result.measuredLatencyMilliseconds, 0, 'f', 1)
                        .arg(result.confidence * 100.0, 0, 'f', 0)
                        .arg(result.recommendedDelayMilliseconds));
            }

            {
                std::lock_guard<std::mutex> lock(resultsMutex_);
                results_ = measuredResults;
            }
        }
    } catch (const std::exception& exception) {
        if (captureStarted && captureAudioClient != nullptr) {
            captureAudioClient->Stop();
        }
        for (auto& worker : workers) {
            worker->stop();
        }
        if (!stopRequested_) {
            failed = true;
            emit errorOccurred(QString::fromUtf8(exception.what()));
        }
    }

    active_ = false;
    emit runningChanged(false);
    emit finished(stopRequested_.load(), failed);
}
