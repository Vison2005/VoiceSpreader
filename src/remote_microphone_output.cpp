#include "remote_microphone_output.h"

#include "output_worker.h"

#include <Windows.h>
#include <mmreg.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>

RemoteMicrophoneOutput::RemoteMicrophoneOutput(StatusCallback statusCallback)
    : statusCallback_(std::move(statusCallback))
{
}

RemoteMicrophoneOutput::~RemoteMicrophoneOutput()
{
    stop();
}

bool RemoteMicrophoneOutput::start(const AudioDevice& device,
                                   std::uint32_t sampleRate,
                                   int bufferMilliseconds,
                                   int volumePercent)
{
    if (device.id.isEmpty() || sampleRate < 8000 || sampleRate > 192000) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_ != nullptr) {
        worker_->stop();
        worker_.reset();
    }

    try {
        auto worker = std::make_unique<OutputWorker>(
            device,
            createMonoFloatFormat(sampleRate),
            std::clamp(bufferMilliseconds, 5, 200),
            std::clamp(volumePercent, 0, 100),
            false,
            statusCallback_,
            true);
        worker->start();
        if (!worker->waitUntilInitialized(std::chrono::seconds(5))) {
            if (statusCallback_) {
                statusCallback_(worker->failureMessage().isEmpty()
                                    ? QStringLiteral("无法启动手机麦克风输出端点")
                                    : worker->failureMessage());
            }
            worker->stop();
            return false;
        }
        sampleRate_ = sampleRate;
        worker_ = std::move(worker);
        return true;
    } catch (const std::exception& exception) {
        if (statusCallback_) {
            statusCallback_(QStringLiteral("手机麦克风输出启动失败：%1")
                                .arg(QString::fromUtf8(exception.what())));
        }
        sampleRate_ = 0;
        return false;
    }
}

void RemoteMicrophoneOutput::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_ != nullptr) {
        worker_->stop();
        worker_.reset();
    }
    sampleRate_ = 0;
}

bool RemoteMicrophoneOutput::isActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_ != nullptr;
}

void RemoteMicrophoneOutput::setVolumePercent(int volumePercent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_ != nullptr) {
        worker_->setVolumePercent(volumePercent);
    }
}

void RemoteMicrophoneOutput::push(std::uint32_t sampleRate,
                                  const std::vector<float>& samples)
{
    if (samples.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_ == nullptr || sampleRate != sampleRate_) {
        return;
    }
    worker_->push(reinterpret_cast<const BYTE*>(samples.data()),
                  samples.size(),
                  false,
                  false);
}

std::vector<unsigned char> RemoteMicrophoneOutput::createMonoFloatFormat(
    std::uint32_t sampleRate)
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1;
    format.nSamplesPerSec = sampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = sizeof(float);
    format.nAvgBytesPerSec = sampleRate * format.nBlockAlign;
    format.cbSize = 0;

    std::vector<unsigned char> bytes(sizeof(format));
    std::memcpy(bytes.data(), &format, sizeof(format));
    return bytes;
}
