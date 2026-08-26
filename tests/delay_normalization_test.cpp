#include "delay_normalization.h"

#include <QHash>
#include <QSet>
#include <QString>

#include <iostream>

namespace
{
bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}

int main()
{
    const QHash<QString, int> signedDelays{
        {QStringLiteral("early"), -40},
        {QStringLiteral("reference"), 0},
        {QStringLiteral("late"), 25},
    };
    const QHash<QString, int> normalized = normalizeRelativeOutputDelays(signedDelays);
    if (!require(normalized.value(QStringLiteral("early")) == 0,
                 "最早设备没有归一化到零延迟")
        || !require(normalized.value(QStringLiteral("reference")) == 40,
                    "参考设备没有保留相对差值")
        || !require(normalized.value(QStringLiteral("late")) == 65,
                    "较晚设备没有保留相对差值")) {
        return 1;
    }

    const QSet<QString> active{
        QStringLiteral("reference"),
        QStringLiteral("late"),
    };
    const QHash<QString, int> activeOnly = normalizeRelativeOutputDelays(signedDelays, active);
    return require(activeOnly.value(QStringLiteral("reference")) == 0,
                   "非活动设备不应改变归一化基准")
               && require(activeOnly.value(QStringLiteral("late")) == 25,
                          "活动设备的相对差值发生变化")
        ? 0
        : 1;
}
