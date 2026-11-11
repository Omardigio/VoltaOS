// Self-test of the Android Auto client: a fake phone (TLS server side of the protocol) talks to
// AaSession through two queued connections, no USB or network involved.
//   build: see tests/androidauto/CMakeLists.txt     run: ./aa_fake_phone_test <cert.crt> <key.key>
#include <functional>
#include <QSet>
#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <QtEndian>
#include <QDebug>
#include "AaSession.h"
#include "AaFraming.h"
#include "AaTls.h"

#include "aap_protobuf/service/control/message/ChannelOpenRequest.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusRequest.pb.h"
#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/source/message/Ack.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorRequest.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorBatch.pb.h"

namespace ctl = aap_protobuf::service::control::message;
using namespace aa;

static int g_fail = 0;
#define CHECK(cond, what) do { if (cond) qInfo().noquote() << "  PASS" << what; else { ++g_fail; qWarning().noquote() << "  FAIL" << what; } } while (0)

class FakePhone {
public:
    std::function<void(const QByteArray &)> transmit;
    std::function<void()> done;
    AaTls tls{true};
    Deframer de;
    bool secure = false;
    QByteArray partial[256];
    // what the head unit answered
    bool gotVersionReq = false, gotAuth = false, gotSD = false, gotPingResp = false, gotAudioFocus = false;
    QSet<int> openedAck, configured;
    int videoAcks = 0, audioAcks = 0, sensorResponses = 0, sensorBatches = 0, keyBinding = 0, touches = 0;
    QSet<int> serviceIds;
    int touchW = 0, touchH = 0, videoRes = 0;
    bool sawNightBatch = false, sawDrivingBatch = false;
    int lastTouchX = -1, lastTouchY = -1;
    int sdChannels = 0;

    explicit FakePhone(const QByteArray &c, const QByteArray &k) { QString e; tls.init(c, k, &e); }

    void send(quint8 ch, bool control, quint16 id, const QByteArray &body, bool enc) {
        QByteArray msg(2, 0); qToBigEndian<quint16>(id, msg.data()); msg += body;
        quint8 fl = kBulk | (control ? kControl : 0);
        QByteArray chunk = msg;
        if (enc) { fl |= kEncrypted; chunk = tls.encrypt(msg); }
        transmit(buildFrame(ch, fl, chunk, msg.size()));
    }
    template <class T> void sendPb(quint8 ch, bool control, quint16 id, const T &m, bool enc = true) {
        std::string s; m.SerializeToString(&s); send(ch, control, id, QByteArray(s.data(), int(s.size())), enc);
    }
    void onBytes(const QByteArray &b) {
        de.feed(b);
        Frame f;
        while (de.next(&f)) {
            QByteArray data = f.payload;
            if (f.flags & kEncrypted) data = tls.decrypt(data);
            const bool ctl = f.flags & kControl;
            QByteArray &p = partial[f.channel];
            if ((f.flags & kBulk) == kBulk) handle(f.channel, ctl, data);
            else { if (f.flags & kFirst) p = data; else p += data; if (f.flags & kLast) { handle(f.channel, ctl, p); p.clear(); } }
        }
    }
private:
    void handle(quint8 ch, bool control, const QByteArray &msg) {
        const quint16 id = qFromBigEndian<quint16>(msg.constData());
        const QByteArray body = msg.mid(2);
        if (ch == 0) {
            if (id == 1) {                                        // version request -> response 1.6 OK
                gotVersionReq = true;
                QByteArray v(6, 0); qToBigEndian<quint16>(1, v.data()); qToBigEndian<quint16>(6, v.data() + 2);
                send(0, false, 2, v, false);
            } else if (id == 3) {                                 // TLS bytes
                bool d, f; const QByteArray out = tls.handshake(body, &d, &f);
                if (f) { qWarning() << "phone: handshake failed"; ++g_fail; done(); return; }
                if (!out.isEmpty()) send(0, false, 3, out, false);
            } else if (id == 4) {                                 // auth complete -> encrypted from now
                gotAuth = true; secure = true;
                ctl::ServiceDiscoveryRequest rq; rq.set_device_name("Fake Phone");
                sendPb(0, false, 5, rq);
            } else if (id == 6) {                                 // service discovery response
                gotSD = true;
                ctl::ServiceDiscoveryResponse r; r.ParseFromArray(body.constData(), body.size());
                sdChannels = r.channels_size();
                for (const auto &s : r.channels()) {
                    serviceIds.insert(s.id());
                    if (s.has_input_source_service() && s.input_source_service().touchscreen_size()) {
                        touchW = s.input_source_service().touchscreen(0).width();
                        touchH = s.input_source_service().touchscreen(0).height();
                    }
                    if (s.has_media_sink_service() && s.media_sink_service().video_configs_size())
                        videoRes = int(s.media_sink_service().video_configs(0).codec_resolution());
                }
                runScenario();
            } else if (id == 12) gotPingResp = true;
            else if (id == 19) gotAudioFocus = true;
            return;
        }
        if (control && id == 8) { openedAck.insert(ch); return; }
        if (id == 0x8003 && (ch == 2 || ch == 3)) { configured.insert(ch); return; }  // CONFIG
        if (id == 0x8004) { if (ch == 2) ++videoAcks; else ++audioAcks; return; }
        if (ch == 7 && id == 0x8003) { ++keyBinding; return; }
        if (ch == 7 && id == 0x8001) {
            aap_protobuf::service::inputsource::message::InputReport r; r.ParseFromArray(body.constData(), body.size());
            if (r.has_touch_event()) { ++touches; lastTouchX = int(r.touch_event().pointer_data(0).x()); lastTouchY = int(r.touch_event().pointer_data(0).y()); }
            return;
        }
        if (ch == 1 && id == 0x8002) { ++sensorResponses; return; }
        if (ch == 1 && id == 0x8003) {
            ++sensorBatches;
            aap_protobuf::service::sensorsource::message::SensorBatch b; b.ParseFromArray(body.constData(), body.size());
            if (b.night_mode_data_size()) sawNightBatch = true;
            if (b.driving_status_data_size()) sawDrivingBatch = true;
        }
    }
    void runScenario() {
        for (int ch : {1, 2, 3, 4, 5, 6, 7}) {                    // open every channel
            ctl::ChannelOpenRequest o; o.set_priority(0); o.set_service_id(ch);
            sendPb(quint8(ch), true, 7, o);
        }
        namespace sh = aap_protobuf::service::media::shared::message;
        for (int ch : {2, 3}) {
            sh::Setup s; s.set_type(ch == 2 ? sh::MEDIA_CODEC_VIDEO_H264_BP : sh::MEDIA_CODEC_AUDIO_PCM);
            sendPb(quint8(ch), false, 0x8000, s);
            sh::Start st; st.set_session_id(10 + ch); st.set_configuration_index(0);
            sendPb(quint8(ch), false, 0x8001, st);
        }
        aap_protobuf::service::sensorsource::message::SensorRequest sr;
        for (int t : {13, 10}) { sr.set_type(aap_protobuf::service::sensorsource::message::SensorType(t)); sr.set_min_update_period(0); sendPb(1, false, 0x8001, sr); }
        send(7, false, 0x8002, QByteArray::fromHex("0a0103"), true);             // key binding request (keycode 3)
        ctl::PingRequest pg; pg.set_timestamp(123); sendPb(0, false, 11, pg);
        ctl::AudioFocusRequest af; af.set_audio_focus_type(ctl::AUDIO_FOCUS_GAIN); sendPb(0, false, 18, af);
        // a video frame bigger than 16 KiB (forces the split in several frames) and an audio block
        QByteArray video(8, 0); video += QByteArray(40000, 'V');
        send(2, false, 0, video, true);
        QByteArray pcm(8, 0); pcm += QByteArray(1920, 'A');
        send(3, false, 0, pcm, true);
        QTimer::singleShot(400, [this] { done(); });
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) { qWarning() << "usage: aa_fake_phone_test cert.crt key.key"; return 2; }
    QFile c(argv[1]), k(argv[2]);
    if (!c.open(QIODevice::ReadOnly) || !k.open(QIODevice::ReadOnly)) { qWarning() << "cannot read cert/key"; return 2; }
    const QByteArray cert = c.readAll(), key = k.readAll();

    AaSession hu;
    AaSession::Config cfg; cfg.certPem = cert; cfg.keyPem = key; cfg.night = true;
    hu.setConfig(cfg);
    FakePhone phone(cert, key);

    int videoBytes = 0, audioBytes = 0; QString name; QString state;
    // queued hand-over in both directions, like two different threads would do
    QObject::connect(&hu, &AaSession::transmit, &app, [&](const QByteArray &b) { QTimer::singleShot(0, &app, [&phone, b] { phone.onBytes(b); }); });
    phone.transmit = [&](const QByteArray &b) { QTimer::singleShot(0, &app, [&hu, b] { hu.onBytes(b); }); };
    QObject::connect(&hu, &AaSession::videoFrame, [&](const QByteArray &b) { videoBytes += b.size(); });
    QObject::connect(&hu, &AaSession::audioPcm, [&](int, int, int, const QByteArray &b) { audioBytes += b.size(); });
    QObject::connect(&hu, &AaSession::phoneName, [&](const QString &n) { name = n; });
    QObject::connect(&hu, &AaSession::stateChanged, [&](const QString &s, const QString &d) { state = s; qInfo().noquote() << "  state:" << s << d; });
    phone.done = [&] {
        hu.touch(0, 0.5, 0.25); hu.touch(1, 1.0, 1.0);          // touch down in the middle, up at the corner
        QTimer::singleShot(200, &app, &QCoreApplication::quit);
    };

    QTimer::singleShot(0, &hu, &AaSession::start);
    QTimer::singleShot(10000, &app, [&] { qWarning() << "TIMEOUT"; ++g_fail; app.quit(); });
    app.exec();

    qInfo() << "results:";
    CHECK(phone.gotVersionReq, "version request received by the phone");
    CHECK(phone.gotAuth, "TLS handshake completed, auth-complete received");
    CHECK(phone.gotSD && name == "Fake Phone", "service discovery answered, phone name read");
    CHECK(phone.sdChannels == 7, QString("7 services offered (got %1)").arg(phone.sdChannels));
    CHECK(phone.touchW == 1280 && phone.touchH == 720, QString("touch screen announced %1x%2").arg(phone.touchW).arg(phone.touchH));
    CHECK(phone.videoRes == 2, "video resolution 1280x720 announced");
    CHECK(phone.openedAck.size() == 7, QString("all 7 channel opens acknowledged (got %1)").arg(phone.openedAck.size()));
    CHECK(phone.configured.contains(2) && phone.configured.contains(3), "video and audio SETUP answered with CONFIG");
    CHECK(videoBytes == 40000, QString("40000-byte (multi-frame) video message delivered whole (got %1)").arg(videoBytes));
    CHECK(audioBytes == 1920, QString("audio PCM delivered (got %1)").arg(audioBytes));
    CHECK(phone.videoAcks >= 1 && phone.audioAcks >= 1, "media data acknowledged");
    CHECK(phone.sensorResponses == 2 && phone.sawDrivingBatch && phone.sawNightBatch, "sensors: driving status + night mode");
    CHECK(phone.keyBinding == 1, "key binding answered");
    CHECK(phone.gotPingResp, "ping answered");
    CHECK(phone.gotAudioFocus, "audio focus granted");
    CHECK(phone.touches == 2, QString("touch events reach the phone (got %1)").arg(phone.touches));
    CHECK(phone.lastTouchX == 1279 && phone.lastTouchY == 719, QString("touch mapped to pixels (last %1,%2)").arg(phone.lastTouchX).arg(phone.lastTouchY));
    qInfo() << (g_fail ? "FAILED" : "ALL OK");
    return g_fail ? 1 : 0;
}
