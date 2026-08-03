#include "phone_pairing_server.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
constexpr quint16 discoveryPort = 39741;
constexpr quint32 maximumFrameBytes = 256 * 1024;

QString randomHex(int bytes)
{
    QByteArray data(bytes, Qt::Uninitialized);
    for (int index = 0; index < bytes; index += 4) {
        const quint32 value = QRandomGenerator::system()->generate();
        const int copyBytes = std::min(4, bytes - index);
        std::memcpy(data.data() + index, &value, static_cast<std::size_t>(copyBytes));
    }
    return QString::fromLatin1(data.toHex().toUpper());
}
}

PhonePairingServer::PhonePairingServer(QObject* parent)
    : QObject(parent)
    , tcpServer_(new QTcpServer(this))
    , discoverySocket_(new QUdpSocket(this))
    , remoteBuffer_(std::make_shared<RemoteMicrophoneBuffer>())
{
    connect(tcpServer_, &QTcpServer::newConnection,
            this, &PhonePairingServer::acceptConnection);
    connect(discoverySocket_, &QUdpSocket::readyRead,
            this, &PhonePairingServer::readDiscoveryDatagrams);
    generateCredentials();
}

PhonePairingServer::~PhonePairingServer()
{
    stop();
}

bool PhonePairingServer::start(QString* errorMessage)
{
    if (!tcpServer_->isListening()
        && !tcpServer_->listen(QHostAddress::AnyIPv4, 0)) {
        if (errorMessage != nullptr) {
            *errorMessage = tcpServer_->errorString();
        }
        return false;
    }
    if (discoverySocket_->state() == QAbstractSocket::UnconnectedState) {
        const bool discoveryReady = discoverySocket_->bind(
            QHostAddress::AnyIPv4,
            discoveryPort,
            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        if (!discoveryReady) {
            emit statusChanged(QStringLiteral("手机配对码发现不可用：%1；二维码仍可使用")
                                   .arg(discoverySocket_->errorString()));
        }
    }
    localAddress_ = chooseLocalIpv4Address();
    emit statusChanged(QStringLiteral("手机麦克风配对服务已启动：%1:%2")
                           .arg(localAddress_)
                           .arg(tcpServer_->serverPort()));
    return true;
}

void PhonePairingServer::stop()
{
    if (client_ != nullptr) {
        client_->disconnect(this);
        client_->disconnectFromHost();
        client_->deleteLater();
        client_ = nullptr;
    }
    authenticated_ = false;
    phoneName_.clear();
    remoteBuffer_->setConnected(false);
    tcpServer_->close();
    discoverySocket_->close();
}

void PhonePairingServer::resetPairing()
{
    if (client_ != nullptr) {
        client_->disconnectFromHost();
    }
    generateCredentials();
    emit statusChanged(QStringLiteral("已生成新的手机配对凭据"));
}

QString PhonePairingServer::pairingCode() const
{
    return pairingCode_;
}

QString PhonePairingServer::pairingPayload() const
{
    return QStringLiteral("VSP1:%1:%2:%3:%4")
        .arg(localAddress_)
        .arg(tcpServer_->serverPort())
        .arg(sessionId_)
        .arg(sessionSecret_);
}

QString PhonePairingServer::localAddress() const
{
    return localAddress_;
}

quint16 PhonePairingServer::serverPort() const
{
    return tcpServer_->serverPort();
}

bool PhonePairingServer::phoneConnected() const
{
    return authenticated_ && client_ != nullptr;
}

QString PhonePairingServer::connectedPhoneName() const
{
    return phoneName_;
}

std::shared_ptr<RemoteMicrophoneBuffer> PhonePairingServer::remoteBuffer() const
{
    return remoteBuffer_;
}

void PhonePairingServer::acceptConnection()
{
    while (tcpServer_->hasPendingConnections()) {
        QTcpSocket* incoming = tcpServer_->nextPendingConnection();
        if (client_ != nullptr) {
            incoming->write("{\"type\":\"error\",\"message\":\"phone already connected\"}\n");
            incoming->disconnectFromHost();
            incoming->deleteLater();
            continue;
        }
        client_ = incoming;
        authenticated_ = false;
        receiveBuffer_.clear();
        connect(client_, &QTcpSocket::readyRead,
                this, &PhonePairingServer::readClientData);
        connect(client_, &QTcpSocket::disconnected,
                this, &PhonePairingServer::clientDisconnected);
    }
}

void PhonePairingServer::readClientData()
{
    if (client_ == nullptr) {
        return;
    }
    receiveBuffer_.append(client_->readAll());
    if (!authenticated_) {
        const int newline = receiveBuffer_.indexOf('\n');
        if (newline < 0) {
            if (receiveBuffer_.size() > 4096) {
                rejectClient(QStringLiteral("握手数据过长"));
            }
            return;
        }
        const QByteArray line = receiveBuffer_.left(newline);
        receiveBuffer_.remove(0, newline + 1);
        if (!processHandshakeLine(line)) {
            return;
        }
    }
    processFrames();
}

void PhonePairingServer::clientDisconnected()
{
    if (client_ == nullptr) {
        return;
    }
    const QString previousName = phoneName_;
    client_->deleteLater();
    client_ = nullptr;
    authenticated_ = false;
    phoneName_.clear();
    receiveBuffer_.clear();
    remoteBuffer_->setConnected(false);
    emit connectionChanged(false, previousName);
    emit statusChanged(QStringLiteral("手机麦克风已断开"));
}

void PhonePairingServer::readDiscoveryDatagrams()
{
    while (discoverySocket_->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = discoverySocket_->receiveDatagram();
        const QByteArray expected = QByteArrayLiteral("VSP_DISCOVER ")
                                    + pairingCode_.toLatin1();
        if (datagram.data().trimmed() != expected) {
            continue;
        }
        QJsonObject response;
        response.insert(QStringLiteral("protocol"), 1);
        response.insert(QStringLiteral("host"), localAddress_);
        response.insert(QStringLiteral("port"), tcpServer_->serverPort());
        response.insert(QStringLiteral("session"), sessionId_);
        response.insert(QStringLiteral("secret"), sessionSecret_);
        discoverySocket_->writeDatagram(
            QJsonDocument(response).toJson(QJsonDocument::Compact),
            datagram.senderAddress(),
            datagram.senderPort());
    }
}

void PhonePairingServer::generateCredentials()
{
    sessionId_ = randomHex(16);
    sessionSecret_ = randomHex(16);
    pairingCode_ = QStringLiteral("%1")
                       .arg(QRandomGenerator::system()->bounded(1000000),
                            6,
                            10,
                            QLatin1Char('0'));
}

QString PhonePairingServer::chooseLocalIpv4Address() const
{
    QString bestAddress = QStringLiteral("127.0.0.1");
    int bestScore = -1;
    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const QString address = entry.ip().toString();
            if (address.startsWith(QStringLiteral("169.254."))) {
                continue;
            }
            int score = 1;
            if (address.startsWith(QStringLiteral("192.168."))) {
                score += 30;
            } else if (address.startsWith(QStringLiteral("10."))) {
                score += 20;
            } else if (entry.ip().isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")),
                                             12)) {
                score += 10;
            }
            if (interface.type() == QNetworkInterface::Wifi) {
                score += 50;
            } else if (interface.type() == QNetworkInterface::Ethernet) {
                score += 40;
            } else if (interface.type() == QNetworkInterface::Virtual) {
                score -= 100;
            }
            if (score > bestScore) {
                bestScore = score;
                bestAddress = address;
            }
        }
    }
    return bestAddress;
}

bool PhonePairingServer::processHandshakeLine(const QByteArray& line)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        rejectClient(QStringLiteral("手机握手格式无效"));
        return false;
    }
    const QJsonObject object = document.object();
    const bool valid = object.value(QStringLiteral("type")).toString()
                           == QStringLiteral("hello")
                       && object.value(QStringLiteral("protocol")).toInt() == 1
                       && object.value(QStringLiteral("session")).toString().toUpper()
                              == sessionId_
                       && object.value(QStringLiteral("secret")).toString().toUpper()
                              == sessionSecret_;
    if (!valid) {
        rejectClient(QStringLiteral("手机配对凭据不匹配"));
        return false;
    }

    phoneName_ = object.value(QStringLiteral("deviceName")).toString();
    if (phoneName_.isEmpty()) {
        phoneName_ = QStringLiteral("Android 手机");
    }
    authenticated_ = true;
    remoteBuffer_->setConnected(true, 48000);
    client_->write("{\"type\":\"accepted\",\"protocol\":1,\"sampleRate\":48000}\n");
    emit connectionChanged(true, phoneName_);
    emit statusChanged(QStringLiteral("手机麦克风已连接：%1").arg(phoneName_));
    return true;
}

void PhonePairingServer::processFrames()
{
    while (authenticated_ && receiveBuffer_.size() >= 4) {
        const auto* raw = reinterpret_cast<const uchar*>(receiveBuffer_.constData());
        const quint32 bodyLength = qFromBigEndian<quint32>(raw);
        if (bodyLength < 13 || bodyLength > maximumFrameBytes) {
            rejectClient(QStringLiteral("手机音频帧长度无效"));
            return;
        }
        if (receiveBuffer_.size() < static_cast<int>(4 + bodyLength)) {
            return;
        }
        const QByteArray body = receiveBuffer_.mid(4, static_cast<int>(bodyLength));
        receiveBuffer_.remove(0, static_cast<int>(4 + bodyLength));
        if (static_cast<quint8>(body.at(0)) != 1) {
            continue;
        }
        const auto* bodyRaw = reinterpret_cast<const uchar*>(body.constData());
        const quint64 firstFrame = qFromBigEndian<quint64>(bodyRaw + 1);
        const quint32 sampleRate = qFromBigEndian<quint32>(bodyRaw + 9);
        const int pcmBytes = body.size() - 13;
        if (sampleRate < 8000 || sampleRate > 192000 || pcmBytes <= 0
            || (pcmBytes % 2) != 0) {
            continue;
        }
        std::vector<float> samples(static_cast<std::size_t>(pcmBytes / 2));
        double energy = 0.0;
        const uchar* pcm = bodyRaw + 13;
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const quint16 rawSample = static_cast<quint16>(pcm[index * 2])
                                      | (static_cast<quint16>(pcm[index * 2 + 1]) << 8);
            const auto signedSample = static_cast<qint16>(rawSample);
            const float value = static_cast<float>(signedSample) / 32768.0F;
            samples[index] = value;
            energy += static_cast<double>(value) * value;
        }
        remoteBuffer_->append(firstFrame, sampleRate, samples);
        const double rms = std::sqrt(energy / samples.size());
        emit microphoneLevelChanged(rms > 0.000001
                                        ? 20.0 * std::log10(rms)
                                        : -120.0);
    }
}

void PhonePairingServer::rejectClient(const QString& reason)
{
    emit statusChanged(reason);
    if (client_ != nullptr) {
        QJsonObject object;
        object.insert(QStringLiteral("type"), QStringLiteral("error"));
        object.insert(QStringLiteral("message"), reason);
        client_->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
        client_->disconnectFromHost();
    }
}
