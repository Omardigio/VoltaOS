#include <QCoreApplication>
#include "CarlinkDongle.h"
#include "CarAudio.h"
#include "AaClient.h"
#include "AaCert.h"
#include <QStandardPaths>

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QMetaObject>
#include <QVideoSink>
#include <QtEndian>
#include <chrono>

#include <libusb-1.0/libusb.h>

namespace {

constexpr quint32 kMagic = 0x55AA55AA;

// dongle ⇄ computer packet types
enum Pkt : quint32 {
    OPEN = 0x01, PLUGGED = 0x02, PHASE = 0x03, UNPLUGGED = 0x04, TOUCH = 0x05,
    VIDEO = 0x06, AUDIO = 0x07, COMMAND = 0x08, LOGO = 0x09,
    BT_ADDR = 0x0A, BT_PIN = 0x0C, BT_NAME = 0x0D, WIFI_NAME = 0x0E, BT_LIST = 0x12,
    MANUFACTURER = 0x14, CLOSE = 0x15, MULTITOUCH = 0x17, BOX_SETTINGS = 0x19,
    MEDIA_DATA = 0x2A, SEND_FILE = 0x99, HEARTBEAT = 0xAA, SW_VERSION = 0xCC
};

// comandi (pacchetto COMMAND)
enum Cmd : quint32 {
    CMD_RECORD_START = 1, CMD_RECORD_STOP = 2, CMD_REQUEST_UI = 3, CMD_SIRI = 5,
    CMD_MIC_CAR = 7, CMD_KEYFRAME = 12, CMD_WIFI_24 = 24, CMD_WIFI_5 = 25,
    CMD_LEFT = 100, CMD_RIGHT = 101, CMD_SELECT_DOWN = 104, CMD_SELECT_UP = 105,
    CMD_BACK = 106, CMD_DOWN = 114, CMD_HOME = 200, CMD_PLAY_PAUSE = 201,
    CMD_NEXT = 202, CMD_PREV = 203,
    CMD_NIGHT_ON = 16, CMD_NIGHT_OFF = 17
};

QByteArray le32(quint32 v) { QByteArray b(4, 0); qToLittleEndian<quint32>(v, b.data()); return b; }
QByteArray lef32(float f) { quint32 u; memcpy(&u, &f, 4); return le32(u); }

// audio format from the dongle's "decode type"
bool audioFormat(quint32 decodeType, int *rate, int *channels)
{
    switch (decodeType) {
    case 1: case 2: *rate = 44100; *channels = 2; return true;
    case 3:         *rate = 8000;  *channels = 1; return true;
    case 4:         *rate = 48000; *channels = 2; return true;
    case 5:         *rate = 16000; *channels = 1; return true;
    case 6:         *rate = 24000; *channels = 1; return true;
    case 7:         *rate = 16000; *channels = 2; return true;
    default: return false;
    }
}

} // namespace

CarlinkDongle::CarlinkDongle(QObject *parent) : QObject(parent)
{
    loadSettings();
    m_mic = new CarMic(this);
    m_mic->onPcm = [this](const QByteArray &pcm) {
        if (m_native && m_native->running()) { m_native->micData(pcm); return; }
        if (!m_sessionUp) return;
        // voice to the phone: type 5 (16 kHz mono), volume 0, "input" channel 3
        sendPacket(AUDIO, le32(5) + lef32(0.0f) + le32(3) + pcm);
    };
    m_thread = std::thread([this] { usbLoop(); });
    if (qEnvironmentVariableIntValue("VOLTA_CARLINK_AUTO") == 1) setEnabled(true);
}

CarlinkDongle::~CarlinkDongle()
{
    m_stop = true;
    m_dropSession = true;
    if (m_thread.joinable()) m_thread.join();
}

void CarlinkDongle::setEnabled(bool on)
{
    if (m_enabled == on) return;
    m_enabled = on;
    const bool native = m_source != QLatin1String("dongle");
    m_wantEnabled = on && !native;
    if (!on || native) m_dropSession = true;
    emit enabledChanged();
    setState(on ? QStringLiteral("searching") : QStringLiteral("off"));
    if (native) { if (on) startNative(); else stopNative(); }
}

QObject *CarlinkDongle::videoSink() const { return m_sink.data(); }

void CarlinkDongle::setVideoSink(QObject *sink)
{
    auto *s = qobject_cast<QVideoSink *>(sink);
    if (m_sink == s) return;
    m_sink = s;
    m_decoder.setSink(s);
    emit videoSinkChanged();
}

// ── impostazioni salvate ──
void CarlinkDongle::loadSettings()
{
    QSettings q(QStringLiteral("VoltaUI"), QStringLiteral("carlink"));
    m_resolution = qBound(1, q.value(QStringLiteral("resolution"), 3).toInt(), 3);
    m_dpi = q.value(QStringLiteral("dpi"), 0).toInt();
    m_theme = q.value(QStringLiteral("theme"), QStringLiteral("ui")).toString();
    m_fitSmall = q.value(QStringLiteral("fitSmall"), QStringLiteral("fit")).toString();
    m_fitFull = q.value(QStringLiteral("fitFull"), QStringLiteral("fill")).toString();
    if (m_fitSmall != QLatin1String("fill")) m_fitSmall = QStringLiteral("fit");   // "ritaglia" tolto
    if (m_fitFull != QLatin1String("fit")) m_fitFull = QStringLiteral("fill");
    QString src = q.value(QStringLiteral("source"), QStringLiteral("dongle")).toString();
    if (qEnvironmentVariableIsSet("VOLTA_AA_SOURCE")) src = qEnvironmentVariable("VOLTA_AA_SOURCE");   // tests
    m_source = (src == QLatin1String("usb") || src == QLatin1String("wireless")) ? src : QStringLiteral("dongle");
    m_resolutionA = m_resolution;
    m_dpiA = m_dpi;
    applyNight();
}

void CarlinkDongle::saveSettings()
{
    QSettings q(QStringLiteral("VoltaUI"), QStringLiteral("carlink"));
    q.setValue(QStringLiteral("resolution"), m_resolution);
    q.setValue(QStringLiteral("dpi"), m_dpi);
    q.setValue(QStringLiteral("theme"), m_theme);
    q.setValue(QStringLiteral("fitSmall"), m_fitSmall);
    q.setValue(QStringLiteral("fitFull"), m_fitFull);
    q.setValue(QStringLiteral("source"), m_source);
}

void CarlinkDongle::applyNight()
{
    const int v = m_theme == QLatin1String("light") ? 0
                : m_theme == QLatin1String("dark")  ? 1
                : m_theme == QLatin1String("auto")  ? 2
                : (m_uiLight ? 0 : 1);
    if (m_nightValue.exchange(v) != v) {
        if (m_sessionUp) sendNight();
        if (m_native && m_native->running()) m_native->setNight(v == 1);
    }
}

// The theme is changed on the fly with a command (night/day); the file is used at startup and for "auto"
void CarlinkDongle::sendNight()
{
    const int v = m_nightValue.load();
    sendNumberFile(QStringLiteral("/tmp/night_mode"), quint32(v));
    if (v == 0) sendCommand(CMD_NIGHT_OFF);
    else if (v == 1) sendCommand(CMD_NIGHT_ON);
}

void CarlinkDongle::setResolution(int r)
{
    r = qBound(1, r, 3);
    if (m_resolution == r) return;
    m_resolution = r; m_resolutionA = r;
    saveSettings(); emit settingsChanged();
    if (m_native && m_native->running()) { stopNative(); startNative(); }
}
void CarlinkDongle::setDpi(int d)
{
    if (m_dpi == d) return;
    m_dpi = d; m_dpiA = d;
    saveSettings(); emit settingsChanged();
    if (m_native && m_native->running()) { stopNative(); startNative(); }
}
void CarlinkDongle::setTheme(const QString &t)
{
    if (m_theme == t) return;
    m_theme = t; applyNight();
    saveSettings(); emit settingsChanged();
}
void CarlinkDongle::setFitSmall(const QString &f)
{
    if (m_fitSmall == f) return;
    m_fitSmall = f; saveSettings(); emit settingsChanged();
}
void CarlinkDongle::setFitFull(const QString &f)
{
    if (m_fitFull == f) return;
    m_fitFull = f; saveSettings(); emit settingsChanged();
}
void CarlinkDongle::setUiLight(bool l)
{
    if (m_uiLight == l) return;
    m_uiLight = l; applyNight(); emit settingsChanged();
}

void CarlinkDongle::setViewSize(int w, int h)
{
    if (w > 50 && h > 50) { m_viewW = w; m_viewH = h; }
}

void CarlinkDongle::restart()
{
    m_dropSession = true;
    if (m_source != QLatin1String("dongle") && m_enabled) { stopNative(); startNative(); }
}

void CarlinkDongle::key(const QString &name)
{
    if (m_native && m_native->running()) {                  // Android key codes
        static const QHash<QString, int> codes = {
            {QStringLiteral("home"), 3}, {QStringLiteral("back"), 4}, {QStringLiteral("playpause"), 85},
            {QStringLiteral("next"), 87}, {QStringLiteral("prev"), 88}, {QStringLiteral("siri"), 84}};
        if (codes.contains(name)) { m_native->key(codes.value(name), true); m_native->key(codes.value(name), false); }
        return;
    }
    if (!m_sessionUp) return;
    static const QHash<QString, quint32> map = {
        {QStringLiteral("home"), CMD_HOME}, {QStringLiteral("back"), CMD_BACK},
        {QStringLiteral("siri"), CMD_SIRI}, {QStringLiteral("playpause"), CMD_PLAY_PAUSE},
        {QStringLiteral("next"), CMD_NEXT}, {QStringLiteral("prev"), CMD_PREV},
        {QStringLiteral("left"), CMD_LEFT}, {QStringLiteral("right"), CMD_RIGHT},
        {QStringLiteral("down"), CMD_DOWN}, {QStringLiteral("select"), CMD_SELECT_DOWN},
    };
    if (map.contains(name)) sendCommand(map.value(name));
}

void CarlinkDongle::setState(const QString &state, const QString &detail)
{
    QMetaObject::invokeMethod(this, [this, state, detail] {
        if (m_state == state && m_detail == detail) return;
        m_state = state;
        m_detail = detail;
        emit stateChanged();
    }, Qt::QueuedConnection);
}

void CarlinkDongle::setVideoActive(bool on)
{
    QMetaObject::invokeMethod(this, [this, on] {
        if (m_videoActive == on) return;
        m_videoActive = on;
        emit videoActiveChanged();
    }, Qt::QueuedConnection);
}

// ─────────────────────────────── USB ────────────────────────────────────────

void CarlinkDongle::usbLoop()
{
    if (libusb_init(&m_usb) != 0) {
        setState(QStringLiteral("error"), QCoreApplication::translate("Msg", "USB not available"));
        return;
    }
    bool reportedPerm = false;
    while (!m_stop) {
        if (!m_wantEnabled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }
        m_dropSession = false;
        libusb_device_handle *h = openDongle();
        if (!h) {
            if (!reportedPerm) setState(QStringLiteral("searching"));
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        unsigned char epIn = 0, epOut = 0;
        libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_active_config_descriptor(libusb_get_device(h), &cfg) == 0) {
            const libusb_interface_descriptor &alt = cfg->interface[0].altsetting[0];
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const auto &ep = alt.endpoint[e];
                if ((ep.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) epIn = ep.bEndpointAddress;
                else epOut = ep.bEndpointAddress;
            }
            libusb_free_config_descriptor(cfg);
        }
        if (epIn && epOut && libusb_claim_interface(h, 0) == 0) {
            runSession(h, epIn, epOut);
            libusb_release_interface(h, 0);
        } else {
            setState(QStringLiteral("error"), QCoreApplication::translate("Msg", "The dongle is not responding"));
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        libusb_close(h);
    }
    libusb_exit(m_usb);
    m_usb = nullptr;
}

libusb_device_handle *CarlinkDongle::openDongle()
{
    libusb_device **list = nullptr;
    const ssize_t n = libusb_get_device_list(m_usb, &list);
    libusb_device_handle *h = nullptr;
    for (ssize_t k = 0; k < n && !h; ++k) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[k], &d) != 0) continue;
        if (d.idVendor != 0x1314 || (d.idProduct != 0x1520 && d.idProduct != 0x1521)) continue;
        if (libusb_open(list[k], &h) != 0) {
            setState(QStringLiteral("error"), QCoreApplication::translate("Msg", "Missing USB permissions for the dongle"));
            h = nullptr;
        }
    }
    libusb_free_device_list(list, 1);
    if (h) libusb_set_auto_detach_kernel_driver(h, 1);
    return h;
}

void CarlinkDongle::runSession(libusb_device_handle *h, unsigned char epIn, unsigned char epOut)
{
    {
        QMutexLocker tx(&m_txLock);
        m_dev = h;
        m_epOut = epOut;
    }
    m_rx.clear();
    m_videoFlag = false;
    m_greeted = false;
    m_phoneType = 0;
    m_decoder.reset();
    // Video size: Android Auto only accepts 800x480, 1280x720 or 1920x1080 (16:9),
    // any other size is drawn badly. The panel is then filled in QML.
    // VOLTA_CARLINK_RES=1280x720 (or 800x480) for a slow computer.
    switch (m_resolutionA.load()) {
    case 1:  m_reqW = 800;  m_reqH = 480;  break;
    case 2:  m_reqW = 1280; m_reqH = 720;  break;
    default: m_reqW = 1920; m_reqH = 1080; break;
    }
    const QString res = qEnvironmentVariable("VOLTA_CARLINK_RES");     // test from the terminal
    if (res == QLatin1String("1280x720")) { m_reqW = 1280; m_reqH = 720; }
    else if (res == QLatin1String("800x480")) { m_reqW = 800; m_reqH = 480; }
    m_sessionUp = true;
    setState(QStringLiteral("starting"));
    qInfo() << "[Carlink] dongle open, video requested" << m_reqW << "x" << m_reqH;
    initDongle();

    auto now = [] { return std::chrono::steady_clock::now(); };
    auto lastBeat = now();
    auto lastKey = now();
    QByteArray buf(0x100000, 0);
    while (!m_stop && !m_dropSession && m_wantEnabled) {
        if (now() - lastBeat > std::chrono::seconds(2)) {
            lastBeat = now();
            if (!sendPacket(HEARTBEAT)) break;
        }
        // until the first frame arrives a full frame is requested now and then
        if (m_phoneType && !m_videoFlag && now() - lastKey > std::chrono::seconds(2)) {
            lastKey = now();
            sendCommand(CMD_KEYFRAME);
        }
        int n = 0;
        const int r = libusb_bulk_transfer(h, epIn, (unsigned char *)buf.data(), int(buf.size()), &n, 100);
        if (r == LIBUSB_ERROR_TIMEOUT) { if (n > 0) onData(buf.left(n)); continue; }
        if (r != 0) {
            qInfo() << "[Carlink] dongle scollegato:" << libusb_error_name(r);
            break;
        }
        onData(buf.left(n));
    }
    endSession();
}

void CarlinkDongle::endSession()
{
    m_sessionUp = false;
    {
        QMutexLocker tx(&m_txLock);
        m_dev = nullptr;
    }
    setVideoActive(false);
    QMetaObject::invokeMethod(this, [this] {
        for (CarAudioOut *a : std::as_const(m_audio)) a->stop();
        m_mic->stop();
        if (m_mediaPlaying) { m_mediaPlaying = false; emit mediaAudioActiveChanged(false); }
        m_phoneKind.clear();
        m_phoneName.clear();
    }, Qt::QueuedConnection);
    setState(m_wantEnabled ? QStringLiteral("searching") : QStringLiteral("off"));
}

// ─────────────────────────────── invio ──────────────────────────────────────

bool CarlinkDongle::sendPacket(quint32 type, const QByteArray &payload)
{
    QMutexLocker tx(&m_txLock);
    if (!m_dev) return false;
    QByteArray head = le32(kMagic) + le32(quint32(payload.size())) + le32(type) + le32(type ^ 0xFFFFFFFFu);
    auto write = [this](const QByteArray &d) {
        int done = 0;
        while (done < d.size()) {
            int n = 0;
            const int r = libusb_bulk_transfer(m_dev, m_epOut, (unsigned char *)d.constData() + done,
                                               int(d.size()) - done, &n, 1000);
            if (r != 0 && !(r == LIBUSB_ERROR_TIMEOUT && n > 0)) return false;
            done += n;
        }
        return true;
    };
    if (!write(head)) { m_dropSession = true; return false; }
    if (!payload.isEmpty() && !write(payload)) { m_dropSession = true; return false; }
    return true;
}

void CarlinkDongle::sendFile(const QString &path, const QByteArray &content)
{
    QByteArray name = path.toUtf8();
    name.append('\0');
    sendPacket(SEND_FILE, le32(quint32(name.size())) + name + le32(quint32(content.size())) + content);
}

void CarlinkDongle::sendNumberFile(const QString &path, quint32 value) { sendFile(path, le32(value)); }

void CarlinkDongle::sendCommand(quint32 cmd) { sendPacket(COMMAND, le32(cmd)); }

void CarlinkDongle::initDongle()
{
    // settings read by the dongle before opening
    const int dpi = m_dpiA.load() > 0 ? m_dpiA.load()
                  : (m_reqH >= 1080 ? 240 : m_reqH >= 720 ? 160 : 120);   // like Android Auto's scale
    sendNumberFile(QStringLiteral("/tmp/screen_dpi"), quint32(dpi));
    sendNumberFile(QStringLiteral("/tmp/night_mode"), quint32(m_nightValue.load()));
    sendNumberFile(QStringLiteral("/tmp/hand_drive_mode"), 0);       // guida a sinistra
    sendNumberFile(QStringLiteral("/tmp/charge_mode"), 0);
    sendFile(QStringLiteral("/tmp/box_name"), QByteArrayLiteral("Volta OS"));

    // apertura: larghezza, altezza, fotogrammi/s, formato (5 = H.264), pacchetto massimo,
    // version, mode
    sendPacket(OPEN, le32(quint32(m_reqW)) + le32(quint32(m_reqH)) + le32(60) + le32(5)
                         + le32(49152) + le32(2) + le32(2));

    QJsonObject box;
    box[QStringLiteral("mediaDelay")] = 300;
    box[QStringLiteral("syncTime")] = qint64(QDateTime::currentSecsSinceEpoch());
    box[QStringLiteral("androidAutoSizeW")] = m_reqW;
    box[QStringLiteral("androidAutoSizeH")] = m_reqH;
    box[QStringLiteral("autoConn")] = true;
    box[QStringLiteral("boxName")] = QStringLiteral("Volta OS");
    sendPacket(BOX_SETTINGS, QJsonDocument(box).toJson(QJsonDocument::Compact));

    sendCommand(CMD_MIC_CAR);       // the microphone is the car's (ours)
    sendCommand(CMD_WIFI_5);        // Wi-Fi a 5 GHz
}

// ─────────────────────────────── ricezione ──────────────────────────────────

void CarlinkDongle::onData(const QByteArray &chunk)
{
    m_rx.append(chunk);
    for (;;) {
        if (m_rx.size() < 16) return;
        const auto *p = reinterpret_cast<const uchar *>(m_rx.constData());
        if (qFromLittleEndian<quint32>(p) != kMagic) {      // out of alignment: look for the next one
            const int next = m_rx.indexOf(le32(kMagic), 1);
            m_rx.remove(0, next < 0 ? int(m_rx.size()) : next);
            continue;
        }
        const quint32 len = qFromLittleEndian<quint32>(p + 4);
        const quint32 type = qFromLittleEndian<quint32>(p + 8);
        const quint32 check = qFromLittleEndian<quint32>(p + 12);
        if (check != (type ^ 0xFFFFFFFFu) || len > (8u << 20)) { m_rx.remove(0, 4); continue; }
        if (quint32(m_rx.size()) < 16 + len) return;
        const QByteArray data = m_rx.mid(16, int(len));
        m_rx.remove(0, int(16 + len));
        handlePacket(type, data);
    }
}

void CarlinkDongle::handlePacket(quint32 type, const QByteArray &data)
{
    if (!m_greeted) {            // the dongle answers: it is alive
        m_greeted = true;
        if (!m_phoneType && type != PLUGGED) setState(QStringLiteral("waiting"));
    }
    switch (type) {
    case PLUGGED: {
        m_phoneType = data.size() >= 4 ? qFromLittleEndian<quint32>(data.constData()) : 0;
        const QString kind = m_phoneType == 3 ? QStringLiteral("CarPlay")
                           : m_phoneType == 5 ? QStringLiteral("Android Auto") : QCoreApplication::translate("Msg", "Phone");
        qInfo() << "[Carlink] telefono collegato:" << kind << "(tipo" << m_phoneType << ")";
        QMetaObject::invokeMethod(this, [this, kind] { m_phoneKind = kind; }, Qt::QueuedConnection);
        setState(QStringLiteral("connecting"));
        sendNight();
        sendCommand(CMD_KEYFRAME);
        break;
    }
    case UNPLUGGED:
        qInfo() << "[Carlink] phone disconnected from the dongle";
        m_phoneType = 0;
        m_videoFlag = false;
        m_decoder.reset();
        setVideoActive(false);
        QMetaObject::invokeMethod(this, [this] {
            m_phoneKind.clear();
            for (CarAudioOut *a : std::as_const(m_audio)) a->stop();
            m_mic->stop();
        }, Qt::QueuedConnection);
        setState(QStringLiteral("waiting"));
        break;
    case PHASE:
        qInfo() << "[Carlink] fase" << (data.size() >= 4 ? qFromLittleEndian<quint32>(data.constData()) : 0);
        break;
    case VIDEO: {
        if (data.size() <= 20) break;
        const quint32 w = qFromLittleEndian<quint32>(data.constData());
        const quint32 hgt = qFromLittleEndian<quint32>(data.constData() + 4);
        m_decoder.decode(data.mid(20));
        if (!m_videoFlag && m_decoder.frames() > 0) {
            m_videoFlag = true;
            qInfo() << "[Carlink] primo fotogramma" << w << "x" << hgt;
            QMetaObject::invokeMethod(this, [this, w, hgt] {
                m_videoSize = QSize(int(w), int(hgt));
                emit videoSizeChanged();
            }, Qt::QueuedConnection);
            setVideoActive(true);
            setState(QStringLiteral("running"));
        }
        break;
    }
    case AUDIO:
        handleAudio(data);
        break;
    case COMMAND: {
        const quint32 c = data.size() >= 4 ? qFromLittleEndian<quint32>(data.constData()) : 0;
        qInfo() << "[Carlink] command from the dongle" << c;
        if (c == CMD_RECORD_START) QMetaObject::invokeMethod(this, [this] { m_mic->start(); }, Qt::QueuedConnection);
        else if (c == CMD_RECORD_STOP) QMetaObject::invokeMethod(this, [this] { m_mic->stop(); }, Qt::QueuedConnection);
        break;
    }
    case SW_VERSION:
        qInfo() << "[Carlink] versione dongle:" << QString::fromUtf8(data).trimmed();
        // the dongle is ready: if there is no phone, wait
        if (!m_phoneType) setState(QStringLiteral("waiting"));
        break;
    case MANUFACTURER:
    case BT_NAME:
    case WIFI_NAME:
    case BT_ADDR:
    case BT_PIN:
        qInfo() << "[Carlink] info 0x" + QString::number(type, 16) << QString::fromUtf8(data).trimmed().left(60);
        break;
    case BOX_SETTINGS:
        qInfo().noquote() << "[Carlink] impostazioni dongle:" << QString::fromUtf8(data).left(600);
        break;
    case MEDIA_DATA:
        qInfo() << "[Carlink] track:" << QString::fromUtf8(data.mid(4)).left(160);
        break;
    case HEARTBEAT:
        break;
    default:
        qInfo().noquote() << QStringLiteral("[Carlink] pacchetto 0x%1 (%2 byte) %3")
                                 .arg(type, 2, 16, QLatin1Char('0')).arg(data.size())
                                 .arg(data.size() <= 24 ? QString::fromLatin1(data.toHex()) : QString());
    }
}

void CarlinkDongle::handleAudio(const QByteArray &data)
{
    if (data.size() < 12) return;
    const quint32 decodeType = qFromLittleEndian<quint32>(data.constData());
    float volume = 1.0f;
    memcpy(&volume, data.constData() + 4, 4);
    const quint32 audioType = qFromLittleEndian<quint32>(data.constData() + 8);

    if (data.size() == 13) {       // comando audio
        const int cmd = uchar(data[12]);
        qInfo() << "[Carlink] comando audio" << cmd << "canale" << audioType;
        QMetaObject::invokeMethod(this, [this, cmd, audioType] {
            if (cmd == 2 || cmd == 5 || cmd == 7 || cmd == 9)        // end of a stream
                if (CarAudioOut *a = m_audio.value(int(audioType))) a->stop();
            if (cmd == 3) m_mic->start();                            // configurazione ingresso
            if ((cmd == 2 || cmd == 9) && audioType == 1 && m_mediaPlaying) {
                m_mediaPlaying = false;
                emit mediaAudioActiveChanged(false);
            }
        }, Qt::QueuedConnection);
        return;
    }
    if (data.size() <= 12) return;
    int rate = 0, chans = 0;
    if (!audioFormat(decodeType, &rate, &chans)) return;
    const QByteArray pcm = data.mid(12);
    QMetaObject::invokeMethod(this, [this, audioType, rate, chans, volume, pcm] {
        CarAudioOut *&a = m_audio[int(audioType)];
        if (a && (a->rate() != rate || a->channels() != chans)) { a->stop(); a->deleteLater(); a = nullptr; }
        if (!a) a = new CarAudioOut(rate, chans, this);
        a->push(pcm);
        if (audioType == 1 && !m_mediaPlaying) {
            m_mediaPlaying = true;
            emit mediaAudioActiveChanged(true);
        }
        Q_UNUSED(volume)
    }, Qt::QueuedConnection);
}

// ─────────────────────────────── tocco ──────────────────────────────────────

void CarlinkDongle::touch(int action, double x, double y)
{
    if (m_native && m_native->running()) { m_native->touch(action, x, y); return; }
    if (!m_sessionUp) return;
    static int logged = 0;
    if (logged < 12) { ++logged; qInfo() << "[Carlink] tocco" << action << x << y; }
    static const bool single = qEnvironmentVariable("VOLTA_CARLINK_TOUCH") == "single";
    if (single) {                      // vecchio formato: interi 0..10000
        const quint32 act = action == 0 ? 14 : action == 1 ? 16 : 15;
        sendPacket(TOUCH, le32(act) + le32(quint32(qBound(0.0, x, 1.0) * 10000.0))
                              + le32(quint32(qBound(0.0, y, 1.0) * 10000.0)) + le32(0));
        return;
    }
    // multitouch: for each finger x,y (float 0..1), action (1 down, 2 move, 0 up), id
    const quint32 act = action == 0 ? 1 : action == 1 ? 0 : 2;
    sendPacket(MULTITOUCH, lef32(float(qBound(0.0, x, 1.0))) + lef32(float(qBound(0.0, y, 1.0)))
                               + le32(act) + le32(0));
}


// ─────────────── native Android Auto (no dongle) ───────────────
void CarlinkDongle::setSource(const QString &s)
{
    const QString v = (s == QLatin1String("usb") || s == QLatin1String("wireless")) ? s : QStringLiteral("dongle");
    if (m_source == v) return;
    m_source = v;
    saveSettings();
    emit settingsChanged();
    applySource();
}

void CarlinkDongle::applySource()
{
    const bool native = m_source != QLatin1String("dongle");
    m_wantEnabled = m_enabled && !native;
    if (native) m_dropSession = true;                       // lets go of the dongle if it was in use
    stopNative();
    m_phoneKind.clear(); m_phoneName.clear();
    setState(m_enabled ? QStringLiteral("searching") : QStringLiteral("off"));
    if (native && m_enabled) startNative();
}

void CarlinkDongle::stopNative()
{
    if (m_native) m_native->stop();
    setVideoActive(false);
    for (CarAudioOut *a : std::as_const(m_audio)) if (a) a->stop();
    m_mic->stop();
}

void CarlinkDongle::startNative()
{
    if (!m_native) {
        m_native = new AaClient(this);
        m_native->onVideo = [this](const QByteArray &h264) { m_decoder.decode(h264); };   // worker thread
        connect(m_native, &AaClient::status, this, [this](const QString &st, const QString &detail) {
            // searching / probing / permission / connecting / running / error -> the states the UI knows
            if (st == QLatin1String("probing")) setState(QStringLiteral("starting"));
            else if (st == QLatin1String("permission")) setState(QStringLiteral("error"), QStringLiteral("permission"));
            else if (st == QLatin1String("searching")) setState(QStringLiteral("searching"));
            else setState(st, detail);
            m_sessionUp = (st == QLatin1String("running"));
        });
        connect(m_native, &AaClient::phoneName, this, [this](const QString &n) {
            m_phoneKind = QStringLiteral("Android Auto");
            m_phoneName = n;
            emit stateChanged();
        });
        connect(m_native, &AaClient::videoActive, this, [this](bool on) { setVideoActive(on); });
        connect(m_native, &AaClient::videoSize, this, [this](int w, int h) {
            if (m_videoSize != QSize(w, h)) { m_videoSize = QSize(w, h); emit videoSizeChanged(); }
        });
        connect(m_native, &AaClient::audioPcm, this, &CarlinkDongle::handleNativeAudio);
        connect(m_native, &AaClient::audioStop, this, [this](int stream) {
            const int type = stream == 3 ? 4 : stream;
            if (CarAudioOut *a = m_audio.value(type)) a->stop();
            if (stream == 1 && m_mediaPlaying) { m_mediaPlaying = false; emit mediaAudioActiveChanged(false); }
        });
        connect(m_native, &AaClient::microphone, this, [this](bool open) { if (open) m_mic->start(); else m_mic->stop(); });
    }
    AaSession::Config c;
    c.videoW = m_resolution == 1 ? 800 : m_resolution == 2 ? 1280 : 1920;
    c.videoH = m_resolution == 1 ? 480 : m_resolution == 2 ? 720 : 1080;
    c.fps = 30;
    c.dpi = m_dpi > 0 ? m_dpi : (m_resolution == 1 ? 140 : m_resolution == 2 ? 160 : 240);
    c.theme = m_theme == QLatin1String("light") ? 1 : m_theme == QLatin1String("dark") ? 2
            : m_theme == QLatin1String("auto") ? 0 : (m_uiLight ? 1 : 2);
    c.night = m_nightValue.load() == 1;
    QString err;
    if (!aaLoadCertificate(&c.certPem, &c.keyPem, QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                                                     + QStringLiteral("/volta_os"), &err)) {
        setState(QStringLiteral("error"), err);
        return;
    }
    m_decoder.reset();
    if (m_videoSize != QSize(c.videoW, c.videoH)) { m_videoSize = QSize(c.videoW, c.videoH); emit videoSizeChanged(); }
    m_native->start(m_source == QLatin1String("wireless") ? AaClient::Wireless : AaClient::Usb, c);
}

// PCM from the phone: media = 1, navigation prompts = 2, system sounds = 4 (3 is the microphone)
void CarlinkDongle::handleNativeAudio(int stream, int rate, int channels, const QByteArray &pcm)
{
    const int type = stream == 3 ? 4 : stream;
    CarAudioOut *&a = m_audio[type];
    if (a && (a->rate() != rate || a->channels() != channels)) { a->stop(); a->deleteLater(); a = nullptr; }
    if (!a) a = new CarAudioOut(rate, channels, this);
    a->push(pcm);
    if (stream == 1 && !m_mediaPlaying) { m_mediaPlaying = true; emit mediaAudioActiveChanged(true); }
}
