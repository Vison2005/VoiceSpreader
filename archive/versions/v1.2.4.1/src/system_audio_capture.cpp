#include "system_audio_capture.h"

#include "wasapi_helpers.h"

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr std::uint32_t streamSampleRate = 48000;
constexpr std::uint16_t streamChannels = 2;

WAVEFORMATEX streamingFormat()
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = streamChannels;
    format.nSamplesPerSec = streamSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = streamChannels * sizeof(std::int16_t);
    format.nAvgBytesPerSec = streamSampleRate * format.nBlockAlign;
    return format;
}
}

SystemAudioCapture::SystemAudioCapture(StatusCallback statusCallback)
    : statusCallback_(std::move(statusCallback))
{
}

SystemAudioCapture::~SystemAudioCapture()
{
    stop();
}

bool SystemAudioCapture::start(const AudioDevice& device,
                               int volumePercent,
                               PcmCallback pcmCallback)
{
    stop();
    if (device.id.isEmpty() || !pcmCallback) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(initializationMutex_);
        initializationFinished_ = false;
        initializationSucceeded_ = false;
        failureMessage_.clear();
    }
    stopRequested_ = false;
    volumePercent_ = std::clamp(volumePercent, 0, 100);
    pcmCallback_ = std::move(pcmCallback);
    thread_ = std::thread(&SystemAudioCapture::run, this, device);

    std::unique_lock<std::mutex> lock(initializationMutex_);
    const bool completed = initializationCondition_.wait_for(
        lock,
        std::chrono::seconds(5),
        [this] { return initializationFinished_; });
    const bool succeeded = completed && initializationSucceeded_;
    const QString failure = completed
                                ? failureMessage_
                                : QStringLiteral("Windows 系统声音捕获初始化超时");
    lock.unlock();

    if (!succeeded) {
        stop();
        if (statusCallback_) {
            statusCallback_(failure.isEmpty()
                                ? QStringLiteral("无法启动 Windows 系统声音捕获")
                                : failure);
        }
    }
    return succeeded;
}

void SystemAudioCapture::stop()
{
    stopRequested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    active_ = false;
    pcmCallback_ = {};
}

bool SystemAudioCapture::isActive() const
{
    return active_.load();
}

void SystemAudioCapture::setVolumePercent(int volumePercent)
{
    volumePercent_ = std::clamp(volumePercent, 0, 100);
}

void SystemAudioCapture::run(AudioDevice device)
{
    ComPtr<IAudioClient> audioClient;
    try {
        ComInitializer com(COINIT_MULTITHREADED);
        MmcssRegistration mmcss;

        ComPtr<IMMDeviceEnumerator> enumerator;
        checkHresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                      nullptr,
                                      CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator)),
                     "创建系统声音捕获设备枚举器");

        ComPtr<IMMDevice> endpoint;
        const std::wstring deviceId = toWideString(device.id);
        checkHresult(enumerator->GetDevice(deviceId.c_str(), &endpoint),
                     "打开系统声音捕获端点");
        checkHresult(endpoint->Activate(__uuidof(IAudioClient),
                                       CLSCTX_ALL,
                                       nullptr,
                                       reinterpret_cast<void**>(audioClient.GetAddressOf())),
                     "激活系统声音捕获客户端");

        WAVEFORMATEX format = streamingFormat();
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK
                            | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        checkHresult(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                             flags,
                                             0,
                                             0,
                                             &format,
                                             nullptr),
                     "初始化独立 WASAPI Loopback");

        UniqueHandle captureEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!captureEvent) {
            checkHresult(HRESULT_FROM_WIN32(GetLastError()), "创建系统声音捕获事件");
        }
        checkHresult(audioClient->SetEventHandle(captureEvent.get()),
                     "设置系统声音捕获事件");

        ComPtr<IAudioCaptureClient> captureClient;
        checkHresult(audioClient->GetService(IID_PPV_ARGS(&captureClient)),
                     "获取系统声音捕获服务");
        checkHresult(audioClient->Start(), "启动独立 WASAPI Loopback");

        active_ = true;
        finishInitialization(true);
        if (statusCallback_) {
            statusCallback_(QStringLiteral("正在将 Windows 声音发送到手机：%1，48 kHz / 双声道")
                                .arg(device.name));
        }

        std::uint64_t frameIndex = 0;
        std::vector<std::int16_t> adjustedSamples;
        while (!stopRequested_) {
            const DWORD waitResult = WaitForSingleObject(captureEvent.get(), 100);
            if (waitResult == WAIT_TIMEOUT) {
                continue;
            }
            if (waitResult != WAIT_OBJECT_0) {
                checkHresult(HRESULT_FROM_WIN32(GetLastError()), "等待系统声音捕获事件");
            }

            UINT32 packetFrames = 0;
            checkHresult(captureClient->GetNextPacketSize(&packetFrames),
                         "查询系统声音捕获数据");
            while (packetFrames > 0 && !stopRequested_) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD packetFlags = 0;
                checkHresult(captureClient->GetBuffer(&data,
                                                      &frames,
                                                      &packetFlags,
                                                      nullptr,
                                                      nullptr),
                             "读取系统声音捕获数据");

                const std::size_t sampleCount = static_cast<std::size_t>(frames)
                                                * streamChannels;
                const bool silent = (packetFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const int volume = volumePercent_.load();
                const std::int16_t* outgoing = reinterpret_cast<const std::int16_t*>(data);
                if (silent || volume != 100) {
                    adjustedSamples.resize(sampleCount);
                    if (silent || volume == 0) {
                        std::fill(adjustedSamples.begin(), adjustedSamples.end(), 0);
                    } else {
                        for (std::size_t index = 0; index < sampleCount; ++index) {
                            const int scaled = static_cast<int>(outgoing[index]) * volume / 100;
                            adjustedSamples[index] = static_cast<std::int16_t>(
                                std::clamp(scaled, -32768, 32767));
                        }
                    }
                    outgoing = adjustedSamples.data();
                }

                if (pcmCallback_ && sampleCount > 0) {
                    pcmCallback_(frameIndex,
                                 streamSampleRate,
                                 streamChannels,
                                 outgoing,
                                 sampleCount);
                }
                frameIndex += frames;
                checkHresult(captureClient->ReleaseBuffer(frames),
                             "释放系统声音捕获数据");
                checkHresult(captureClient->GetNextPacketSize(&packetFrames),
                             "继续查询系统声音捕获数据");
            }
        }
        audioClient->Stop();
    } catch (const std::exception& exception) {
        const QString failure = QStringLiteral("Windows 声音到手机链路失败：%1")
                                    .arg(QString::fromUtf8(exception.what()));
        finishInitialization(false, failure);
        if (active_ && statusCallback_) {
            statusCallback_(failure);
        }
    }
    active_ = false;
    finishInitialization(false, failureMessage_);
}

void SystemAudioCapture::finishInitialization(bool succeeded,
                                               const QString& failureMessage)
{
    std::lock_guard<std::mutex> lock(initializationMutex_);
    if (initializationFinished_) {
        return;
    }
    initializationFinished_ = true;
    initializationSucceeded_ = succeeded;
    failureMessage_ = failureMessage;
    initializationCondition_.notify_all();
}
