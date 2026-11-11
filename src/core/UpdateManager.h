#pragma once
// UpdateManager — behind Settings > Updates.
//  - asks GitHub for the latest published release and compares it with the running version
//  - downloads the new AppImage next to the current one, checks it (size, SHA-256 when GitHub
//    gives it, AppImage signature) and only then replaces the old file with an atomic rename,
//    so an interrupted or corrupt download never breaks the installed copy
//  - restart() relaunches the new AppImage once this process has exited
// Only an AppImage can update itself (the APPIMAGE variable is set by the AppImage runtime);
// a source build just reports that a new version exists and links to the release page.
// Errors are reported as a short code (errorCode): the QML turns it into a translated message.
#include <QObject>
#include <QString>
#include <QUrl>
#include <QFile>
#include <QCryptographicHash>

class QNetworkAccessManager;
class QNetworkReply;

class UpdateManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    // idle | checking | uptodate | available | downloading | ready | error
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
    Q_PROPERTY(QString notes READ notes NOTIFY stateChanged)
    Q_PROPERTY(qint64 downloadSize READ downloadSize NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // network | norelease | noasset | nospace | write | badfile | unknown
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(QString errorDetail READ errorDetail NOTIFY stateChanged)
    // true when this copy is an AppImage in a folder we can write to
    Q_PROPERTY(bool canInstall READ canInstall CONSTANT)
    Q_PROPERTY(QString releasePage READ releasePage CONSTANT)
public:
    explicit UpdateManager(QObject *parent = nullptr);

    QString currentVersion() const;
    QString state() const { return m_state; }
    QString latestVersion() const { return m_latest; }
    QString notes() const { return m_notes; }
    qint64 downloadSize() const { return m_assetSize; }
    double progress() const { return m_progress; }
    QString errorCode() const { return m_errCode; }
    QString errorDetail() const { return m_errDetail; }
    bool canInstall() const;
    QString releasePage() const;

    /// Asks GitHub for the latest release (ignored while checking or downloading)
    Q_INVOKABLE void check();
    /// Downloads and installs the update found by check()
    Q_INVOKABLE void install();
    /// Stops a running download and removes the partial file
    Q_INVOKABLE void cancel();
    /// Relaunches the updated AppImage and quits this one (state must be "ready")
    Q_INVOKABLE void restart();

    /// true if version a is newer than b ("v1.2.0" > "1.1.9"); public for tests
    static bool isNewer(const QString &a, const QString &b);

signals:
    void stateChanged();
    void progressChanged();

private:
    void setState(const QString &s);
    void fail(const QString &code, const QString &detail = QString());
    void onCheckFinished();
    void onDownloadReadyRead();
    void onDownloadFinished();
    void discardPartial();
    QString appImagePath() const;

    QNetworkAccessManager *m_net;
    QNetworkReply *m_reply = nullptr;
    QString m_state = QStringLiteral("idle");
    QString m_latest, m_notes, m_errCode, m_errDetail;
    QUrl m_assetUrl;
    qint64 m_assetSize = 0;
    QByteArray m_assetSha256;       // hex, empty if GitHub does not provide it
    double m_progress = 0;
    QFile m_part;                   // "<AppImage>.download" while downloading
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    bool m_cancelled = false;
};
