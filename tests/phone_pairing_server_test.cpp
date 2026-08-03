#include "../src/phone_pairing_server.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>
#include <QtEndian>

#include <cmath>
#include <functional>
#include <iostream>

namespace
{
bool waitFor(const std::function<bool()>& predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) {
            return true;
        }
        QThread::msleep(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

QByteArray makePcmPacket(std::uint64_t firstFrame,
                         std::uint32_t sampleRate,
                         const std::vector<std::int16_t>& samples)
{
    const quint32 bodyLength = static_cast<quint32>(13 + samples.size() * 2);
    QByteArray packet(static_cast<int>(4 + bodyLength), Qt::Uninitialized);
    auto* data = reinterpret_cast<uchar*>(packet.data());
    qToBigEndian<quint32>(bodyLength, data);
    data[4] = 1;
    qToBigEndian<quint64>(firstFrame, data + 5);
    qToBigEndian<quint32>(sampleRate, data + 13);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const quint16 value = static_cast<quint16>(samples[index]);
        data[17 + index * 2] = static_cast<uchar>(value & 0xFFU);
        data[18 + index * 2] = static_cast<uchar>(value >> 8U);
    }
    return packet;
}
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    // 使用临时发现端口，避免正在运行的正式程序占用 39741 干扰测试。
    PhonePairingServer server(nullptr, 0);
    QString error;
    if (!server.start(&error)) {
        std::cerr << "Pairing server failed: " << error.toStdString() << '\n';
        return 1;
    }

    QUdpSocket discovery;
    if (!discovery.bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        std::cerr << "UDP test socket failed\n";
        return 1;
    }
    const QByteArray request = QByteArrayLiteral("VSP_DISCOVER ")
                               + server.pairingCode().toLatin1();
    discovery.writeDatagram(request,
                            QHostAddress::LocalHost,
                            server.discoveryPort());
    if (!waitFor([&] { return discovery.hasPendingDatagrams(); }, 1500)) {
        std::cerr << "Pairing-code discovery timed out\n";
        return 1;
    }
    const QNetworkDatagram discoveryReply = discovery.receiveDatagram();
    const QJsonObject discoveryObject = QJsonDocument::fromJson(
                                             discoveryReply.data())
                                             .object();
    if (discoveryObject.value(QStringLiteral("session")).toString().isEmpty()
        || discoveryObject.value(QStringLiteral("secret")).toString().isEmpty()) {
        std::cerr << "Pairing-code discovery response is invalid\n";
        return 1;
    }

    const QByteArray locateRequest = QByteArrayLiteral("VSP_LOCATE ")
                                     + discoveryObject
                                           .value(QStringLiteral("session"))
                                           .toString()
                                           .toLatin1();
    discovery.writeDatagram(locateRequest,
                            QHostAddress::LocalHost,
                            server.discoveryPort());
    if (!waitFor([&] { return discovery.hasPendingDatagrams(); }, 1500)) {
        std::cerr << "QR session location timed out\n";
        return 1;
    }
    const QJsonObject locationObject = QJsonDocument::fromJson(
                                            discovery.receiveDatagram().data())
                                            .object();
    if (locationObject.value(QStringLiteral("session"))
            != discoveryObject.value(QStringLiteral("session"))
        || locationObject.contains(QStringLiteral("secret"))) {
        std::cerr << "QR session location response is invalid\n";
        return 1;
    }

    const QStringList payloadParts = server.pairingPayload().split(QLatin1Char(':'));
    if (payloadParts.size() != 5) {
        std::cerr << "Pairing payload is invalid\n";
        return 1;
    }
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    if (!waitFor([&] { return client.state() == QAbstractSocket::ConnectedState; },
                 1500)) {
        std::cerr << "TCP pairing connection timed out\n";
        return 1;
    }

    QJsonObject hello;
    hello.insert(QStringLiteral("type"), QStringLiteral("hello"));
    hello.insert(QStringLiteral("protocol"), 1);
    hello.insert(QStringLiteral("session"), payloadParts.at(3));
    hello.insert(QStringLiteral("secret"), payloadParts.at(4));
    hello.insert(QStringLiteral("deviceName"), QStringLiteral("test phone"));
    client.write(QJsonDocument(hello).toJson(QJsonDocument::Compact) + '\n');
    client.flush();
    if (!waitFor([&] { return client.canReadLine(); }, 1500)) {
        std::cerr << "Authenticated handshake timed out\n";
        return 1;
    }
    const QJsonObject accepted = QJsonDocument::fromJson(client.readLine()).object();
    if (accepted.value(QStringLiteral("type")).toString()
        != QStringLiteral("accepted")) {
        std::cerr << "Authenticated handshake was rejected\n";
        return 1;
    }

    constexpr std::uint64_t firstFrame = 123456;
    const std::vector<std::int16_t> samples{0, 16384, -16384, 32767, -32768};
    client.write(makePcmPacket(firstFrame, 48000, samples));
    client.flush();
    if (!waitFor(
            [&] {
                return server.remoteBuffer()->latestFrameIndex()
                       >= firstFrame + samples.size();
            },
            1500)) {
        std::cerr << "PCM frame did not reach the remote microphone buffer\n";
        return 1;
    }
    const RemoteAudioSnapshot snapshot = server.remoteBuffer()->snapshotFrom(firstFrame);
    if (snapshot.sampleRate != 48000 || snapshot.samples.size() != samples.size()
        || std::abs(snapshot.samples[1] - 0.5F) > 0.0001F
        || std::abs(snapshot.samples[2] + 0.5F) > 0.0001F) {
        std::cerr << "PCM frame was decoded incorrectly\n";
        return 1;
    }

    std::cout << "Phone pairing and PCM protocol test passed\n";
    return 0;
}
