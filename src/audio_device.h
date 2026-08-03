#pragma once

#include <QString>

struct AudioDevice
{
    QString id;
    QString name;
    bool isDefault = false;
};

struct OutputDeviceSettings
{
    AudioDevice device;
    int volumePercent = 100;
    int extraDelayMilliseconds = 0;
};
