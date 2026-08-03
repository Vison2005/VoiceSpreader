#include "output_worker.h"

#include "wasapi_helpers.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
struct AudioFormatInfo
{
    UINT32 sampleRate = 0;
    WORD channels = 0;
    WORD bitsPerSample = 0;
    WORD bytesPerFrame = 0;
    bool floatingPoint = false;
    bool pcm = false;
};

AudioFormatInfo inspectFormat(const WAVEFORMATEX* format)
{
    if (format == nullptr) {
        throw std::invalid_argument("音频格式不能为空");
    }

    AudioFormatInfo info;
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
        throw std::runtime_error("当前音频格式不支持内置重采样");
    }
    return info;
}

float decodeSample(const BYTE* source, const AudioFormatInfo& format)
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

void encodeSample(float value, BYTE* target, const AudioFormatInfo& format)
{
    const float limited = std::clamp(value, -1.0F, 1.0F);
    if (format.floatingPoint) {
        std::memcpy(target, &limited, sizeof(limited));
        return;
    }

    if (format.bitsPerSample == 16) {
        const auto encoded = static_cast<std::int16_t>(
            std::lround(limited * static_cast<float>(std::numeric_limits<std::int16_t>::max())));
        std::memcpy(target, &encoded, sizeof(encoded));
        return;
    }
    if (format.bitsPerSample == 24) {
        const auto encoded = static_cast<std::int32_t>(std::lround(limited * 8388607.0F));
        target[0] = static_cast<BYTE>(encoded & 0xFF);
        target[1] = static_cast<BYTE>((encoded >> 8) & 0xFF);
        target[2] = static_cast<BYTE>((encoded >> 16) & 0xFF);
        return;
    }

    const auto encoded = static_cast<std::int32_t>(
        std::llround(static_cast<double>(limited)
                     * static_cast<double>(std::numeric_limits<std::int32_t>::max())));
    std::memcpy(target, &encoded, sizeof(encoded));
}

float mappedSample(const std::vector<float>& source,
                   std::size_t frame,
                   WORD outputChannel,
                   const AudioFormatInfo& inputFormat,
                   const AudioFormatInfo& outputFormat)
{
    if (outputFormat.channels == 1 && inputFormat.channels > 1) {
        float sum = 0.0F;
        for (WORD channel = 0; channel < inputFormat.channels; ++channel) {
            sum += source[frame * inputFormat.channels + channel];
        }
        return sum / static_cast<float>(inputFormat.channels);
    }
    if (inputFormat.channels == 1) {
        return source[frame * inputFormat.channels];
    }
    if (outputChannel < inputFormat.channels) {
        return source[frame * inputFormat.channels + outputChannel];
    }
    return 0.0F;
}

std::int64_t queryPerformanceTimeHundredNanoseconds()
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency)
        || frequency.QuadPart <= 0) {
        return 0;
    }
    const std::int64_t wholeSeconds = counter.QuadPart / frequency.QuadPart;
    const std::int64_t remainder = counter.QuadPart % frequency.QuadPart;
    return wholeSeconds * 10000000LL
           + remainder * 10000000LL / frequency.QuadPart;
}
}

OutputWorker::OutputWorker(AudioDevice device,
                           std::vector<BYTE> waveFormat,
                           int targetBufferMilliseconds,
                           int volumePercent,
                           bool preferExclusiveMode,
                           StatusCallback statusCallback)
    : device_(std::move(device))
    , waveFormat_(std::move(waveFormat))
    , baseTargetBufferMilliseconds_(std::max(0, targetBufferMilliseconds))
    , targetBufferMilliseconds_(std::max(0, targetBufferMilliseconds))
    , requestedVolumePercent_(std::clamp(volumePercent, 0, 100))
    , preferExclusiveMode_(preferExclusiveMode)
    , statusCallback_(std::move(statusCallback))
{
    if (waveFormat_.size() < sizeof(WAVEFORMATEX)) {
        throw std::invalid_argument("输出音频格式无效");
    }

    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(waveFormat_.data());
    const AudioFormatInfo inputFormat = inspectFormat(format);
    sampleRate_ = inputFormat.sampleRate;
    bytesPerFrame_ = inputFormat.bytesPerFrame;
    targetBufferFrames_ = sampleRate_
                          * static_cast<std::size_t>(baseTargetBufferMilliseconds_.load()) / 1000;

    const std::size_t capacityFrames = std::max<std::size_t>(
        sampleRate_ * 3,
        targetBufferFrames_.load() * 4);
    ringBuffer_ = std::make_unique<FrameRingBuffer>(capacityFrames, bytesPerFrame_);
}

OutputWorker::~OutputWorker()
{
    stop();
}

void OutputWorker::start()
{
    stopRequested_ = false;
    thread_ = std::thread(&OutputWorker::run, this);
}

bool OutputWorker::waitUntilInitialized(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(initializationMutex_);
    initializationCondition_.wait_for(lock, timeout, [this] {
        return initializationCompleted_;
    });
    return initializationCompleted_ && initializationSucceeded_;
}

void OutputWorker::stop()
{
    stopRequested_ = true;
    probeCondition_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void OutputWorker::push(const BYTE* data,
                        std::size_t frameCount,
                        bool silent,
                        bool allowAcousticProbe)
{
    if (!initializedSuccessfully() || frameCount == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(probeMutex_);
    if (!allowAcousticProbe && !activeProbe_.empty()) {
        // 节目声一旦跌破门限便立即截断，禁止探针单独暴露在静音中。
        activeProbe_.clear();
        activeProbeOffset_ = 0;
        activeProbeGeneration_ = 0;
    }
    if (allowAcousticProbe && activeProbe_.empty() && !pendingProbe_.empty()) {
        activeProbe_ = std::move(pendingProbe_);
        activeProbeOffset_ = 0;
        activeProbeAmplitude_ = pendingProbeAmplitude_;
        activeProbeGeneration_ = pendingProbeGeneration_;
        pendingProbeGeneration_ = 0;
        lastStartedProbeGeneration_ = activeProbeGeneration_;
        lastProbeStartQpcHundredNanoseconds_ = queryPerformanceTimeHundredNanoseconds();
        probeCondition_.notify_all();
    }

    if (activeProbe_.empty()) {
        ringBuffer_->write(data, frameCount, silent);
        return;
    }

    mixedInputBuffer_.resize(frameCount * bytesPerFrame_);
    if (silent || data == nullptr) {
        std::fill(mixedInputBuffer_.begin(), mixedInputBuffer_.end(), BYTE{0});
    } else {
        std::memcpy(mixedInputBuffer_.data(), data, mixedInputBuffer_.size());
    }

    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(waveFormat_.data());
    const AudioFormatInfo inputFormat = inspectFormat(format);
    const std::size_t bytesPerSample = inputFormat.bitsPerSample / 8;
    const std::size_t mixedFrames = std::min(frameCount,
                                             activeProbe_.size() - activeProbeOffset_);
    for (std::size_t frame = 0; frame < mixedFrames; ++frame) {
        const float probeValue = activeProbe_[activeProbeOffset_ + frame]
                                 * activeProbeAmplitude_;
        BYTE* frameData = mixedInputBuffer_.data() + frame * inputFormat.bytesPerFrame;
        for (WORD channel = 0; channel < inputFormat.channels; ++channel) {
            BYTE* sampleData = frameData + channel * bytesPerSample;
            encodeSample(decodeSample(sampleData, inputFormat) + probeValue,
                         sampleData,
                         inputFormat);
        }
    }
    activeProbeOffset_ += mixedFrames;
    if (activeProbeOffset_ >= activeProbe_.size()) {
        activeProbe_.clear();
        activeProbeOffset_ = 0;
        activeProbeGeneration_ = 0;
    }
    ringBuffer_->write(mixedInputBuffer_.data(), frameCount, false);
}

void OutputWorker::setVolumePercent(int volumePercent)
{
    requestedVolumePercent_ = std::clamp(volumePercent, 0, 100);
}

void OutputWorker::setSynchronizationMarginMilliseconds(int marginMilliseconds)
{
    baseTargetBufferMilliseconds_ = std::max(0, marginMilliseconds);
    updateTargetBuffer();
}

void OutputWorker::setManualDelayMilliseconds(int delayMilliseconds)
{
    manualDelayMilliseconds_ = std::max(0, delayMilliseconds);
    updateTargetBuffer();
}

void OutputWorker::setAutomaticDelayMilliseconds(int delayMilliseconds)
{
    automaticDelayMilliseconds_ = std::max(0, delayMilliseconds);
    updateTargetBuffer();
}

void OutputWorker::setAcousticDelayMilliseconds(int delayMilliseconds)
{
    acousticDelayMilliseconds_ = std::clamp(delayMilliseconds, 0, 500);
    updateTargetBuffer();
}

std::uint64_t OutputWorker::scheduleAcousticProbe(const std::vector<float>& probe,
                                                  float amplitude)
{
    if (probe.empty() || !initializedSuccessfully()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(probeMutex_);
    if (!pendingProbe_.empty() || !activeProbe_.empty()) {
        return 0;
    }
    const std::uint64_t generation = nextProbeGeneration_++;
    pendingProbe_ = probe;
    pendingProbeAmplitude_ = std::clamp(amplitude, 0.0F, 0.25F);
    pendingProbeGeneration_ = generation;
    return generation;
}

bool OutputWorker::waitForAcousticProbeStart(
    std::uint64_t generation,
    std::chrono::milliseconds timeout,
    std::int64_t* qpcHundredNanoseconds)
{
    std::unique_lock<std::mutex> lock(probeMutex_);
    const bool started = probeCondition_.wait_for(lock, timeout, [this, generation] {
        return lastStartedProbeGeneration_ >= generation || stopRequested_.load();
    });
    if (!started || lastStartedProbeGeneration_ != generation) {
        return false;
    }
    if (qpcHundredNanoseconds != nullptr) {
        *qpcHundredNanoseconds = lastProbeStartQpcHundredNanoseconds_;
    }
    return true;
}

void OutputWorker::cancelAcousticProbe(std::uint64_t generation)
{
    std::lock_guard<std::mutex> lock(probeMutex_);
    if (pendingProbeGeneration_ == generation) {
        pendingProbe_.clear();
        pendingProbeGeneration_ = 0;
    }
}

void OutputWorker::updateTargetBuffer()
{
    const int totalMilliseconds = baseTargetBufferMilliseconds_.load()
                                  + manualDelayMilliseconds_.load()
                                  + automaticDelayMilliseconds_.load()
                                  + acousticDelayMilliseconds_.load();
    targetBufferMilliseconds_ = totalMilliseconds;
    targetBufferFrames_ = sampleRate_ * static_cast<std::size_t>(totalMilliseconds) / 1000;
}

bool OutputWorker::initializedSuccessfully() const
{
    std::lock_guard<std::mutex> lock(initializationMutex_);
    return initializationSucceeded_;
}

QString OutputWorker::failureMessage() const
{
    std::lock_guard<std::mutex> lock(initializationMutex_);
    return failureMessage_;
}

std::uint64_t OutputWorker::underrunFrames() const
{
    return underrunFrames_.load();
}

std::uint64_t OutputWorker::correctedFrames() const
{
    return correctedFrames_.load();
}

double OutputWorker::streamLatencyMilliseconds() const
{
    return static_cast<double>(streamLatencyHundredNanoseconds_.load()) / 10000.0;
}

double OutputWorker::enginePeriodMilliseconds() const
{
    return static_cast<double>(enginePeriodHundredNanoseconds_.load()) / 10000.0;
}

bool OutputWorker::usesLowLatencyMode() const
{
    return lowLatencyMode_.load();
}

int OutputWorker::targetBufferMilliseconds() const
{
    return targetBufferMilliseconds_.load();
}

int OutputWorker::acousticDelayMilliseconds() const
{
    return acousticDelayMilliseconds_.load();
}

std::uint32_t OutputWorker::inputSampleRate() const
{
    return static_cast<std::uint32_t>(sampleRate_);
}

void OutputWorker::run()
{
    bool initializationReported = false;
    ComPtr<IAudioClient> audioClient;

    try {
        ComInitializer com(COINIT_MULTITHREADED);
        MmcssRegistration mmcss;

        const auto* inputWaveFormat = reinterpret_cast<const WAVEFORMATEX*>(waveFormat_.data());
        const AudioFormatInfo inputFormat = inspectFormat(inputWaveFormat);

        ComPtr<IMMDeviceEnumerator> enumerator;
        checkHresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator)),
                     "创建输出设备枚举器");

        ComPtr<IMMDevice> endpoint;
        const std::wstring id = toWideString(device_.id);
        checkHresult(enumerator->GetDevice(id.c_str(), &endpoint), "打开输出设备");

        auto activateAudioClient = [&endpoint]() {
            ComPtr<IAudioClient> client;
            checkHresult(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(client.GetAddressOf())),
                         "激活输出 IAudioClient");
            return client;
        };

        audioClient = activateAudioClient();
        WAVEFORMATEX* rawRenderFormat = nullptr;
        checkHresult(audioClient->GetMixFormat(&rawRenderFormat), "获取输出设备原生格式");
        const std::vector<BYTE> renderFormatBytes = copyWaveFormat(rawRenderFormat);
        CoTaskMemFree(rawRenderFormat);
        rawRenderFormat = nullptr;
        audioClient.Reset();

        const auto* renderWaveFormat = reinterpret_cast<const WAVEFORMATEX*>(
            renderFormatBytes.data());
        const AudioFormatInfo renderFormat = inspectFormat(renderWaveFormat);
        const DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                  | AUDCLNT_STREAMFLAGS_NOPERSIST;

        REFERENCE_TIME selectedPeriod = 0;
        bool rawProcessing = false;
        bool exclusiveMode = false;

        auto tryExclusiveInitialization = [&]() {
            audioClient.Reset();
            audioClient = activateAudioClient();

            if (audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                               renderWaveFormat,
                                               nullptr) != S_OK) {
                return false;
            }

            REFERENCE_TIME minimumPeriod = 0;
            if (FAILED(audioClient->GetDevicePeriod(nullptr, &minimumPeriod))
                || minimumPeriod <= 0) {
                return false;
            }

            if (FAILED(audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                               streamFlags,
                                               minimumPeriod,
                                               minimumPeriod,
                                               renderWaveFormat,
                                               nullptr))) {
                return false;
            }

            selectedPeriod = minimumPeriod;
            exclusiveMode = true;
            return true;
        };

        auto tryLowLatencyInitialization = [&](bool requestRawProcessing) {
            audioClient.Reset();
            audioClient = activateAudioClient();

            ComPtr<IAudioClient3> audioClient3;
            if (FAILED(audioClient.As(&audioClient3))) {
                return false;
            }

            if (requestRawProcessing) {
                AudioClientProperties properties{};
                properties.cbSize = sizeof(properties);
                properties.bIsOffload = FALSE;
                properties.eCategory = AudioCategory_Media;
                properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
                if (FAILED(audioClient3->SetClientProperties(&properties))) {
                    return false;
                }
            }

            UINT32 defaultPeriodFrames = 0;
            UINT32 fundamentalPeriodFrames = 0;
            UINT32 minimumPeriodFrames = 0;
            UINT32 maximumPeriodFrames = 0;
            if (FAILED(audioClient3->GetSharedModeEnginePeriod(renderWaveFormat,
                                                               &defaultPeriodFrames,
                                                               &fundamentalPeriodFrames,
                                                               &minimumPeriodFrames,
                                                               &maximumPeriodFrames))) {
                return false;
            }

            if (FAILED(audioClient3->InitializeSharedAudioStream(streamFlags,
                                                                 minimumPeriodFrames,
                                                                 renderWaveFormat,
                                                                 nullptr))) {
                return false;
            }

            selectedPeriod = static_cast<REFERENCE_TIME>(minimumPeriodFrames)
                             * 10000000LL / renderFormat.sampleRate;
            rawProcessing = requestRawProcessing;
            return true;
        };

        if ((preferExclusiveMode_ && tryExclusiveInitialization())
            || tryLowLatencyInitialization(true)
            || tryLowLatencyInitialization(false)) {
            lowLatencyMode_ = true;
        } else {
            audioClient.Reset();
            audioClient = activateAudioClient();
            checkHresult(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                 streamFlags,
                                                 0,
                                                 0,
                                                 renderWaveFormat,
                                                 nullptr),
                         "初始化输出流");
        }

        REFERENCE_TIME streamLatency = 0;
        if (SUCCEEDED(audioClient->GetStreamLatency(&streamLatency))) {
            streamLatencyHundredNanoseconds_ = streamLatency;
        }

        if (selectedPeriod > 0) {
            enginePeriodHundredNanoseconds_ = selectedPeriod;
        } else {
            REFERENCE_TIME defaultPeriod = 0;
            if (SUCCEEDED(audioClient->GetDevicePeriod(&defaultPeriod, nullptr))) {
                enginePeriodHundredNanoseconds_ = defaultPeriod;
            }
        }

        UniqueHandle audioEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!audioEvent) {
            checkHresult(HRESULT_FROM_WIN32(GetLastError()), "创建输出事件");
        }
        checkHresult(audioClient->SetEventHandle(audioEvent.get()), "设置输出事件");

        ComPtr<IAudioRenderClient> renderClient;
        checkHresult(audioClient->GetService(IID_PPV_ARGS(&renderClient)),
                     "获取 IAudioRenderClient");

        UINT32 endpointBufferFrames = 0;
        checkHresult(audioClient->GetBufferSize(&endpointBufferFrames), "获取输出缓冲大小");

        BYTE* initialBuffer = nullptr;
        checkHresult(renderClient->GetBuffer(endpointBufferFrames, &initialBuffer),
                     "获取初始输出缓冲");
        checkHresult(renderClient->ReleaseBuffer(endpointBufferFrames,
                                                 AUDCLNT_BUFFERFLAGS_SILENT),
                     "提交初始静音缓冲");

        checkHresult(audioClient->Start(), "启动输出流");
        completeInitialization(true);
        initializationReported = true;
        const QString streamMode = exclusiveMode
                                       ? QStringLiteral("独占低延迟")
                                       : (lowLatencyMode_ ? QStringLiteral("共享低延迟")
                                                          : QStringLiteral("兼容模式"));
        report(QStringLiteral("输出设备已启动：%1（%2%3，周期 %4 ms，流延迟 %5 ms，%6→%7 Hz）")
                   .arg(device_.name,
                        streamMode,
                        rawProcessing ? QStringLiteral("/RAW") : QString())
                   .arg(enginePeriodMilliseconds(), 0, 'f', 2)
                   .arg(streamLatencyMilliseconds(), 0, 'f', 2)
                   .arg(inputFormat.sampleRate)
                   .arg(renderFormat.sampleRate));

        std::vector<float> conversionBuffer;
        std::vector<BYTE> rawInputBuffer;
        double sourcePosition = 0.0;
        const double nominalRatio = static_cast<double>(inputFormat.sampleRate)
                                    / static_cast<double>(renderFormat.sampleRate);

        while (!stopRequested_) {
            const DWORD waitResult = WaitForSingleObject(audioEvent.get(), 100);
            if (waitResult == WAIT_TIMEOUT) {
                continue;
            }
            if (waitResult != WAIT_OBJECT_0) {
                checkHresult(HRESULT_FROM_WIN32(GetLastError()), "等待输出事件");
            }

            UINT32 writableFrames = endpointBufferFrames;
            if (!exclusiveMode) {
                UINT32 paddingFrames = 0;
                checkHresult(audioClient->GetCurrentPadding(&paddingFrames), "读取输出填充量");
                writableFrames = endpointBufferFrames - paddingFrames;
            }
            if (writableFrames == 0) {
                continue;
            }

            BYTE* target = nullptr;
            checkHresult(renderClient->GetBuffer(writableFrames, &target), "获取可写输出缓冲");

            const std::size_t targetBufferFrames = targetBufferFrames_.load();
            const std::size_t sourceFramesPerPass = static_cast<std::size_t>(
                                                        std::ceil(writableFrames * nominalRatio))
                                                    + 2;
            const std::size_t minimumStartupFrames = targetBufferFrames
                                                     + sourceFramesPerPass;
            if (!playbackStarted_
                && ringBuffer_->availableFrames() >= minimumStartupFrames) {
                playbackStarted_ = true;
                report(QStringLiteral("输出设备缓冲就绪：%1（%2 ms）")
                           .arg(device_.name)
                           .arg(targetBufferMilliseconds_.load()));
            }

            std::size_t producedFrames = 0;
            if (playbackStarted_) {
                const std::size_t conversionFrames = conversionBuffer.size()
                                                     / inputFormat.channels;
                const std::size_t bufferedFrames = ringBuffer_->availableFrames()
                                                   + conversionFrames
                                                   - std::min<std::size_t>(
                                                       conversionFrames,
                                                       static_cast<std::size_t>(sourcePosition));
                const double desiredBufferedFrames = static_cast<double>(targetBufferFrames)
                                                     + writableFrames * nominalRatio;
                const double errorFrames = static_cast<double>(bufferedFrames)
                                           - desiredBufferedFrames;
                // 较大的水位差通常来自运行时延迟调整，需要较快但连续地追踪；
                // 接近目标后切回微小修正，只补偿不同设备之间的时钟漂移。
                const double transitionThresholdFrames = inputFormat.sampleRate * 0.0015;
                const bool adjustingDelay = std::abs(errorFrames) > transitionThresholdFrames;
                const double correctionTimeSeconds = adjustingDelay ? 0.5 : 10.0;
                const double correctionLimit = adjustingDelay ? 0.05 : 0.001;
                const double ratioCorrection = std::clamp(
                    errorFrames
                        / (static_cast<double>(inputFormat.sampleRate)
                           * correctionTimeSeconds),
                    -correctionLimit,
                    correctionLimit);
                const double effectiveRatio = nominalRatio * (1.0 + ratioCorrection);

                const double finalSourcePosition = sourcePosition
                                                   + static_cast<double>(writableFrames - 1)
                                                         * effectiveRatio;
                const std::size_t requiredFrames = static_cast<std::size_t>(
                                                       std::floor(finalSourcePosition))
                                                   + 2;
                std::size_t currentConversionFrames = conversionBuffer.size()
                                                      / inputFormat.channels;
                if (currentConversionFrames < requiredFrames) {
                    const std::size_t requestedInputFrames = requiredFrames
                                                             - currentConversionFrames;
                    rawInputBuffer.resize(requestedInputFrames * inputFormat.bytesPerFrame);
                    const std::size_t framesRead = ringBuffer_->read(rawInputBuffer.data(),
                                                                     requestedInputFrames);
                    conversionBuffer.reserve((currentConversionFrames + framesRead)
                                             * inputFormat.channels);
                    const std::size_t bytesPerSample = inputFormat.bitsPerSample / 8;
                    for (std::size_t frame = 0; frame < framesRead; ++frame) {
                        const BYTE* frameData = rawInputBuffer.data()
                                                + frame * inputFormat.bytesPerFrame;
                        for (WORD channel = 0; channel < inputFormat.channels; ++channel) {
                            conversionBuffer.push_back(
                                decodeSample(frameData + channel * bytesPerSample, inputFormat));
                        }
                    }
                    currentConversionFrames = conversionBuffer.size() / inputFormat.channels;
                }

                const std::size_t outputBytesPerSample = renderFormat.bitsPerSample / 8;
                for (UINT32 outputFrame = 0; outputFrame < writableFrames; ++outputFrame) {
                    const double position = sourcePosition
                                            + static_cast<double>(outputFrame) * effectiveRatio;
                    const std::size_t leftFrame = static_cast<std::size_t>(std::floor(position));
                    const std::size_t rightFrame = leftFrame + 1;
                    if (rightFrame >= currentConversionFrames) {
                        break;
                    }
                    const float fraction = static_cast<float>(position - leftFrame);
                    BYTE* outputFrameData = target
                                            + static_cast<std::size_t>(outputFrame)
                                                  * renderFormat.bytesPerFrame;
                    for (WORD channel = 0; channel < renderFormat.channels; ++channel) {
                        const float left = mappedSample(conversionBuffer,
                                                        leftFrame,
                                                        channel,
                                                        inputFormat,
                                                        renderFormat);
                        const float right = mappedSample(conversionBuffer,
                                                         rightFrame,
                                                         channel,
                                                         inputFormat,
                                                         renderFormat);
                        const float volumeScale = static_cast<float>(
                                                      requestedVolumePercent_.load())
                                                  / 100.0F;
                        encodeSample((left + (right - left) * fraction) * volumeScale,
                                     outputFrameData + channel * outputBytesPerSample,
                                     renderFormat);
                    }
                    ++producedFrames;
                }

                correctedFrames_ += static_cast<std::uint64_t>(std::llround(
                    std::abs(effectiveRatio - nominalRatio) * producedFrames));

                sourcePosition += static_cast<double>(producedFrames) * effectiveRatio;
                std::size_t consumedFrames = static_cast<std::size_t>(std::floor(sourcePosition));
                currentConversionFrames = conversionBuffer.size() / inputFormat.channels;
                if (currentConversionFrames > 1) {
                    consumedFrames = std::min(consumedFrames, currentConversionFrames - 1);
                } else {
                    consumedFrames = 0;
                }
                if (consumedFrames > 0) {
                    conversionBuffer.erase(conversionBuffer.begin(),
                                           conversionBuffer.begin()
                                               + static_cast<std::ptrdiff_t>(consumedFrames
                                                                            * inputFormat.channels));
                    sourcePosition -= static_cast<double>(consumedFrames);
                }
            }

            if (producedFrames < writableFrames) {
                const std::size_t missingFrames = writableFrames - producedFrames;
                std::memset(target + producedFrames * renderFormat.bytesPerFrame,
                            0,
                            missingFrames * renderFormat.bytesPerFrame);
                if (playbackStarted_) {
                    underrunFrames_ += missingFrames;
                    if (missingFrames >= std::max<std::size_t>(1, writableFrames / 4)) {
                        // 捕获源长时间静音时可能停止投递 Loopback 包。
                        // 进入重新缓冲状态，避免把整段静音持续计为欠载。
                        playbackStarted_ = false;
                        conversionBuffer.clear();
                        sourcePosition = 0.0;
                    }
                }
            }

            checkHresult(renderClient->ReleaseBuffer(writableFrames, 0), "提交输出缓冲");
        }

        audioClient->Stop();
    } catch (const std::exception& exception) {
        const QString message = QString::fromUtf8(exception.what());
        if (!initializationReported) {
            completeInitialization(false, message);
        } else {
            {
                std::lock_guard<std::mutex> lock(initializationMutex_);
                initializationSucceeded_ = false;
                failureMessage_ = message;
            }
            report(QStringLiteral("输出设备运行失败：%1：%2").arg(device_.name, message));
        }
    }
}

void OutputWorker::completeInitialization(bool succeeded, const QString& failure)
{
    {
        std::lock_guard<std::mutex> lock(initializationMutex_);
        initializationCompleted_ = true;
        initializationSucceeded_ = succeeded;
        failureMessage_ = failure;
    }
    initializationCondition_.notify_all();
}

void OutputWorker::report(const QString& message) const
{
    if (statusCallback_) {
        statusCallback_(message);
    }
}
