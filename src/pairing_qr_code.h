#pragma once

#include <QImage>
#include <QString>

QImage createPairingQrCode(const QString& text, int moduleScale = 8);
