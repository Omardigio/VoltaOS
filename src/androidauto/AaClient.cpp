#include "AaClient.h"
#include "AaUsb.h"

#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QTcpSocket>

static constexpr quint16 kWirelessPort = 5288;     // the port Android Auto wireless connects to

AaClient::AaClient(QObject *parent) : QObject(parent)
{
    m_session = new AaSession;
    m_session->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_session, &QObject::deleteLater);

    // session -> app (the receiver lives in the main thread: the connections are queued)
    connect(m_session, &AaSession::stateChanged, this, [this](const QString &s, const QString &d) {
        if (s == QLatin1String("connecting")) emit status(QStringLiteral("connecting"), d);
        else if (s == QLatin1String("running")) emit status(QStringLiteral("running"), QString());
        else if (s == QLatin1String("error")) emit status(QStringLiteral("error"), d);
        else if (s == QLatin1String("ended")) emit status(QStringLiteral("searching"), QString());
    });
    connect(m_session, &AaSession::phoneName, this, &AaClient::phoneName);
    connect(m_session, &AaSession::videoActive, this, &AaClient::videoActive);
    connect(m_session, &AaSession::videoSize, this, &AaClient::videoSize);
    connect(m_session, &AaSession::audioPcm, this, &AaClient::audioPcm);
    connect(m_session, &AaSession::audioStop, this, &AaClient::audioStop);
    connect(m_session, &AaSession::microphone, this, &AaClient::microphone);

    // video: straight in the worker thread, so decoding never waits for the interface
    connect(m_session, &AaSession::videoFrame, m_session, [this](const QByteArray &b) { if (onVideo) onVideo(b); },
            Qt::DirectConnection);
    // bytes out of the session -> transport (worker thread)
    connect(m_session, &AaSession::transmit, m_session, [this](const QByteArray &b) {
        if (m_mode == Usb) { if (m_usb) m_usb->write(b); }
        else if (m_sock && m_sock->state() == QAbstractSocket::ConnectedState) m_sock->write(b);
    }, Qt::DirectConnection);
    // the session ended (phone said goodbye, error): drop the TCP connection so it can come back
    connect(m_session, &AaSession::finished, m_session, [this](const QString &) {
        if (m_mode == Wireless && m_sock) m_sock->disconnectFromHost();
    }, Qt::DirectConnection);

    m_thread.start();
}

AaClient::~AaClient()
{
    stop();
    m_thread.quit();
    m_thread.wait(3000);
}

void AaClient::start(Mode mode, const AaSession::Config &cfg)
{
    if (m_running) stop();
    m_mode = mode;
    m_running = true;
    QMetaObject::invokeMethod(m_session, [this, cfg] { m_session->setConfig(cfg); }, Qt::BlockingQueuedConnection);
    emit status(QStringLiteral("searching"), QString());
    if (mode == Usb) startUsb(); else startTcp();
}

void AaClient::startUsb()
{
    m_usb = std::make_unique<AaUsb>();
    m_usb->onConnected = [this] {
        QMetaObject::invokeMethod(m_session, [this] { m_session->start(); }, Qt::QueuedConnection);
    };
    m_usb->onData = [this](const QByteArray &d) {
        QMetaObject::invokeMethod(m_session, [this, d] { m_session->onBytes(d); }, Qt::QueuedConnection);
    };
    m_usb->onDisconnected = [this](const QString &why) {
        QMetaObject::invokeMethod(m_session, [this, why] { m_session->stop(why); }, Qt::QueuedConnection);
    };
    m_usb->onStatus = [this](const QString &what, const QString &detail) {
        if (what == QLatin1String("error")) emit status(QStringLiteral("error"), detail);
        else emit status(what, detail);
    };
    m_usb->start();
}

void AaClient::startTcp()
{
    QMetaObject::invokeMethod(m_session, [this] {
        m_server = new QTcpServer(m_session);
        connect(m_server, &QTcpServer::newConnection, m_session, [this] {
            while (QTcpSocket *s = m_server->nextPendingConnection()) {
                if (m_sock && m_sock->state() == QAbstractSocket::ConnectedState) { s->close(); s->deleteLater(); continue; }
                m_sock = s;
                s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                qInfo() << "[AA/TCP] phone connected from" << s->peerAddress().toString();
                connect(s, &QTcpSocket::readyRead, m_session, [this, s] { m_session->onBytes(s->readAll()); });
                connect(s, &QTcpSocket::disconnected, m_session, [this, s] {
                    qInfo() << "[AA/TCP] phone disconnected";
                    m_session->stop(QStringLiteral("connection closed"));
                    s->deleteLater();
                });
                m_session->start();
            }
        });
        if (!m_server->listen(QHostAddress::AnyIPv4, kWirelessPort)) {
            const QString why = QStringLiteral("port %1 busy: %2").arg(kWirelessPort).arg(m_server->errorString());
            qWarning().noquote() << "[AA/TCP]" << why;
            QMetaObject::invokeMethod(this, [this, why] { emit status(QStringLiteral("error"), why); }, Qt::QueuedConnection);
        } else {
            qInfo() << "[AA/TCP] waiting for the phone on port" << kWirelessPort;
        }
    }, Qt::BlockingQueuedConnection);
}

void AaClient::closeTransport()
{
    if (m_usb) { m_usb->stop(); m_usb.reset(); }
    if (m_server || m_sock) {
        QMetaObject::invokeMethod(m_session, [this] {
            if (m_sock) { m_sock->disconnect(); m_sock->close(); m_sock->deleteLater(); m_sock = nullptr; }
            if (m_server) { m_server->close(); m_server->deleteLater(); m_server = nullptr; }
        }, Qt::BlockingQueuedConnection);
    }
}

void AaClient::stop()
{
    if (!m_running) return;
    m_running = false;
    QMetaObject::invokeMethod(m_session, [this] { m_session->stop(QStringLiteral("stopped")); }, Qt::BlockingQueuedConnection);
    closeTransport();
}

void AaClient::touch(int a, double x, double y)
{ QMetaObject::invokeMethod(m_session, [=] { m_session->touch(a, x, y); }, Qt::QueuedConnection); }
void AaClient::key(int k, bool d)
{ QMetaObject::invokeMethod(m_session, [=] { m_session->key(k, d); }, Qt::QueuedConnection); }
void AaClient::setNight(bool n)
{ QMetaObject::invokeMethod(m_session, [=] { m_session->setNight(n); }, Qt::QueuedConnection); }
void AaClient::micData(const QByteArray &pcm)
{ QMetaObject::invokeMethod(m_session, [=] { m_session->micData(pcm); }, Qt::QueuedConnection); }
