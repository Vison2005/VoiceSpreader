#pragma once

#include <QHash>
#include <QSet>
#include <QString>

#include <algorithm>

inline QHash<QString, int> normalizeRelativeOutputDelays(
    const QHash<QString, int>& requestedDelays,
    const QSet<QString>& activeDeviceIds = {})
{
    QSet<QString> deviceIds = activeDeviceIds;
    if (deviceIds.isEmpty()) {
        deviceIds = QSet<QString>(requestedDelays.keyBegin(), requestedDelays.keyEnd());
    }

    int minimumDelay = 0;
    for (const QString& deviceId : deviceIds) {
        minimumDelay = std::min(minimumDelay, requestedDelays.value(deviceId));
    }

    QHash<QString, int> normalized;
    for (const QString& deviceId : deviceIds) {
        normalized.insert(deviceId, requestedDelays.value(deviceId) - minimumDelay);
    }
    return normalized;
}
