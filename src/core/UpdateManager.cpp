#include "UpdateManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

// Where the releases live. The AppImage asset of a release is the file ending in .AppImage.
static const char *kRepo = "Omardigio/VoltaOS";

#ifndef VOLTA_VERSION
#define VOLTA_VERSION "0"
#endif

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this))
{
}

QString UpdateManager::currentVersion() const { return QStringLiteral(VOLTA_VERSION); }

QString UpdateManager::releasePage() const
{
    return QStringLiteral("https://github.com/%1/releases/latest").arg(kRepo);
}

QString UpdateManager::appImagePath() const
{
    return qEnvironmentVariable("APPIMAGE");
}

bool UpdateManager::canInstall() const
{
    const QString p = appImagePath();
    if (p.isEmpty()) return false;
    const QFileInfo fi(p);
    return fi.isFile() && QFileInfo(fi.absolutePath()).isWritable();
}

bool UpdateManager::isNewer(const QString &a, const QString &b)
{
    // "v1.2.3" / "1.2" / "1.2.3-beta": the numbers are compared one by one, missing ones are 0
    const auto parts = [](const QString &v) {
        QList<int> out;
        QString s = v.trimmed();
        if (s.startsWith('v', Qt::CaseInsensitive)) s.remove(0, 1);
        const QStringList raw = s.split(QRegularExpression(QStringLiteral("[^0-9]+")), Qt::SkipEmptyParts);
        for (const QString &r : raw) out << r.toInt();
        return out;
    };
    const QList<int> x = parts(a), y = parts(b);
    for (int i = 0; i < qMax(x.size(), y.size()); ++i) {
        const int xi = i < x.size() ? x[i] : 0, yi = i < y.size() ? y[i] : 0;
        if (xi != yi) return xi > yi;
    }
    return false;
}

void UpdateManager::setState(const QString &s)
{
    if (m_state == s) { emit stateChanged(); return; }
    m_state = s;
    emit stateChanged();
}

void UpdateManager::fail(const QString &code, const QString &detail)
{
    qWarning().noquote() << "[update] error:" << code << detail;
    m_errCode = code;
    m_errDetail = detail;
    setState(QStringLiteral("error"));
}

void UpdateManager::check()
{
    if (m_state == QLatin1String("checking") || m_state == QLatin1String("downloading")) return;
    m_errCode.clear(); m_errDetail.clear();
    setState(QStringLiteral("checking"));

    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(kRepo)));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("VoltaOS/") + currentVersion());
    req.setTransferTimeout(15000);
    m_reply = m_net->get(req);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateManager::onCheckFinished);
}

void UpdateManager::onCheckFinished()
{
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    r->deleteLater();

    const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http == 404) { fail(QStringLiteral("norelease")); return; }
    if (r->error() != QNetworkReply::NoError) { fail(QStringLiteral("network"), r->errorString()); return; }

    const QJsonObject rel = QJsonDocument::fromJson(r->readAll()).object();
    const QString tag = rel.value(QStringLiteral("tag_name")).toString();
    if (tag.isEmpty()) { fail(QStringLiteral("unknown"), QStringLiteral("no tag_name in the answer")); return; }

    m_latest = tag.startsWith('v', Qt::CaseInsensitive) ? tag.mid(1) : tag;
    m_notes = rel.value(QStringLiteral("body")).toString().trimmed();
    m_assetUrl.clear(); m_assetSize = 0; m_assetSha256.clear();
    for (const QJsonValue &v : rel.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        const QString name = a.value(QStringLiteral("name")).toString();
        if (!name.endsWith(QLatin1String(".AppImage"), Qt::CaseInsensitive)) continue;
        if (!name.contains(QLatin1String("x86_64"))) continue;
        m_assetUrl = QUrl(a.value(QStringLiteral("browser_download_url")).toString());
        m_assetSize = qint64(a.value(QStringLiteral("size")).toDouble());
        const QString digest = a.value(QStringLiteral("digest")).toString();   // "sha256:<hex>"
        if (digest.startsWith(QLatin1String("sha256:"))) m_assetSha256 = digest.mid(7).toLatin1().toLower();
        break;
    }

    qDebug().noquote() << "[update] installed" << currentVersion() << "- latest" << m_latest;
    if (!isNewer(m_latest, currentVersion())) { setState(QStringLiteral("uptodate")); return; }
    setState(QStringLiteral("available"));
}

void UpdateManager::install()
{
    if (m_state != QLatin1String("available")) return;
    if (!canInstall()) { fail(QStringLiteral("unknown"), QStringLiteral("not running from a writable AppImage")); return; }
    if (!m_assetUrl.isValid() || m_assetUrl.scheme() != QLatin1String("https")) {
        fail(QStringLiteral("noasset")); return;
    }

    const QString target = appImagePath();
    const QFileInfo ti(target);
    // room for the download (the old file stays until the new one is complete)
    if (m_assetSize > 0 && QStorageInfo(ti.absolutePath()).bytesAvailable() < m_assetSize + (64ll << 20)) {
        fail(QStringLiteral("nospace")); return;
    }

    m_part.setFileName(target + QStringLiteral(".download"));
    if (!m_part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("write"), m_part.errorString()); return;
    }
    m_hash.reset();
    m_cancelled = false;
    m_progress = 0;
    emit progressChanged();
    m_errCode.clear(); m_errDetail.clear();
    setState(QStringLiteral("downloading"));

    QNetworkRequest req(m_assetUrl);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("VoltaOS/") + currentVersion());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);   // no data for 30 s = connection lost
    m_reply = m_net->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateManager::onDownloadReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        const qint64 t = total > 0 ? total : m_assetSize;
        m_progress = t > 0 ? qBound(0.0, double(got) / double(t), 1.0) : 0.0;
        emit progressChanged();
    });
    connect(m_reply, &QNetworkReply::finished, this, &UpdateManager::onDownloadFinished);
}

void UpdateManager::onDownloadReadyRead()
{
    if (!m_reply) return;
    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty()) return;
    m_hash.addData(chunk);
    if (m_part.write(chunk) != chunk.size()) {
        const QString why = m_part.errorString();
        m_reply->abort();            // onDownloadFinished reports the failure
        m_errDetail = why;
    }
}

void UpdateManager::discardPartial()
{
    if (m_part.isOpen()) m_part.close();
    QFile::remove(m_part.fileName());
}

void UpdateManager::onDownloadFinished()
{
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    r->deleteLater();
    const QByteArray rest = r->readAll();
    if (!rest.isEmpty()) { m_hash.addData(rest); m_part.write(rest); }
    m_part.flush();
    const qint64 written = m_part.size();
    m_part.close();

    if (m_cancelled) { discardPartial(); m_progress = 0; emit progressChanged(); setState(QStringLiteral("available")); return; }
    if (r->error() != QNetworkReply::NoError) {
        const QString d = m_errDetail.isEmpty() ? r->errorString() : m_errDetail;
        discardPartial();
        fail(m_errDetail.isEmpty() ? QStringLiteral("network") : QStringLiteral("write"), d);
        return;
    }

    // ── checks: the file must be what GitHub says it is, and an AppImage ──
    bool ok = true;
    QString why;
    if (m_assetSize > 0 && written != m_assetSize) { ok = false; why = QStringLiteral("size %1 != %2").arg(written).arg(m_assetSize); }
    if (ok && !m_assetSha256.isEmpty() && m_hash.result().toHex() != m_assetSha256) { ok = false; why = QStringLiteral("sha256 does not match"); }
    if (ok) {
        QFile f(m_part.fileName());
        QByteArray head;
        if (f.open(QIODevice::ReadOnly)) head = f.read(11);
        // ELF header + "AI\x02" (AppImage type 2) at offset 8
        if (head.size() < 11 || !head.startsWith("\x7f" "ELF") || head.mid(8, 3) != QByteArray("AI\x02", 3)) {
            ok = false; why = QStringLiteral("not an AppImage");
        }
    }
    if (!ok) { discardPartial(); fail(QStringLiteral("badfile"), why); return; }

    QFile::setPermissions(m_part.fileName(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);

    // Atomic replacement: this process keeps running from the old file (still open), the
    // next launch finds the new one. rename(2) overwrites the destination.
    if (std::rename(m_part.fileName().toLocal8Bit().constData(), appImagePath().toLocal8Bit().constData()) != 0) {
        const QString d = QString::fromLocal8Bit(strerror(errno));
        discardPartial();
        fail(QStringLiteral("write"), d);
        return;
    }
    qDebug().noquote() << "[update] installed" << m_latest << "->" << appImagePath();
    m_progress = 1; emit progressChanged();
    setState(QStringLiteral("ready"));
}

void UpdateManager::cancel()
{
    if (m_state != QLatin1String("downloading") || !m_reply) return;
    m_cancelled = true;
    m_reply->abort();
}

void UpdateManager::restart()
{
    if (m_state != QLatin1String("ready")) return;
    const QString img = appImagePath();
    // The new instance waits for this one to exit (services, ports and the copied scripts
    // in the data folder are released) and then starts from the replaced file.
    QProcess p;
    p.setProgram(QStringLiteral("/bin/sh"));
    p.setArguments({QStringLiteral("-c"),
                    QStringLiteral("n=0; while kill -0 \"$1\" 2>/dev/null && [ $n -lt 40 ]; do sleep 0.5; n=$((n+1)); done; exec \"$2\""),
                    QStringLiteral("sh"), QString::number(QCoreApplication::applicationPid()), img});
    p.setStandardInputFile(QProcess::nullDevice());
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    p.setChildProcessModifier([] { ::setsid(); });   // survives the quit of this process
    if (!p.startDetached()) { fail(QStringLiteral("unknown"), p.errorString()); return; }
    QCoreApplication::quit();
}
