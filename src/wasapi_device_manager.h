#pragma once

#include "audio_device.h"

#include <QString>
#include <QVector>

class WasapiDeviceManager
{
public:
    static QVector<AudioDevice> enumerateRenderDevices(QString* errorMessage = nullptr);
    static QVector<AudioDevice> enumerateCaptureDevices(QString* errorMessage = nullptr);
};
