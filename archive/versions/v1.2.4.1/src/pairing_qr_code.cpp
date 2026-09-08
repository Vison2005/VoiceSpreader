#include "pairing_qr_code.h"

#include <QColor>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace
{
constexpr int version = 5;
constexpr int qrSize = version * 4 + 17;
constexpr int dataCodewords = 108;
constexpr int errorCorrectionCodewords = 26;
constexpr char alphanumericCharset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

class BitBuffer
{
public:
    void append(std::uint32_t value, int bitCount)
    {
        for (int bit = bitCount - 1; bit >= 0; --bit) {
            bits_.push_back(((value >> bit) & 1U) != 0);
        }
    }

    std::size_t size() const { return bits_.size(); }
    bool at(std::size_t index) const { return bits_.at(index); }

private:
    std::vector<bool> bits_;
};

std::uint8_t finiteFieldMultiply(std::uint8_t left, std::uint8_t right)
{
    std::uint8_t result = 0;
    for (int bit = 7; bit >= 0; --bit) {
        result = static_cast<std::uint8_t>((result << 1)
                                           ^ ((result >> 7) * 0x1DU));
        result ^= static_cast<std::uint8_t>(((right >> bit) & 1U) * left);
    }
    return result;
}

std::vector<std::uint8_t> reedSolomonGenerator(int degree)
{
    std::vector<std::uint8_t> result(static_cast<std::size_t>(degree), 0);
    result.back() = 1;
    std::uint8_t root = 1;
    for (int index = 0; index < degree; ++index) {
        for (int coefficient = 0; coefficient < degree; ++coefficient) {
            result[static_cast<std::size_t>(coefficient)] = finiteFieldMultiply(
                result[static_cast<std::size_t>(coefficient)], root);
            if (coefficient + 1 < degree) {
                result[static_cast<std::size_t>(coefficient)] ^=
                    result[static_cast<std::size_t>(coefficient + 1)];
            }
        }
        root = finiteFieldMultiply(root, 0x02);
    }
    return result;
}

std::vector<std::uint8_t> reedSolomonRemainder(
    const std::vector<std::uint8_t>& data,
    const std::vector<std::uint8_t>& generator)
{
    std::vector<std::uint8_t> result(generator.size(), 0);
    for (std::uint8_t value : data) {
        const std::uint8_t factor = value ^ result.front();
        std::move(result.begin() + 1, result.end(), result.begin());
        result.back() = 0;
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] ^= finiteFieldMultiply(generator[index], factor);
        }
    }
    return result;
}

class Matrix
{
public:
    Matrix()
        : modules_(qrSize * qrSize, false)
        , functions_(qrSize * qrSize, false)
    {
    }

    void setFunction(int x, int y, bool black)
    {
        if (x < 0 || x >= qrSize || y < 0 || y >= qrSize) {
            return;
        }
        modules_[index(x, y)] = black;
        functions_[index(x, y)] = true;
    }

    bool isFunction(int x, int y) const { return functions_[index(x, y)]; }
    void set(int x, int y, bool black) { modules_[index(x, y)] = black; }
    bool get(int x, int y) const { return modules_[index(x, y)]; }

private:
    static std::size_t index(int x, int y)
    {
        return static_cast<std::size_t>(y * qrSize + x);
    }

    std::vector<bool> modules_;
    std::vector<bool> functions_;
};

void drawFinder(Matrix& matrix, int centerX, int centerY)
{
    for (int y = -4; y <= 4; ++y) {
        for (int x = -4; x <= 4; ++x) {
            const int distance = std::max(std::abs(x), std::abs(y));
            matrix.setFunction(centerX + x,
                               centerY + y,
                               distance != 2 && distance != 4);
        }
    }
}

void drawAlignment(Matrix& matrix, int centerX, int centerY)
{
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            matrix.setFunction(centerX + x,
                               centerY + y,
                               std::max(std::abs(x), std::abs(y)) != 1);
        }
    }
}

std::uint16_t formatBits(int mask)
{
    const int data = (1 << 3) | mask; // L 级纠错的格式标识为 01。
    int remainder = data << 10;
    for (int bit = 14; bit >= 10; --bit) {
        if (((remainder >> bit) & 1) != 0) {
            remainder ^= 0x537 << (bit - 10);
        }
    }
    return static_cast<std::uint16_t>(((data << 10) | remainder) ^ 0x5412);
}

void reserveAndDrawFormat(Matrix& matrix, int mask, bool reserveOnly)
{
    const std::uint16_t bits = formatBits(mask);
    auto value = [&](int bit) {
        return reserveOnly ? false : ((bits >> bit) & 1U) != 0;
    };
    for (int index = 0; index <= 5; ++index) matrix.setFunction(8, index, value(index));
    matrix.setFunction(8, 7, value(6));
    matrix.setFunction(8, 8, value(7));
    matrix.setFunction(7, 8, value(8));
    for (int index = 9; index < 15; ++index) {
        matrix.setFunction(14 - index, 8, value(index));
    }
    for (int index = 0; index < 8; ++index) {
        matrix.setFunction(qrSize - 1 - index, 8, value(index));
    }
    for (int index = 8; index < 15; ++index) {
        matrix.setFunction(8, qrSize - 15 + index, value(index));
    }
    matrix.setFunction(8, qrSize - 8, true);
}

Matrix encodeAlphanumeric(const QString& text)
{
    const QByteArray encoded = text.toLatin1();
    if (encoded.size() > 106) {
        throw std::invalid_argument("配对信息过长，无法放入固定版本二维码");
    }

    std::vector<int> values;
    values.reserve(static_cast<std::size_t>(encoded.size()));
    for (char character : encoded) {
        const char* position = std::find(std::begin(alphanumericCharset),
                                         std::end(alphanumericCharset) - 1,
                                         character);
        if (position == std::end(alphanumericCharset) - 1) {
            throw std::invalid_argument("配对二维码包含不支持的字符");
        }
        values.push_back(static_cast<int>(position - alphanumericCharset));
    }

    BitBuffer bits;
    bits.append(0x2, 4);
    bits.append(static_cast<std::uint32_t>(values.size()), 9);
    for (std::size_t index = 0; index + 1 < values.size(); index += 2) {
        bits.append(static_cast<std::uint32_t>(values[index] * 45 + values[index + 1]),
                    11);
    }
    if ((values.size() % 2) != 0) {
        bits.append(static_cast<std::uint32_t>(values.back()), 6);
    }
    const std::size_t dataCapacityBits = dataCodewords * 8;
    bits.append(0, static_cast<int>(std::min<std::size_t>(4,
                                                         dataCapacityBits - bits.size())));
    while ((bits.size() % 8) != 0) bits.append(0, 1);

    std::vector<std::uint8_t> codewords;
    for (std::size_t offset = 0; offset < bits.size(); offset += 8) {
        std::uint8_t value = 0;
        for (int bit = 0; bit < 8; ++bit) {
            value = static_cast<std::uint8_t>((value << 1)
                                              | (bits.at(offset + bit) ? 1 : 0));
        }
        codewords.push_back(value);
    }
    for (int padIndex = 0; codewords.size() < dataCodewords; ++padIndex) {
        codewords.push_back((padIndex % 2) == 0 ? 0xEC : 0x11);
    }
    const std::vector<std::uint8_t> generator = reedSolomonGenerator(
        errorCorrectionCodewords);
    const std::vector<std::uint8_t> remainder = reedSolomonRemainder(codewords,
                                                                     generator);
    codewords.insert(codewords.end(), remainder.begin(), remainder.end());

    Matrix matrix;
    for (int index = 0; index < qrSize; ++index) {
        matrix.setFunction(6, index, (index % 2) == 0);
        matrix.setFunction(index, 6, (index % 2) == 0);
    }
    drawFinder(matrix, 3, 3);
    drawFinder(matrix, qrSize - 4, 3);
    drawFinder(matrix, 3, qrSize - 4);
    drawAlignment(matrix, 30, 30);
    reserveAndDrawFormat(matrix, 0, true);

    std::size_t bitIndex = 0;
    bool upward = true;
    for (int right = qrSize - 1; right >= 1; right -= 2) {
        if (right == 6) --right;
        for (int vertical = 0; vertical < qrSize; ++vertical) {
            const int y = upward ? qrSize - 1 - vertical : vertical;
            for (int column = 0; column < 2; ++column) {
                const int x = right - column;
                if (matrix.isFunction(x, y)) {
                    continue;
                }
                bool black = false;
                if (bitIndex < codewords.size() * 8) {
                    black = ((codewords[bitIndex >> 3]
                              >> (7 - static_cast<int>(bitIndex & 7)))
                             & 1U) != 0;
                    ++bitIndex;
                }
                if (((x + y) % 2) == 0) {
                    black = !black;
                }
                matrix.set(x, y, black);
            }
        }
        upward = !upward;
    }
    reserveAndDrawFormat(matrix, 0, false);
    return matrix;
}
}

QImage createPairingQrCode(const QString& text, int moduleScale)
{
    const Matrix matrix = encodeAlphanumeric(text.toUpper());
    constexpr int quietZone = 4;
    const int scale = std::max(2, moduleScale);
    const int imageSize = (qrSize + quietZone * 2) * scale;
    QImage image(imageSize, imageSize, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < qrSize; ++y) {
        for (int x = 0; x < qrSize; ++x) {
            if (!matrix.get(x, y)) {
                continue;
            }
            for (int pixelY = 0; pixelY < scale; ++pixelY) {
                QRgb* row = reinterpret_cast<QRgb*>(
                    image.scanLine((y + quietZone) * scale + pixelY));
                for (int pixelX = 0; pixelX < scale; ++pixelX) {
                    row[(x + quietZone) * scale + pixelX] = qRgb(0, 0, 0);
                }
            }
        }
    }
    return image;
}
