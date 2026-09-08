#include "wasapi_helpers.h"

#include <comdef.h>

#include <cstring>

namespace
{
std::string toUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}
}

ComInitializer::ComInitializer(DWORD apartmentType)
{
    const HRESULT result = CoInitializeEx(nullptr, apartmentType);
    if (result == RPC_E_CHANGED_MODE) {
        return;
    }
    checkHresult(result, "CoInitializeEx");
    shouldUninitialize_ = true;
}

ComInitializer::~ComInitializer()
{
    if (shouldUninitialize_) {
        CoUninitialize();
    }
}

UniqueHandle::UniqueHandle(HANDLE handle)
    : handle_(handle)
{
}

UniqueHandle::~UniqueHandle()
{
    reset();
}

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept
    : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept
{
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

HANDLE UniqueHandle::get() const
{
    return handle_;
}

UniqueHandle::operator bool() const
{
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
}

void UniqueHandle::reset()
{
    if (*this) {
        CloseHandle(handle_);
    }
    handle_ = nullptr;
}

MmcssRegistration::MmcssRegistration()
{
    DWORD taskIndex = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
}

MmcssRegistration::~MmcssRegistration()
{
    if (handle_ != nullptr) {
        AvRevertMmThreadCharacteristics(handle_);
    }
}

void checkHresult(HRESULT result, const char* operation)
{
    if (SUCCEEDED(result)) {
        return;
    }

    const QString message = QStringLiteral("%1 失败：%2 (0x%3)")
                                .arg(QString::fromUtf8(operation))
                                .arg(hresultToQString(result))
                                .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    throw std::runtime_error(toUtf8(message));
}

QString hresultToQString(HRESULT result)
{
    switch (result) {
    case AUDCLNT_E_DEVICE_IN_USE:
        return QStringLiteral("设备正在被其他应用独占使用");
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        return QStringLiteral("系统策略或驱动不允许独占模式");
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return QStringLiteral("设备不支持请求的音频格式");
    case AUDCLNT_E_DEVICE_INVALIDATED:
        return QStringLiteral("音频设备已经断开或失效");
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return QStringLiteral("Windows 音频服务未运行");
    default:
        break;
    }

    const _com_error error(result);
    const wchar_t* message = error.ErrorMessage();
    if (message == nullptr) {
        return QStringLiteral("未知 HRESULT 错误");
    }
    return QString::fromWCharArray(message);
}

std::wstring toWideString(const QString& value)
{
    return value.toStdWString();
}

std::vector<BYTE> copyWaveFormat(const WAVEFORMATEX* format)
{
    if (format == nullptr) {
        throw std::invalid_argument("音频格式不能为空");
    }

    const std::size_t size = sizeof(WAVEFORMATEX) + format->cbSize;
    std::vector<BYTE> result(size);
    std::memcpy(result.data(), format, size);
    return result;
}
