#pragma once
// CarPlay and Android Auto through the Carlinkit dongle (USB 1314:1520 / 1521).
//
// The dongle has the Apple authentication chip and it talks to the phone itself (Bluetooth +
// Wi-Fi). To the computer, over USB, it only sends the H.264 video, the audio and the status, and
// receives touches: this is the "packet" protocol between dongle and computer.
//
//   packet = 16-byte header (little endian)
//       0x55AA55AA | data length | type | type XOR 0xFFFFFFFF
//     + data
//
// All the USB work runs in its own thread; towards QML only properties and signals.

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QMutex>
#include <QHash>
#include <atomic>
#include <thread>

#include "CarVideoDecoder.h"

struct libusb_context;
struct libusb_device_handle;
class QVideoSink;
class CarAudioOut;
class CarMic;
class AaClient;

class CarlinkDongle : public QObject {
    Q_OBJECT
    // Looks for the dongle only when the app is open (or VOLTA_CARLINK_AUTO=1)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // off | searching | starting | waiting | connecting | running | error
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY stateChanged)
    // "CarPlay" | "Android Auto" | ""
    // How the phone connects: "dongle" (Carlinkit, CarPlay + Android Auto), "usb" (Android Auto
    // native, phone plugged in with a cable) or "wireless" (Android Auto native over Wi-Fi)
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY settingsChanged)
    Q_PROPERTY(QString phoneKind READ phoneKind NOTIFY stateChanged)
    Q_PROPERTY(QString phoneName READ phoneName NOTIFY stateChanged)
    Q_PROPERTY(bool videoActive READ videoActive NOTIFY videoActiveChanged)
    Q_PROPERTY(QObject *videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(QSize videoSize READ videoSize NOTIFY videoSizeChanged)
    // Settings (saved to disk). resolution: 1 = 480p, 2 = 720p, 3 = 1080p;
    // dpi: 0 = automatic; theme: "light" | "dark" | "auto" (the phone decides) | "ui" (like the interface);
    // fitSmall / fitFull: "fill" (stretch) | "fit" (bars), in the small panel / in full screen
    Q_PROPERTY(int resolution READ resolution WRITE setResolution NOTIFY settingsChanged)
    Q_PROPERTY(int dpi READ dpi WRITE setDpi NOTIFY settingsChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY settingsChanged)
    Q_PROPERTY(QString fitSmall READ fitSmall WRITE setFitSmall NOTIFY settingsChanged)
    Q_PROPERTY(QString fitFull READ fitFull WRITE setFitFull NOTIFY settingsChanged)
    // light theme of the interface (set by the view), used with theme = "ui"
    Q_PROPERTY(bool uiLight READ uiLight WRITE setUiLight NOTIFY settingsChanged)

public:
    explicit CarlinkDongle(QObject *parent = nullptr);
    ~CarlinkDongle() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    QString state() const { return m_state; }
    QString detail() const { return m_detail; }
    QString source() const { return m_source; }
    void setSource(const QString &s);
    QString phoneKind() const { return m_phoneKind; }
    QString phoneName() const { return m_phoneName; }
    bool videoActive() const { return m_videoActive; }
    QObject *videoSink() const;
    void setVideoSink(QObject *sink);
    QSize videoSize() const { return m_videoSize; }
    int resolution() const { return m_resolution; }
    void setResolution(int r);
    int dpi() const { return m_dpi; }
    void setDpi(int d);
    QString theme() const { return m_theme; }
    void setTheme(const QString &t);
    QString fitSmall() const { return m_fitSmall; }
    void setFitSmall(const QString &f);
    QString fitFull() const { return m_fitFull; }
    void setFitFull(const QString &f);
    bool uiLight() const { return m_uiLight; }
    void setUiLight(bool l);

    // Size of the area where it is shown: the dongle already sends the video at this size
    // (valid from the next connection)
    Q_INVOKABLE void setViewSize(int w, int h);
    // Touch: action 0 down, 1 up, 2 move; x and y from 0 to 1 in the video area
    Q_INVOKABLE void touch(int action, double x, double y);
    // Phone buttons: "home", "back", "siri" ...
    Q_INVOKABLE void key(const QString &name);
    Q_INVOKABLE void restart();

signals:
    void enabledChanged();
    void stateChanged();
    void videoActiveChanged();
    void videoSinkChanged();
    void videoSizeChanged();
    void settingsChanged();
    // the phone started/stopped playing music: to stop the other sources
    void mediaAudioActiveChanged(bool active);

private:
    // ── thread USB ──
    void usbLoop();
    libusb_device_handle *openDongle();
    void runSession(libusb_device_handle *h, unsigned char epIn, unsigned char epOut);
    void initDongle();

    // ── pacchetti ──
    bool sendPacket(quint32 type, const QByteArray &payload = QByteArray());
    void sendFile(const QString &path, const QByteArray &content);
    void sendCommand(quint32 cmd);
    void sendNumberFile(const QString &path, quint32 value);
    void onData(const QByteArray &chunk);
    void handlePacket(quint32 type, const QByteArray &data);
    void handleAudio(const QByteArray &data);

    void setState(const QString &state, const QString &detail = QString());
    void setVideoActive(bool on);
    void endSession();

    // stato pubblico (thread principale)
    bool m_enabled = false;
    QString m_state = QStringLiteral("off");
    QString m_detail;
    QString m_phoneKind;
    QString m_phoneName;
    bool m_videoActive = false;
    QSize m_videoSize{1280, 720};
    int m_resolution = 3;
    int m_dpi = 0;
    QString m_theme = QStringLiteral("ui");
    QString m_fitSmall = QStringLiteral("fit");
    QString m_fitFull = QStringLiteral("fill");
    bool m_uiLight = false;
    QPointer<QVideoSink> m_sink;

    // thread USB
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_wantEnabled{false};
    std::atomic<bool> m_dropSession{false};
    std::atomic<int> m_viewW{1280}, m_viewH{720};
    std::atomic<int> m_nightValue{1};      // 0 giorno, 1 notte, 2 automatico
    std::atomic<int> m_resolutionA{3};
    std::atomic<int> m_dpiA{0};
    void loadSettings();
    void saveSettings();
    // ── native Android Auto (src/androidauto) ──
    void startNative();
    void stopNative();
    void applySource();
    void handleNativeAudio(int stream, int rate, int channels, const QByteArray &pcm);
    QString m_source = QStringLiteral("dongle");
    AaClient *m_native = nullptr;
    void applyNight();
    void sendNight();
    libusb_context *m_usb = nullptr;
    libusb_device_handle *m_dev = nullptr;
    unsigned char m_epOut = 0;
    QMutex m_txLock;

    // sessione
    QByteArray m_rx;
    std::atomic<bool> m_sessionUp{false};
    bool m_videoFlag = false;
    bool m_greeted = false;
    int m_reqW = 1280, m_reqH = 720;
    quint32 m_phoneType = 0;
    qint64 m_lastKeyframeMs = 0;

    CarVideoDecoder m_decoder;
    QHash<int, CarAudioOut *> m_audio;     // per audio type; they live in the main thread
    CarMic *m_mic = nullptr;
    bool m_mediaPlaying = false;
};
