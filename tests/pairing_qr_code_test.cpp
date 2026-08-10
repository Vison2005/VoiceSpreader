#include "../src/pairing_qr_code.h"

#include <QCoreApplication>
#include <QImage>
#include <QString>

#include <fstream>
#include <iostream>

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const QString payload = QStringLiteral(
        "VSP1:192.168.100.200:65535:0123456789ABCDEF0123456789ABCDEF:"
        "FEDCBA9876543210FEDCBA9876543210");
    const QImage image = createPairingQrCode(payload, 8);
    if (image.isNull() || image.width() != 360 || image.height() != 360) {
        std::cerr << "QR image dimensions are invalid\n";
        return 1;
    }

    // 使用最简单的 PGM 格式输出，便于外部解码器验证而不依赖图片插件。
    std::ofstream output("pairing_qr_test.pgm", std::ios::binary);
    output << "P5\n" << image.width() << ' ' << image.height() << "\n255\n";
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const unsigned char value = qRed(row[x]) == 0 ? 0 : 255;
            output.write(reinterpret_cast<const char*>(&value), 1);
        }
    }
    if (!output.good()) {
        std::cerr << "Unable to write QR verification image\n";
        return 1;
    }
    std::cout << payload.toStdString() << '\n';
    return 0;
}
