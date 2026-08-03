#pragma once

#include "remote_microphone_buffer.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <memory>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

class PhonePairingServer : public QObject
{
    Q_OBJECT

public:
    explicit PhonePairingServer(QObject* parent = nullptr,
                                quint16 discoveryPort = 39741);
    ~PhonePairingServer() override;

    bool start(QString* errorMessage = nullptr);
    void stop();
    void resetPairing();

    QString pairingCode() const;
    QString pairingPayload() const;
    QString localAddress() const;
    QStringList localIpv4Addresses() const;
    bool setLocalAddress(const QString& address);
    quint16 serverPort() const;
    quint16 discoveryPort() const;
    bool phoneConnected() const;
    QString connectedPhoneName() const;
    std::shared_ptr<RemoteMicrophoneBuffer> remoteBuffer() const;

signals:
    void statusChanged(const QString& message);
    void connectionChanged(bool connected, const QString& phoneName);
    void microphoneLevelChanged(double levelDbfs);

private slots:
    void acceptConnection();
    void readClientData();
    void clientDisconnected();
    void readDiscoveryDatagrams();

private:
    void generateCredentials();
    QString chooseLocalIpv4Address() const;
    bool processHandshakeLine(const QByteArray& line);
    void processFrames();
    void rejectClient(const QString& reason);

    QTcpServer* tcpServer_ = nullptr;
    QUdpSocket* discoverySocket_ = nullptr;
    QTcpSocket* client_ = nullptr;
    QByteArray receiveBuffer_;
    bool authenticated_ = false;
    QString sessionId_;
    QString sessionSecret_;
    QString pairingCode_;
    QString localAddress_;
    QString phoneName_;
    quint16 discoveryPort_ = 39741;
    std::shared_ptr<RemoteMicrophoneBuffer> remoteBuffer_;
};
