#include "AaSession.h"

#include <QDebug>
#include <QtEndian>
#include <algorithm>

#include "aap_protobuf/service/control/message/AudioFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusRequest.pb.h"
#include "aap_protobuf/service/control/message/AuthResponse.pb.h"
#include "aap_protobuf/service/control/message/ByeByeResponse.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenRequest.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenResponse.pb.h"
#include "aap_protobuf/service/control/message/NavFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/NavFocusRequestNotification.pb.h"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"
#include "aap_protobuf/service/control/message/PingResponse.pb.h"
#include "aap_protobuf/service/Service.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h"
#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/sink/message/KeyBindingResponse.pb.h"
#include "aap_protobuf/service/media/source/message/Ack.pb.h"
#include "aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h"
#include "aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusRequestNotification.pb.h"
#include "aap_protobuf/service/sensorsource/message/DrivingStatus.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorBatch.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorRequest.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorResponse.pb.h"

namespace pb = aap_protobuf;
namespace ctl = aap_protobuf::service::control::message;
namespace med = aap_protobuf::service::media;
namespace sen = aap_protobuf::service::sensorsource::message;
namespace inp = aap_protobuf::service::inputsource::message;

// control channel messages
enum : quint16 { C_VERSION_REQ = 1, C_VERSION_RESP = 2, C_SSL = 3, C_AUTH_COMPLETE = 4, C_SD_REQ = 5,
                 C_SD_RESP = 6, C_CHAN_OPEN_REQ = 7, C_CHAN_OPEN_RESP = 8, C_CHAN_CLOSE = 9,
                 C_PING_REQ = 11, C_PING_RESP = 12, C_NAV_FOCUS_REQ = 13, C_NAV_FOCUS = 14,
                 C_BYEBYE_REQ = 15, C_BYEBYE_RESP = 16, C_AUDIO_FOCUS_REQ = 18, C_AUDIO_FOCUS = 19 };
// media channels
enum : quint16 { M_DATA = 0, M_CODEC_CONFIG = 1, M_SETUP = 0x8000, M_START = 0x8001, M_STOP = 0x8002,
                 M_CONFIG = 0x8003, M_ACK = 0x8004, M_MIC_REQ = 0x8005, M_MIC_RESP = 0x8006,
                 M_VIDEO_FOCUS_REQ = 0x8007, M_VIDEO_FOCUS = 0x8008 };
enum : quint16 { I_REPORT = 0x8001, I_BINDING_REQ = 0x8002, I_BINDING_RESP = 0x8003 };
enum : quint16 { S_REQUEST = 0x8001, S_RESPONSE = 0x8002, S_BATCH = 0x8003 };

static const bool kTrace = qEnvironmentVariableIsSet("VOLTA_AA_TRACE");   // log every message

static const quint16 kVersionMajor = 1, kVersionMinor = 6;     // as the aasdk library announces

AaSession::AaSession(QObject *parent) : QObject(parent)
{
    m_ping.setInterval(5000);
    connect(&m_ping, &QTimer::timeout, this, [this] {
        if (!m_secure || m_ended) return;
        qInfo().nospace() << "[AA] stats: video " << m_vFrames << " units (" << m_vBytes / 1024 << " KiB), audio "
                          << m_aBytes / 1024 << " KiB, touches sent " << m_touches;
        ctl::PingRequest p;
        p.set_timestamp(qint64(m_clock.nsecsElapsed()));
        sendPb(Control, false, C_PING_REQ, p);
    });
}

template <class T>
void AaSession::sendPb(quint8 ch, bool control, quint16 id, const T &msg)
{
    std::string s;
    msg.SerializeToString(&s);
    sendMessage(ch, control, m_secure, id, QByteArray(s.data(), int(s.size())));
}

void AaSession::sendMessage(quint8 channel, bool control, bool encrypted, quint16 id, const QByteArray &body)
{
    if (m_ended) return;
    if (kTrace) qInfo().nospace() << "[AA] -> ch " << channel << (control ? " ctl" : "") << " id 0x" << Qt::hex << id
                                  << " len " << Qt::dec << body.size();
    QByteArray msg(2, 0);
    qToBigEndian<quint16>(id, msg.data());
    msg += body;
    const int total = msg.size();
    int off = 0;
    while (off < total || (total == 0 && off == 0)) {
        const int n = std::min(aa::kMaxFramePayload, total - off);
        quint8 flags = 0;
        if (off == 0) flags |= aa::kFirst;
        if (off + n >= total) flags |= aa::kLast;
        if (control) flags |= aa::kControl;
        QByteArray chunk = msg.mid(off, n);
        if (encrypted) {
            flags |= aa::kEncrypted;
            chunk = m_tls ? m_tls->encrypt(chunk) : QByteArray();
            if (chunk.isEmpty()) { fail(QStringLiteral("encryption failed")); return; }
        }
        emit transmit(aa::buildFrame(channel, flags, chunk, quint32(total)));
        off += n;
        if (total == 0) break;
    }
}

void AaSession::start()
{
    m_clock.start();
    m_ended = false; m_secure = false; m_started = true;
    m_deframer.clear();
    for (auto &p : m_partial) p.clear();
    m_open.clear(); m_mediaSession.clear(); m_sensorsOn = 0; m_videoOn = false;
    m_tls = std::make_unique<AaTls>(m_serverTls);
    QString err;
    if (!m_tls->init(m_cfg.certPem, m_cfg.keyPem, &err)) { fail(QStringLiteral("TLS: ") + err); return; }
    m_state = QStringLiteral("connecting");
    emit stateChanged(m_state, QStringLiteral("version"));
    QByteArray v(4, 0);
    qToBigEndian<quint16>(kVersionMajor, v.data());
    qToBigEndian<quint16>(kVersionMinor, v.data() + 2);
    sendMessage(Control, false, false, C_VERSION_REQ, v);
    qInfo() << "[AA] connected, version request sent";
}

void AaSession::stop(const QString &reason)
{
    if (m_ended) return;
    if (m_secure) {                       // polite goodbye
        sendControl(C_BYEBYE_REQ, QByteArray::fromHex("0800"));
    }
    m_ended = true;
    m_ping.stop();
    if (m_videoOn) { m_videoOn = false; emit videoActive(false); }
    for (int s = 1; s <= 3; ++s) emit audioStop(s);
    if (m_micOpen) { m_micOpen = false; emit microphone(false); }
    m_state = QStringLiteral("ended");
    emit stateChanged(m_state, reason);
    emit finished(reason);
}

void AaSession::fail(const QString &why)
{
    if (m_ended) return;
    qWarning().noquote() << "[AA] error:" << why;
    m_ended = true;
    m_ping.stop();
    if (m_videoOn) { m_videoOn = false; emit videoActive(false); }
    for (int s = 1; s <= 3; ++s) emit audioStop(s);
    if (m_micOpen) { m_micOpen = false; emit microphone(false); }
    m_state = QStringLiteral("error");
    emit stateChanged(m_state, why);
    emit finished(why);
}

// ───────────────────────────── receiving ─────────────────────────────
void AaSession::onBytes(const QByteArray &bytes)
{
    if (m_ended || !m_started) return;
    m_deframer.feed(bytes);
    aa::Frame f;
    while (!m_ended && m_deframer.next(&f)) handleFrame(f);
}

void AaSession::handleFrame(const aa::Frame &f)
{
    QByteArray data = f.payload;
    if (f.flags & aa::kEncrypted) {
        if (!m_tls || !m_tls->active()) { fail(QStringLiteral("encrypted data before the handshake")); return; }
        data = m_tls->decrypt(data);
    }
    const bool control = f.flags & aa::kControl;
    QByteArray &part = m_partial[f.channel];
    if ((f.flags & aa::kBulk) == aa::kBulk) {                 // whole message in one frame
        dispatch(f.channel, control, data);
        return;
    }
    if (f.flags & aa::kFirst) { part = data; m_partialControl[f.channel] = control; }
    else part += data;
    if (f.flags & aa::kLast) {
        const QByteArray whole = part;
        part.clear();
        dispatch(f.channel, m_partialControl[f.channel], whole);
    }
}

void AaSession::dispatch(quint8 channel, bool control, const QByteArray &msg)
{
    if (msg.size() < 2) return;
    const quint16 id = qFromBigEndian<quint16>(msg.constData());
    const QByteArray body = msg.mid(2);
    if (kTrace) qInfo().nospace() << "[AA] <- ch " << channel << (control ? " ctl" : "") << " id 0x" << Qt::hex << id
                                  << " len " << Qt::dec << body.size() << " " << body.left(24).toHex();
    if (channel == Control) { onControl(id, body); return; }
    if (control) { onChannelControl(channel, id, body); return; }
    switch (channel) {
    case Video: case AudioMedia: case AudioGuidance: case AudioSystem: onMedia(channel, id, body); break;
    case Input: onInput(id, body); break;
    case Sensor: onSensor(id, body); break;
    case Microphone: onMicrophone(id, body); break;
    default: qWarning() << "[AA] message on unknown channel" << channel << "id" << id; break;
    }
}

void AaSession::handshakeStep(const QByteArray &in)
{
    bool done = false, failed = false;
    const QByteArray out = m_tls->handshake(in, &done, &failed);
    if (failed) { fail(QStringLiteral("TLS handshake failed (certificate refused by the phone?)")); return; }
    if (!out.isEmpty()) sendMessage(Control, false, false, C_SSL, out);
    if (done) {
        qInfo() << "[AA] TLS handshake done";
        ctl::AuthResponse a;
        a.set_status(0);
        sendPb(Control, false, C_AUTH_COMPLETE, a);       // still plain: it is the last plain message
        m_secure = true;
        m_ping.start();
        emit stateChanged(m_state, QStringLiteral("discovery"));
    }
}

void AaSession::onControl(quint16 id, const QByteArray &body)
{
    switch (id) {
    case C_VERSION_RESP: {
        if (body.size() < 6) { fail(QStringLiteral("short version answer")); return; }
        const quint16 maj = qFromBigEndian<quint16>(body.constData());
        const quint16 min = qFromBigEndian<quint16>(body.constData() + 2);
        const qint16 st = qFromBigEndian<qint16>(body.constData() + 4);
        qInfo() << "[AA] phone protocol version" << maj << "." << min << "status" << st;
        if (st != 0) { fail(QStringLiteral("protocol version not compatible (%1.%2)").arg(maj).arg(min)); return; }
        emit stateChanged(m_state, QStringLiteral("handshake"));
        handshakeStep(QByteArray());                      // we are the TLS client: we speak first
        break;
    }
    case C_SSL:
        handshakeStep(body);
        break;
    case C_SD_REQ:
        serviceDiscovery(body);
        break;
    case C_PING_REQ: {
        ctl::PingRequest rq;
        ctl::PingResponse rs;
        rs.set_timestamp(rq.ParseFromArray(body.constData(), body.size()) ? rq.timestamp() : 0);
        sendPb(Control, false, C_PING_RESP, rs);
        break;
    }
    case C_PING_RESP:
        break;
    case C_AUDIO_FOCUS_REQ: {
        ctl::AudioFocusRequest rq;
        rq.ParseFromArray(body.constData(), body.size());
        ctl::AudioFocusNotification n;
        const bool release = rq.audio_focus_type() == ctl::AUDIO_FOCUS_RELEASE;
        n.set_focus_state(release ? ctl::AUDIO_FOCUS_STATE_LOSS
                          : rq.audio_focus_type() == ctl::AUDIO_FOCUS_GAIN ? ctl::AUDIO_FOCUS_STATE_GAIN
                                                                           : ctl::AUDIO_FOCUS_STATE_GAIN_TRANSIENT);
        n.set_unsolicited(false);
        sendPb(Control, false, C_AUDIO_FOCUS, n);
        break;
    }
    case C_NAV_FOCUS_REQ: {
        ctl::NavFocusNotification n;
        n.set_focus_type(ctl::NAV_FOCUS_PROJECTED);
        sendPb(Control, false, C_NAV_FOCUS, n);
        break;
    }
    case C_BYEBYE_REQ:
        sendPb(Control, false, C_BYEBYE_RESP, ctl::ByeByeResponse());
        stop(QStringLiteral("the phone ended the session"));
        break;
    case C_BYEBYE_RESP:
        break;
    default:
        qInfo() << "[AA] control message ignored, id" << id;
        break;
    }
}

void AaSession::onChannelControl(quint8 ch, quint16 id, const QByteArray &body)
{
    if (id == C_CHAN_OPEN_REQ) {
        ctl::ChannelOpenRequest rq;
        rq.ParseFromArray(body.constData(), body.size());
        ctl::ChannelOpenResponse rs;
        rs.set_status(pb::shared::STATUS_SUCCESS);
        m_open[ch] = true;
        sendPb(ch, true, C_CHAN_OPEN_RESP, rs);
        qInfo() << "[AA] channel" << ch << "open";
        if (ch == Video) emit stateChanged(QStringLiteral("running"), QString());
    } else if (id == C_CHAN_CLOSE) {
        m_open[ch] = false;
        if (isAudio(ch)) emit audioStop(streamOf(ch));
    }
}

// Video and audio: the phone sends SETUP, we answer READY, then START and the data flow.
void AaSession::onMedia(quint8 ch, quint16 id, const QByteArray &body)
{
    switch (id) {
    case M_SETUP: {
        med::shared::message::Config c;
        c.set_status(med::shared::message::Config::STATUS_READY);
        c.set_max_unacked(1);
        c.add_configuration_indices(0);
        sendPb(ch, false, M_CONFIG, c);
        if (ch == Video) {                      // the phone waits for the video focus before it starts
            med::video::message::VideoFocusNotification n;
            n.set_focus(med::video::message::VIDEO_FOCUS_PROJECTED);
            n.set_unsolicited(true);
            sendPb(ch, false, M_VIDEO_FOCUS, n);
        }
        break;
    }
    case M_START: {
        med::shared::message::Start s;
        s.ParseFromArray(body.constData(), body.size());
        m_mediaSession[ch] = s.session_id();
        qInfo() << "[AA] channel" << ch << "started, session" << s.session_id();
        if (ch == Video) {
            m_videoOn = true;
            emit videoSize(m_cfg.videoW, m_cfg.videoH);
        }
        break;
    }
    case M_STOP:
        if (ch == Video && m_videoOn) { m_videoOn = false; emit videoActive(false); }
        if (isAudio(ch)) emit audioStop(streamOf(ch));
        break;
    case M_CODEC_CONFIG:
    case M_DATA: {
        QByteArray payload = body;
        if (id == M_DATA) {                                   // 8-byte timestamp first
            if (payload.size() < 8) break;
            payload.remove(0, 8);
        }
        if (ch == Video) {
            if (!m_lastVideoW) { m_lastVideoW = m_cfg.videoW; m_lastVideoH = m_cfg.videoH; }
            ++m_vFrames; m_vBytes += payload.size();
            emit videoFrame(payload);
            if (!payload.isEmpty()) emit videoActive(true);
        } else if (isAudio(ch)) {
            const int rate = ch == AudioMedia ? 48000 : 16000;
            const int chans = ch == AudioMedia ? 2 : 1;
            m_aBytes += payload.size();
            emit audioPcm(streamOf(ch), rate, chans, payload);
        }
        med::source::message::Ack ack;                        // one ACK per data message
        ack.set_session_id(m_mediaSession.value(ch, 0));
        ack.set_ack(1);
        sendPb(ch, false, M_ACK, ack);
        break;
    }
    case M_VIDEO_FOCUS_REQ: {
        med::video::message::VideoFocusNotification n;
        n.set_focus(med::video::message::VIDEO_FOCUS_PROJECTED);
        n.set_unsolicited(false);
        sendPb(ch, false, M_VIDEO_FOCUS, n);
        break;
    }
    default:
        qInfo() << "[AA] media message ignored: channel" << ch << "id" << Qt::hex << id;
        break;
    }
}

void AaSession::onInput(quint16 id, const QByteArray &)
{
    if (id == I_BINDING_REQ) {
        med::sink::message::KeyBindingResponse r;
        r.set_status(0);
        sendPb(Input, false, I_BINDING_RESP, r);
    }
}

void AaSession::onSensor(quint16 id, const QByteArray &body)
{
    if (id != S_REQUEST) return;
    sen::SensorRequest rq;
    rq.ParseFromArray(body.constData(), body.size());
    sen::SensorResponse rs;
    rs.set_status(pb::shared::STATUS_SUCCESS);
    sendPb(Sensor, false, S_RESPONSE, rs);
    sendSensorBatch(int(rq.type()));
}

void AaSession::sendSensorBatch(int type)
{
    sen::SensorBatch b;
    if (type == sen::SENSOR_DRIVING_STATUS_DATA) {
        b.add_driving_status_data()->set_status(sen::DRIVE_STATUS_UNRESTRICTED);
    } else if (type == sen::SENSOR_NIGHT_MODE) {
        b.add_night_mode_data()->set_night_mode(m_cfg.night);
    } else {
        return;                                              // other sensors: the phone copes without
    }
    m_sensorsOn |= 1 << type;
    sendPb(Sensor, false, S_BATCH, b);
}

void AaSession::setNight(bool night)
{
    m_cfg.night = night;
    if (m_secure && (m_sensorsOn & (1 << sen::SENSOR_NIGHT_MODE))) sendSensorBatch(sen::SENSOR_NIGHT_MODE);
}

void AaSession::onMicrophone(quint16 id, const QByteArray &body)
{
    if (id != M_MIC_REQ) return;
    med::source::message::MicrophoneRequest rq;
    rq.ParseFromArray(body.constData(), body.size());
    med::source::message::MicrophoneResponse rs;
    rs.set_status(0);
    rs.set_session_id(++m_micSession);
    sendPb(Microphone, false, M_MIC_RESP, rs);
    m_micOpen = rq.open();
    emit microphone(m_micOpen);
}

void AaSession::micData(const QByteArray &pcm)
{
    if (!m_secure || !m_micOpen || pcm.isEmpty()) return;
    QByteArray body(8, 0);
    qToBigEndian<quint64>(quint64(m_clock.nsecsElapsed() / 1000), body.data());   // timestamp (µs)
    body += pcm;
    sendMessage(Microphone, false, true, M_DATA, body);
}

// ───────────────────────────── input ─────────────────────────────
void AaSession::touch(int action, double x, double y)
{
    if (!m_secure || !m_open.value(Input)) return;
    inp::InputReport r;
    r.set_timestamp(quint64(m_clock.nsecsElapsed()));
    auto *t = r.mutable_touch_event();
    auto *p = t->add_pointer_data();
    p->set_x(quint32(std::clamp(x, 0.0, 1.0) * (m_cfg.videoW - 1)));
    p->set_y(quint32(std::clamp(y, 0.0, 1.0) * (m_cfg.videoH - 1)));
    p->set_pointer_id(0);
    ++m_touches;
    t->set_action_index(0);
    t->set_action(action == 0 ? inp::ACTION_DOWN : action == 1 ? inp::ACTION_UP : inp::ACTION_MOVED);
    sendPb(Input, false, I_REPORT, r);
}

void AaSession::key(int keycode, bool down)
{
    if (!m_secure || !m_open.value(Input)) return;
    inp::InputReport r;
    r.set_timestamp(quint64(m_clock.nsecsElapsed()));
    auto *k = r.mutable_key_event()->add_keys();
    k->set_keycode(quint32(keycode));
    k->set_down(down);
    k->set_metastate(0);
    sendPb(Input, false, I_REPORT, r);
}

// ───────────────────────────── service discovery ─────────────────────────────
// The phone asks what this "car" offers; the answer lists every channel with its configuration.
void AaSession::serviceDiscovery(const QByteArray &body)
{
    ctl::ServiceDiscoveryRequest rq;
    rq.ParseFromArray(body.constData(), body.size());
    const QString dev = QString::fromStdString(rq.device_name());
    qInfo().noquote() << "[AA] phone:" << dev;
    emit phoneName(dev);

    namespace sh = aap_protobuf::service::media::shared::message;
    namespace sk = aap_protobuf::service::media::sink::message;
    ctl::ServiceDiscoveryResponse r;

    // sensors: driving status (so the phone does not lock the screen) and day/night
    {
        auto *s = r.add_channels();
        s->set_id(Sensor);
        auto *ss = s->mutable_sensor_source_service();
        ss->add_sensors()->set_sensor_type(sen::SENSOR_DRIVING_STATUS_DATA);
        ss->add_sensors()->set_sensor_type(sen::SENSOR_NIGHT_MODE);
    }
    // video
    {
        auto *s = r.add_channels();
        s->set_id(Video);
        auto *m = s->mutable_media_sink_service();
        m->set_available_type(sh::MEDIA_CODEC_VIDEO_H264_BP);
        auto *v = m->add_video_configs();
        v->set_codec_resolution(m_cfg.videoW >= 1920 ? sk::VIDEO_1920x1080
                                : m_cfg.videoW >= 1280 ? sk::VIDEO_1280x720 : sk::VIDEO_800x480);
        v->set_frame_rate(m_cfg.fps >= 60 ? sk::VIDEO_FPS_60 : sk::VIDEO_FPS_30);
        v->set_width_margin(0);
        v->set_height_margin(0);
        v->set_density(quint32(m_cfg.dpi > 0 ? m_cfg.dpi : 160));
        v->set_video_codec_type(sh::MEDIA_CODEC_VIDEO_H264_BP);
        v->mutable_ui_config()->set_ui_theme(m_cfg.theme == 1 ? sh::UI_THEME_LIGHT
                                             : m_cfg.theme == 2 ? sh::UI_THEME_DARK : sh::UI_THEME_AUTOMATIC);
    }
    // audio: media (stereo 48 kHz), navigation prompts and system sounds (mono 16 kHz)
    const auto audio = [&](quint8 ch, sk::AudioStreamType type, int rate, int chans) {
        auto *s = r.add_channels();
        s->set_id(ch);
        auto *m = s->mutable_media_sink_service();
        m->set_available_type(sh::MEDIA_CODEC_AUDIO_PCM);
        m->set_audio_type(type);
        auto *c = m->add_audio_configs();
        c->set_sampling_rate(quint32(rate));
        c->set_number_of_bits(16);
        c->set_number_of_channels(quint32(chans));
    };
    audio(AudioMedia, sk::AUDIO_STREAM_MEDIA, 48000, 2);
    audio(AudioGuidance, sk::AUDIO_STREAM_GUIDANCE, 16000, 1);
    audio(AudioSystem, sk::AUDIO_STREAM_SYSTEM_AUDIO, 16000, 1);
    // microphone
    {
        auto *s = r.add_channels();
        s->set_id(Microphone);
        auto *m = s->mutable_media_source_service();
        m->set_available_type(sh::MEDIA_CODEC_AUDIO_PCM);
        auto *c = m->mutable_audio_config();
        c->set_sampling_rate(16000);
        c->set_number_of_bits(16);
        c->set_number_of_channels(1);
    }
    // touch screen and keys
    {
        auto *s = r.add_channels();
        s->set_id(Input);
        auto *i = s->mutable_input_source_service();
        for (int k : {3, 4, 84, 85, 87, 88, 126, 127, 209}) i->add_keycodes_supported(k);   // home, back, search, media...
        auto *t = i->add_touchscreen();
        t->set_width(m_cfg.videoW);
        t->set_height(m_cfg.videoH);
        t->set_type(inp::CAPACITIVE);
    }

    r.set_driver_position(ctl::DRIVER_POSITION_LEFT);
    r.set_display_name(m_cfg.name.toStdString());
    auto *h = r.mutable_headunit_info();
    h->set_make("Volta");
    h->set_model("Volta OS");
    h->set_year("2026");
    h->set_vehicle_id("volta-os");
    h->set_head_unit_make("Volta");
    h->set_head_unit_model("Volta OS");
    h->set_head_unit_software_build("1");
    h->set_head_unit_software_version("1.0");
    sendPb(Control, false, C_SD_RESP, r);
    qInfo() << "[AA] service discovery answered";
}
