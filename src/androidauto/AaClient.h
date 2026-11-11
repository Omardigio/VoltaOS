#pragma once
// AaClient — native Android Auto (no dongle) as the rest of the app sees it.
// It owns a worker thread with the AaSession and a transport:
//   Usb      the phone is plugged in with a cable (AaUsb, accessory mode)
//   Wireless the phone connects over Wi-Fi to TCP port 5288 (the Bluetooth/Wi-Fi handshake that
//            makes it do so is separate: see AaWireless)
// Video and audio come out through callbacks/signals; touches and keys go in through the slots.
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QThread>
#include <functional>
#include <memory>

#include "AaSession.h"

class AaUsb;
class QTcpSocket;

class AaClient : public QObject
{
    Q_OBJECT
public:
    enum Mode { Usb, Wireless };
    explicit AaClient(QObject *parent = nullptr);
    ~AaClient() override;

    /// Starts (or restarts) with this configuration.
    void start(Mode mode, const AaSession::Config &cfg);
    void stop();
    bool running() const { return m_running; }
    Mode mode() const { return m_mode; }

    /// H.264 data: called IN THE WORKER THREAD, set it before start().
    std::function<void(const QByteArray &)> onVideo;

public slots:
    void touch(int action, double x, double y);
    void key(int keycode, bool down);
    void setNight(bool night);
    void micData(const QByteArray &pcm);

signals:
    // searching | permission | probing | connecting | running | error   (+ detail text)
    void status(const QString &state, const QString &detail);
    void phoneName(const QString &name);
    void videoActive(bool on);
    void videoSize(int w, int h);
    void audioPcm(int stream, int rate, int channels, const QByteArray &pcm);
    void audioStop(int stream);
    void microphone(bool open);

private:
    void startUsb();
    void startTcp();
    void closeTransport();

    QThread m_thread;
    AaSession *m_session = nullptr;                 // lives in m_thread
    std::unique_ptr<AaUsb> m_usb;
    QTcpServer *m_server = nullptr;                 // lives in m_thread
    QPointer<QTcpSocket> m_sock;                    // lives in m_thread
    Mode m_mode = Usb;
    bool m_running = false;
};
