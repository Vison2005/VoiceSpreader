#include "wasapi_device_manager.h"

#include "wasapi_helpers.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace
{
QString endpointId(IMMDevice* device)
{
    LPWSTR rawId = nullptr;
    checkHresult(device->GetId(&rawId), "IMMDevice::GetId");
    const QString id = QString::fromWCharArray(rawId);
    CoTaskMemFree(rawId);
    return id;
}

QString endpointName(IMMDevice* device)
{
    ComPtr<IPropertyStore> properties;
    checkHresult(device->OpenPropertyStore(STGM_READ, &properties), "IMMDevice::OpenPropertyStore");

    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    if (FAILED(result)) {
        PropVariantClear(&value);
        checkHresult(result, "IPropertyStore::GetValue");
    }

    const QString name = value.vt == VT_LPWSTR && value.pwszVal != nullptr
                             ? QString::fromWCharArray(value.pwszVal)
                             : QStringLiteral("未命名音频设备");
    PropVariantClear(&value);
    return name;
}

QVector<AudioDevice> enumerateEndpoints(EDataFlow dataFlow,
                                        const char* operation,
                                        QString* errorMessage)
{
    QVector<AudioDevice> result;

    try {
        ComInitializer com(COINIT_APARTMENTTHREADED);

        ComPtr<IMMDeviceEnumerator> enumerator;
        checkHresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator)),
                     "创建 MMDeviceEnumerator");

        QString defaultId;
        ComPtr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(dataFlow,
                                                          eMultimedia,
                                                          &defaultDevice))) {
            defaultId = endpointId(defaultDevice.Get());
        }

        ComPtr<IMMDeviceCollection> collection;
        checkHresult(enumerator->EnumAudioEndpoints(dataFlow,
                                                    DEVICE_STATE_ACTIVE,
                                                    &collection),
                     operation);

        UINT count = 0;
        checkHresult(collection->GetCount(&count), "获取设备数量");

        result.reserve(static_cast<int>(count));
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            checkHresult(collection->Item(index, &device), "读取音频设备");

            AudioDevice info;
            info.id = endpointId(device.Get());
            info.name = endpointName(device.Get());
            info.isDefault = info.id == defaultId;
            result.push_back(info);
        }

        std::sort(result.begin(), result.end(), [](const AudioDevice& left,
                                                   const AudioDevice& right) {
            if (left.isDefault != right.isDefault) {
                return left.isDefault;
            }
            return left.name.localeAwareCompare(right.name) < 0;
        });
    } catch (const std::exception& exception) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromUtf8(exception.what());
        }
        result.clear();
    }

    return result;
}
}

QVector<AudioDevice> WasapiDeviceManager::enumerateRenderDevices(QString* errorMessage)
{
    return enumerateEndpoints(eRender, "枚举播放设备", errorMessage);
}

QVector<AudioDevice> WasapiDeviceManager::enumerateCaptureDevices(QString* errorMessage)
{
    return enumerateEndpoints(eCapture, "枚举录音设备", errorMessage);
}
