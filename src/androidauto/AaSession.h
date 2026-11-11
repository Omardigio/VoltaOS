#pragma once
// AaSession — the head unit side of one Android Auto session, independent of how the bytes travel
// (USB cable or Wi-Fi/TCP: both just feed onBytes() and send what comes out of transmit()).
//
//   version -> TLS handshake -> service discovery -> channels (video, 3 audio, microphone, input,
//   sensors). Video arrives as H.264, audio as PCM; touches and keys go the other way.
//
// Everything runs in the thread the object lives in; the rest of the app talks to it only through
// signals and slots (queued across threads).
#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QTimer>
#include <memory>

#include "AaFraming.h"
#include "AaTls.h"

class AaSession : public QObject
{
    Q_OBJECT
public:
    struct Config {
        int videoW = 1280, videoH = 720;   // 800x480, 1280x720 or 1920x1080
        int fps = 30;                       // 30 or 60
        int dpi = 160;
        int theme = 0;                      // 0 automatic, 1 light, 2 dark
        bool night = false;
        QString name = QStringLiteral("Volta OS");
        QByteArray certPem, keyPem;         // head unit certificate
    };
    // channel numbers we give to our services (they are our choice, sent in the service discovery)
    enum Channel : quint8 { Control = 0, Sensor = 1, Video = 2, AudioMedia = 3, AudioGuidance = 4,
                            AudioSystem = 5, Microphone = 6, Input = 7 };

    explicit AaSession(QObject *parent = nullptr);
    void setConfig(const Config &c) { m_cfg = c; }
    void setServerRoleForTests(bool on) { m_serverTls = on; }

public slots:
    /// Transport connected: starts the version exchange.
    void start();
    /// Bytes received from the transport.
    void onBytes(const QByteArray &bytes);
    /// Touch: action 0 down, 1 up, 2 move; x, y from 0 to 1 in the video area.
    void touch(int action, double x, double y);
    /// Phone keys (Android key codes): 3 home, 4 back...
    void key(int keycode, bool down);
    void setNight(bool night);
    /// PCM from the microphone (16 kHz, 16 bit, mono), when the phone asked for it.
    void micData(const QByteArray &pcm);
    void stop(const QString &reason = QString());

signals:
    /// Bytes to send over the transport.
    void transmit(const QByteArray &bytes);
    // idle | connecting | running | ended | error
    void stateChanged(const QString &state, const QString &detail);
    void phoneName(const QString &name);
    void videoFrame(const QByteArray &h264);
    void videoSize(int w, int h);
    void videoActive(bool on);
    void audioPcm(int stream, int rate, int channels, const QByteArray &pcm);
    void audioStop(int stream);
    void microphone(bool open);
    void finished(const QString &reason);

private:
    // ── sending ──
    void sendMessage(quint8 channel, bool control, bool encrypted, quint16 id, const QByteArray &body = QByteArray());
    void sendControl(quint16 id, const QByteArray &body = QByteArray()) { sendMessage(Control, false, m_secure, id, body); }
    template <class T> void sendPb(quint8 ch, bool control, quint16 id, const T &msg);
    // ── receiving ──
    void handleFrame(const aa::Frame &f);
    void dispatch(quint8 channel, bool control, const QByteArray &msg);
    void onControl(quint16 id, const QByteArray &body);
    void onChannelControl(quint8 ch, quint16 id, const QByteArray &body);
    void onMedia(quint8 ch, quint16 id, const QByteArray &body);
    void onInput(quint16 id, const QByteArray &body);
    void onSensor(quint16 id, const QByteArray &body);
    void onMicrophone(quint16 id, const QByteArray &body);
    void handshakeStep(const QByteArray &in);
    void serviceDiscovery(const QByteArray &body);
    void sendSensorBatch(int type);
    void fail(const QString &why);
    static bool isAudio(quint8 ch) { return ch == AudioMedia || ch == AudioGuidance || ch == AudioSystem; }
    static int streamOf(quint8 ch) { return ch == AudioMedia ? 1 : ch == AudioGuidance ? 2 : 3; }

    Config m_cfg;
    bool m_serverTls = false;
    std::unique_ptr<AaTls> m_tls;
    aa::Deframer m_deframer;
    QByteArray m_partial[256];               // message being rebuilt, per channel
    bool m_partialControl[256] = {};
    bool m_secure = false;                   // TLS established: messages are encrypted
    bool m_ended = false;
    bool m_started = false;
    QString m_state = QStringLiteral("idle");
    QTimer m_ping{this};                     // child of the session: it moves to its thread with it
    QElapsedTimer m_clock;
    QHash<quint8, int> m_mediaSession;       // media session id per channel (from START)
    QHash<quint8, bool> m_open;              // channel open
    int m_sensorsOn = 0;                     // bit mask of sensors the phone started
    bool m_videoOn = false;
    bool m_micOpen = false;
    int m_micSession = 0;
    int m_lastVideoW = 0, m_lastVideoH = 0;
    qint64 m_vFrames = 0, m_vBytes = 0, m_aBytes = 0, m_touches = 0;     // for the log
};
