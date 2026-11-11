#include <QLoggingCategory>
#include <execinfo.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include "LanguageManager.h"
#include "CarModelManager.h"
#include "ServiceManager.h"
#include <QQmlContext>
#include <QCoreApplication>
#include <QProcess>
#include <QSettings>
#include <QDir>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QFileInfo>
#include <QFile>
#include <QUrl>
#include <QDebug>
#include <QQuickWindow>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QtWebEngineQuick>
#include "PhoneManager.h"
#include "NavigationManager.h"
#include "GpsService.h"
#include "CarlinkDongle.h"
#include "SetupManager.h"
#include "UpdateManager.h"
#include "MprisManager.h"
#include "FileHelper.h"
#ifdef VOLTA_HAS_DIRTYPROBE
#include "DirtyProbe.h"
#endif
#include <QLibrary>

// Project folder (Python scripts, assets, keys.env). Order:
//   1. variabile d'ambiente VOLTA_DIR
//   2. going up from the executable's folder (build/<config>/VoltaOS -> ../..)
//   3. ~/volta_os
// It is recognized by the file services/map_proxy.py.
static QString projectDir()
{
    static QString cached;
    if (!cached.isEmpty()) return cached;
    QStringList candidates;
    const QString env = qEnvironmentVariable("VOLTA_DIR");
    if (!env.isEmpty()) candidates << env;
    const QString exe = QCoreApplication::applicationDirPath();
    candidates << exe << exe + "/.." << exe + "/../.." << exe + "/../../.."
               << QDir::homePath() + "/volta_os";
    for (const QString &c : candidates) {
        const QString d = QDir::cleanPath(c);
        if (QFileInfo::exists(d + "/services/map_proxy.py")) {
            cached = d;
            qDebug().noquote() << "[project] folder:" << cached;
            return cached;
        }
    }
    qWarning() << "[project] project folder not found: set VOLTA_DIR";
    cached = exe;
    return cached;
}

// Servizi Python: stanno in services/
static QString findScript(const QString &name)
{
    const QString p = projectDir() + "/services/" + name;
    if (QFileInfo::exists(p)) return p;
    qWarning().noquote() << "[findScript] Not found:" << p;
    return {};
}

// Loads the API keys from keys.env (see keys.env.example) into the
// environment variables: NavigationManager reads them and the Python
// scripts started from here inherit them. A variable already set wins over the file.
static void loadKeysFile()
{
    QString path = projectDir() + "/keys.env";
    if (!QFileInfo::exists(path) && QFileInfo::exists(projectDir() + "/chiavi.env"))
        path = projectDir() + "/chiavi.env";   // legacy name
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[keys] keys.env not found: copy keys.env.example to keys.env";
        return;
    }
    int n = 0;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QByteArray key = line.left(eq).trimmed().toUtf8();
        QString val = line.mid(eq + 1).trimmed();
        if (val.size() >= 2 && (val.startsWith('"') || val.startsWith('\'')) && val.endsWith(val.at(0)))
            val = val.mid(1, val.size() - 2);
        if (val.isEmpty() || qEnvironmentVariableIsSet(key.constData())) continue;
        qputenv(key.constData(), val.toUtf8());
        ++n;
    }
    qDebug() << "[keys] loaded" << n << "keys from" << path;
}

static void startNodeServer(QObject *parent)
{
    QStringList candidates = {
        QDir::homePath() + "/Documenti/volta-map-only",
        QDir::homePath() + "/volta-map-only",
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path + "/main.cjs")) {
            auto *proc = new QProcess(parent);
            proc->setWorkingDirectory(path);
            proc->start("node", {path + "/main.cjs"});
            qDebug() << "[node-server] Started WS server from" << path;
            return;
        }
    }
}

// ── Service supervision ────────────────────────────────────────
// If a service dies it is restarted with increasing waits (3,6,12,24,48,60 s):
// a real fault no longer generates a restart every 3 seconds forever, but a network
// blip still recovers in a few seconds. If the service stays alive for more than
// 60 s the counter starts again from zero.
struct SvcState { int fails = 0; qint64 startedAt = 0; };
static QHash<QString, SvcState> g_svc;
static bool g_quitting = false;   // true from when the app is closing (UI thread only)

static void svcMarkStarted(const QString &key)
{
    g_svc[key].startedAt = QDateTime::currentMSecsSinceEpoch();
}
static int svcNextDelayMs(const QString &key)
{
    SvcState &s = g_svc[key];
    const qint64 up = QDateTime::currentMSecsSinceEpoch() - s.startedAt;
    s.fails = (up > 60000) ? 1 : s.fails + 1;
    return qMin(60000, 3000 * (1 << qMin(s.fails - 1, 5)));
}
static bool appIsClosing() { return g_quitting || QCoreApplication::closingDown(); }

// Only OUR scripts: the command line must start with the interpreter we use.
// (a "nano map_proxy.py" or a service launched by hand by you in a terminal are not touched)
static const QString kOurScripts =
    QStringLiteral("^/usr/bin/python3 [^ ]*/(map_service|map_proxy|ytmusic_scraper_service|"
                   "spotify_scraper_service|phone_service|network_settings_service|"
                   "podcast_service)\\.py$");   // ALL those started with startPy()
static const QString kOurMpv = QStringLiteral("--input-ipc-server=/tmp/volta_mpv.sock");

static void killStaleOurProcesses()
{
    QProcess::execute("pkill", {"-f", kOurScripts});
    QProcess::execute("pkill", {"-f", "--", kOurMpv});
    QProcess::execute("pkill", {"-x", "spotifyd"});
    QFile::remove("/tmp/volta_mpv.sock");
}

static void startPy(const QString &name, QObject *parent)
{
    QString path = findScript(name);
    if (path.isEmpty()) return;
    auto *proc = new QProcess(parent);
    // ABSOLUTE path instead of the generic "python3" searched in PATH: if pyenv is installed
    // its shim may resolve to a different version from the one where the services'
    // packages are installed (ytmusicapi, spotipy, dbus_next, python-mpv, yt_dlp...).
    proc->setProgram("/usr/bin/python3");
    proc->setArguments({path});
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(QFileInfo(path).absolutePath());
    QObject::connect(proc, &QProcess::readyReadStandardOutput,
                     [proc, name]() {
                         QString out = proc->readAllStandardOutput();
                         // Skips the HTTP access log lines (Flask): dozens per second
                         QStringList keep;
                         for (const QString &ln : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                             if (!ln.contains(QLatin1String("HTTP/1.1\" ")) && !ln.contains(QLatin1String("HTTP/1.0\" ")))
                                 keep << ln;
                         out = keep.join(QLatin1Char('\n')).trimmed();
                         if (!out.isEmpty()) qDebug().noquote() << "[" + name + "]" << out;
                     });
    // Impossible to start (missing interpreter...): no point retrying, I write it only once.
    QObject::connect(proc, &QProcess::errorOccurred, [name, proc](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            qWarning().noquote() << "[main] start ERROR" << name << proc->errorString();
    });
    QObject::connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [name, parent, proc](int exitCode, QProcess::ExitStatus status) {
                         proc->deleteLater();                 // before, the dead ones stayed in memory
                         if (appIsClosing()) return;          // spegnimento in corso: niente riavvii
                         const int delay = svcNextDelayMs(name);
                         qWarning().noquote() << "[" + name + "] terminato (codice" << exitCode
                                               << ", stato" << status << ") — restarting in"
                                               << delay / 1000.0 << "s";
                         QTimer::singleShot(delay, parent, [name, parent]() {
                             if (appIsClosing()) return;
                             startPy(name, parent);
                         });
                     });
    svcMarkStarted(name);
    proc->start();
}

// spotifyd (Spotify Connect): supervised like the Python services.
static void startSpotifyd(QObject *parent)
{
    auto *p = new QProcess(parent);
    p->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(p, &QProcess::readyRead, [p]() {
        QByteArray out = p->readAll().trimmed();
        if (!out.isEmpty())
            qDebug().noquote() << "[spotifyd]" << out;
    });
    QObject::connect(p, &QProcess::started, []() { qDebug() << "[spotifyd] Avviato (Spotify Connect)"; });
    QObject::connect(p, &QProcess::errorOccurred, [](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            qWarning() << "[spotifyd] Not found — install spotifyd for Spotify playback";
    });
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [p, parent](int exitCode, QProcess::ExitStatus) {
                         p->deleteLater();
                         if (appIsClosing()) return;
                         const int delay = svcNextDelayMs(QStringLiteral("spotifyd"));
                         qWarning().noquote() << "[spotifyd] terminato (codice" << exitCode
                                               << ") — restarting in" << delay / 1000.0 << "s";
                         QTimer::singleShot(delay, parent, [parent]() {
                             if (!appIsClosing()) startSpotifyd(parent);
                         });
                     });
    svcMarkStarted(QStringLiteral("spotifyd"));
    // Same level as the other sources, MEASURED (same song): YT Music -14.1 LUFS; Spotify with pregain 0 -17.6, with +3.5 -14.5.
    // Initial volume at 100% (by default it is 90% on a logarithmic curve, about -6 dB).
    p->start("spotifyd", {"--no-daemon", "--volume-normalisation=true", "--normalisation-pregain=3.5",
                          "--initial-volume=100"});
}

// ── UI stutter probe (active only with VOLTA_STALL=1) ─────────────────────────
static std::atomic<long long> g_stallBeat{0};
static pthread_t g_stallMainThread;
static long long volta_nowMs()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
static void volta_stallSig(int)
{
    void *bt[48];
    int n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, 2);
}
static void volta_startStallProbe()
{
    g_stallMainThread = pthread_self();
    void *prime[4]; backtrace(prime, 4);   // loads libgcc outside the signal handler
    struct sigaction sa = {}; sa.sa_handler = volta_stallSig; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);
    g_stallBeat = volta_nowMs();
    auto *beat = new QTimer(qApp);
    beat->setTimerType(Qt::PreciseTimer);
    beat->setInterval(10);
    QObject::connect(beat, &QTimer::timeout, []() { g_stallBeat = volta_nowMs(); });
    beat->start();
    std::thread([]() {
        long long reported = 0;      // heartbeat value the stall in progress refers to
        bool second = false;
        for (;;) {
            usleep(10000);
            long long b = g_stallBeat.load();
            long long gap = volta_nowMs() - b;
            if (reported != 0 && b != reported) {
                fprintf(stderr, "[STALL] end: UI stalled for about %lld ms\n", b - reported);
                reported = 0;
            }
            if (gap > 60 && reported == 0) {
                reported = b; second = false;
                fprintf(stderr, "[STALL] UI stalled for >60 ms, UI thread stack:\n");
                pthread_kill(g_stallMainThread, SIGUSR2);
            } else if (gap > 200 && reported == b && !second) {
                second = true;
                fprintf(stderr, "[STALL] still stalled (>200 ms), second stack:\n");
                pthread_kill(g_stallMainThread, SIGUSR2);
            }
        }
    }).detach();
}


int main(int argc, char *argv[])
{
    qDebug() << "[T] main() entered";
    qputenv("QT_DEBUG_PLUGINS", "0");
    qputenv("QT_FATAL_WARNINGS", "0");
    // Forces XWayland ("xcb" platform) instead of native Wayland. The log
    // confirms the reason: "qtvirtualkeyboard currently is not supported at
    // client-side, use QT_IM_MODULES=qtvirtualkeyboard at compositor-side" —
    // under native Wayland a client app cannot activate its
    // own virtual keyboard by itself (that would be up to the compositor, i.e. GNOME).
    // The project already depends on X11 (find_package X11 in CMakeLists.txt),
    // so XWayland is available anyway on the GNOME session.
    // If the user has already set QT_QPA_PLATFORM from outside, respect it.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");
    // round72: under "xcb" by default Qt uses GLX for OpenGL ("xcb_glx"
    // integration), not EGL — confirmed in the log, ALWAYS present at every
    // avvio: "qt.waylandcompositor.hardwareintegration: Failed to
    // initialize EGL display. Could not get EglDisplay for window.". With
    // a GLX context there is no EGLDisplay to retrieve, so the
    // hardware integration of OUR compositor (needed for
    // zwp_linux_dmabuf_v1, one of the protocols required by gamescope, see
    // CMakeLists.txt/VoltaRelativePointer.h) falliva SEMPRE, a prescindere
    // from any protocol we implemented by hand. Verified that
    // Qt tells the two integrations apart exactly with this variable
    // (string "QT_XCB_GL_INTEGRATION" found in the system's real
    // libQt6XcbQpa.so library, with the two xcbglintegrations plugins/
    // libqxcb-egl-integration.so e libqxcb-glx-integration.so entrambi
    // present) — it is not a guessed variable. If the user has already
    // set QT_XCB_GL_INTEGRATION from outside, respect it, same principle
    // as QT_QPA_PLATFORM above.
    if (qEnvironmentVariableIsEmpty("QT_XCB_GL_INTEGRATION"))
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    // round72d: QT_XCB_GL_INTEGRATION=xcb_egl (above) worked — the log
    // no longer shows "Failed to initialize EGL display. Could not get
    // EglDisplay for window.", present at every single start before
    // this fix. BUT gamescope still got stuck identically on
    // "Couldn't create Wayland objects.": read the REAL source code
    // of Qt (qtwayland/src/compositor/compositor_api/qwaylandcompositor.cpp,
    // QWaylandCompositorPrivate::loadClientBufferIntegration()) ho trovato
    // the reason — Qt picks ONE compositor-side "client buffer integration", and
    // if QT_WAYLAND_CLIENT_BUFFER_INTEGRATION is not
    // set it ALWAYS uses "wayland-egl" when available (it is, now that
    // EGL works). The protocol zwp_linux_dmabuf_v1 however is not
    // provided by "wayland-egl" — it is a SEPARATE and alternative plugin,
    // "linux-dmabuf-unstable-v1" (same source code,
    // qtwayland/src/hardwareintegration/compositor/linux-dmabuf-unstable-v1/
    // linuxdmabuf.cpp line 16: "zwp_linux_dmabuf_v1(display, 3)" — version
    // 3, it also satisfies the "uVersion >= 3" check that I read in
    // gamescope's code), and Qt NEVER loads it by default together with
    // "wayland-egl": va richiesto esplicitamente. La variabile accetta
    // however a ";"-separated list (same file, loadClientBufferIntegration,
    // "clientBufferIntegration.split(';')") and loads EVERY integration
    // listed one it finds — it is not an exclusive OR. We therefore set
    // both: "wayland-egl" stays for everything that already works today
    // (Waydroid, Chrome, i browser incorporati, ecc.), "linux-dmabuf-unstable-v1"
    // is only added to expose the protocol that gamescope requires.
    if (qEnvironmentVariableIsEmpty("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION"))
        qputenv("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION", "wayland-egl;linux-dmabuf-unstable-v1");
    // Widevine DRM: copy to /tmp to avoid sandbox block
    const QString widevineSrc = "/opt/google/chrome/WidevineCdm/_platform_specific/linux_x64/libwidevinecdm.so";
    const QString widevineDst = "/tmp/libwidevinecdm.so";
    if (QFileInfo::exists(widevineSrc) && !QFileInfo::exists(widevineDst))
        QFile::copy(widevineSrc, widevineDst);

    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            "--disable-accelerated-video-decode "
            "--disable-features=WebContentsForceDark,ForcedColors "  // FIX colori YouTube invertiti
            "--force-color-profile=srgb "
            "--ignore-gpu-blocklist --enable-gpu-rasterization "
            "--disable-web-security --allow-running-insecure-content "
            "--no-sandbox "
            "--widevine-cdm-path=/tmp/libwidevinecdm.so "
            "--widevine-cdm-version=4.10.2710.0");

    QtWebEngineQuick::initialize();
    qDebug() << "[T] WebEngine initialized";

    // Enables Qt Virtual Keyboard's input engine for ANY text field
    // of the app (it must be set BEFORE creating
    // QGuiApplication). The visible keyboard is NO longer Qt's "official" one
    // (InputPanel + style) — it is hand-drawn in
    // qml/VoltaInputPanel.qml on the low-level components (InputContext/
    // InputEngine), so no style variable is needed anymore: this
    // env var only serves to register the input platform plugin.
    qputenv("QT_IM_MODULE", "qtvirtualkeyboard");

    QGuiApplication app(argc, argv);
    qDebug() << "[T] QGuiApplication ready";
    loadKeysFile();   // before NavigationManager and the Python scripts
    // Inter font (OFL license) for the whole UI (double path: the resource
    // prefix depends on the QML module's configuration).
    int fontId = -1;
    for (const char *w : {"Regular", "Medium", "SemiBold", "Bold"}) {
        int id = QFontDatabase::addApplicationFont(QStringLiteral(":/VoltaApp/fonts/Inter-%1.otf").arg(w));
        if (id < 0)
            id = QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/VoltaApp/fonts/Inter-%1.otf").arg(w));
        if (fontId < 0) fontId = id;
    }
    QString uiFamily = "Liberation Sans";
    if (fontId >= 0) {
        const auto fams = QFontDatabase::applicationFontFamilies(fontId);
        if (!fams.isEmpty()) uiFamily = fams.first();
    } else {
        qWarning() << "[Font] Inter not loaded, using the fallback";
    }
    QFont appFont(uiFamily, 11);
    appFont.setHintingPreference(QFont::PreferFullHinting);
    app.setFont(appFont);
    app.setApplicationName("Volta OS");
    app.setOrganizationName("VoltaOS");
    app.setDesktopFileName("VoltaOS");  // silences the qt.positioning.geoclue2 warning
    app.setOrganizationDomain("voltaqt.local");
    {   // chosen language: the Python services read it from VOLTA_LANG (YT Music, podcast, voice content...)
        const QString l = QSettings().value("ui/language", "en").toString();
        qputenv("VOLTA_LANG", l.toUtf8());
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    // Threaded rendering: the scene is composed on a dedicated thread, without
    // blocking the UI thread (more responsive input, smoother animations)
    qputenv("QSG_RENDER_LOOP", "threaded");
    // Optimizes the compositor: always use the full updated buffer
    qputenv("QT_QUICK_CONTROLS_HOVER_ENABLED", "0");
    // Log: here and not with QT_LOGGING_RULES (at this point it would no longer be read).
    // VOLTA_DEBUG=1 re-enables the diagnostic messages (FPS, 3D materials, keyboard,
    // JavaScript console of web pages like Zoom/YouTube...).
    QString logRules = QStringLiteral("qt.qml.diskcache.debug=false\nqt.qml.gc.statistics=false\n");
    if (!qEnvironmentVariableIsSet("VOLTA_DEBUG"))
        logRules += QStringLiteral("js=false\n");   // web page console: noise, not our errors
    // Performance measurement (only with VOLTA_PERF=1): timing of every scene graph frame
    if (qEnvironmentVariableIsSet("VOLTA_PERF"))
        logRules += QStringLiteral("qt.scenegraph.time.renderloop=true\n"
                                   "qt.scenegraph.time.renderer=true\n"
                                   "qt.scenegraph.time.texture=true\n");
    QLoggingCategory::setFilterRules(logRules);

    if (qEnvironmentVariableIsSet("VOLTA_STALL"))
        volta_startStallProbe();

    // Certificate verification ACTIVE (Qt's default). With VOLTA_SSL_INSECURE=1 we go back to the
    // old behavior (no verification) without recompiling, to diagnose problems.
    if (qEnvironmentVariableIsSet("VOLTA_SSL_INSECURE")) {
        qWarning() << "[TLS] WARNING: certificate verification DISABLED (VOLTA_SSL_INSECURE)";
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        config.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslConfiguration::setDefaultConfiguration(config);
    } else {
        // Loading the system certificates is costly: I do it on a separate thread,
        // so it does not block the interface startup.
        std::thread([]() {
            (void) QSslConfiguration::defaultConfiguration();
            (void) QSslConfiguration::systemCaCertificates();
        }).detach();
    }

    // ── Managers: launch and track the native apps and the services ────────────────────────────
    PhoneManager phoneManager;
    NavigationManager navManager;
    CarlinkDongle carlink;          // CarPlay / Android Auto via dongle Carlinkit (src/carlink)
    GpsService gps;                 // serial GPS receiver (before: services/map_service.py)
    SetupManager setup(projectDir(), &navManager);   // guided setup (keys.env)
    setup.logMissingDependencies();

    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&app]() {
        g_quitting = true;
        // 1) Stops all the child processes: first the SIGTERM to all together, then a single
        //    wait (max 1.5 s in total). Before, it waited up to 1 s for each one.
        const auto kids = app.findChildren<QProcess *>();
        for (QProcess *p : kids)
            if (p->state() != QProcess::NotRunning) p->terminate();
        QElapsedTimer t; t.start();
        for (QProcess *p : kids) {
            if (p->state() == QProcess::NotRunning) continue;
            const int left = qMax(50, 1500 - int(t.elapsed()));
            if (!p->waitForFinished(left)) p->kill();
        }
        // 2) Fallback ONLY for our processes that are not direct children
        //    (e.g. the mpv started by ytmusic_service.py): no more generic "pkill -x mpv".
        killStaleOurProcesses();
        qDebug() << "[main] servizi terminati";
    });

    // Leftovers of a previous session that ended badly (crash, kill): a single quick pkill,
    // only on our scripts. Before there were two bash commands that forcibly freed ports
    // 5174/8080/8099/9010/9999: they blocked the UI and could kill your own programs on those ports.
    killStaleOurProcesses();

    // spotifyd after 1.5 s (staged start)
    QTimer::singleShot(1500, &app, [&app]() { startSpotifyd(&app); });

    startPy("map_proxy.py",                &app);  // Mapbox/satellite style for MapLibre (port 9098) — the only maps script left, before 3 started racing on the same port
    // YT Music, Spotify and podcasts start ONLY when needed (ServiceManager::ensure, called by the UI when
    // the app is opened or a track starts). At startup only what is needed to resume the last listening is launched.
    auto *services = new ServiceManager([&app](const QString &script) { startPy(script, &app); }, &app);
    {
        QSettings qs;
        const bool resume = qs.value("ui/resumePlayback", true).toBool();
        const QString last = qs.value("playback/lastProvider").toString();
        if (resume && last == QLatin1String("ytmusic")) {
            services->markStarted("ytmusic");
            QTimer::singleShot(4000, &app, [&app]() { startPy("ytmusic_scraper_service.py", &app); });
        }
        if (resume && last == QLatin1String("spotify")) {
            services->markStarted("spotify");
            QTimer::singleShot(4000, &app, [&app]() { startPy("spotify_scraper_service.py", &app); });
        }
    }
    QTimer::singleShot(2000, &app, [&app]() { startPy("phone_service.py", &app); });  // Bluetooth: contatti/chiamate/SMS via D-Bus (com.volta.Phone)
    startPy("network_settings_service.py", &app);  // Real Wi-Fi/Bluetooth for the Settings (port 9097)
    startNodeServer(&app);

    // Created BEFORE the QML engine (which uses it) so it is destroyed AFTER.
    // System player (MPRIS over D-Bus): YT Music (mpv), Spotify (spotifyd),
    // phone over Bluetooth (mpris-proxy) — music state and commands
    MprisManager mpris;
    mpris.ensureBluetoothProxy();

    qDebug() << "[T] services launched, creating the QML engine";
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("PhoneManager", &phoneManager);
    engine.rootContext()->setContextProperty("Nav", &navManager);
    engine.rootContext()->setContextProperty("Gps", &gps);
    engine.rootContext()->setContextProperty("Carlink", &carlink);
    engine.rootContext()->setContextProperty("Setup", &setup);
    static UpdateManager updates;   // Settings > Updates (new AppImage from GitHub)
    engine.rootContext()->setContextProperty("Updates", &updates);
    static CarModelManager carModels(projectDir());   // auto 3D (.glb) dell'utente
    engine.rootContext()->setContextProperty("CarModels", &carModels);
    if (qEnvironmentVariableIsSet("VOLTA_CARDEBUG")) { const auto r = carModels.inspect(carModels.carFile()); qDebug() << "[auto]" << carModels.carFile() << r["ok"] << r["error"] << r["count"] << r["parts"]; }
    LanguageManager lingua(&engine);   // lingua dell'interfaccia (traduzioni in i18n/)
    engine.rootContext()->setContextProperty("Lang", &lingua);
    engine.rootContext()->setContextProperty("Services", services);
    engine.rootContext()->setContextProperty("Mpris", &mpris);
    // Project folder for QML: appDir = path ("/.../volta_os"),
    // appUrl = the same as a URL ("file:///.../volta_os"), homeDir = the home.
    // Assets on disk in assets/, generated data in data/, scripts in scripts/.
    engine.rootContext()->setContextProperty("appDir", projectDir());
    engine.rootContext()->setContextProperty("appUrl", QUrl::fromLocalFile(projectDir()).toString());
    engine.rootContext()->setContextProperty("homeDir", QDir::homePath());
    // debugMode (VOLTA_DEBUG=1): enables the diagnostic console.log in QML
    engine.rootContext()->setContextProperty("debugMode", qEnvironmentVariableIsSet("VOLTA_DEBUG"));
    // tests/screenshots: VOLTA_OPEN=app[:page] opens an app at startup (e.g. settings:Arcade)
    engine.rootContext()->setContextProperty("debugOpen", qEnvironmentVariable("VOLTA_OPEN"));
    static FileHelper fileHelper;
    engine.rootContext()->setContextProperty("Files", &fileHelper);

    // MapLibre QML plugin: first the one included in third_party/maplibre (precompiled),
    // otherwise the one installed on the system in /usr/local/qml.
    {
        const QString ml = projectDir() + "/third_party/maplibre";
        if (QFileInfo::exists(ml + "/qml/MapLibre/qmldir")) {
            // the libraries must be loaded first, with the full path
            // The plugin's libraries have no search path of their own: their dependencies (Qt SQL from the
            // AppImage, ICU 76 next to them) must already be loaded, otherwise on a system without Qt
            // the map does not start. Files that do not exist are skipped (normal installations).
            const QString appLib = QCoreApplication::applicationDirPath() + "/../lib/";
            for (const QString &dep : {appLib + "libQt6Sql.so.6", appLib + "libQt6Positioning.so.6",
                                       appLib + "libQt6Location.so.6",
                                       ml + "/lib/libicudata.so.76", ml + "/lib/libicuuc.so.76",
                                       ml + "/lib/libicui18n.so.76"}) {
                if (QFileInfo::exists(dep)) QLibrary(dep).load();
            }
            for (const char *lib : {"libQMapLibre.so.4", "libQMapLibreQuickPrivate.so.4", "libQMapLibreLocation.so.4"}) {
                QLibrary l(ml + "/lib/" + lib);
                l.setLoadHints(QLibrary::ExportExternalSymbolsHint);
                if (!l.load()) qWarning().noquote() << "[maplibre] library not loaded:" << l.errorString();
            }
            QCoreApplication::addLibraryPath(ml + "/plugins");
            engine.addImportPath(ml + "/qml");
            qDebug().noquote() << "[maplibre] using the bundled version:" << ml;
        }
        engine.addImportPath("/usr/local/qml");
    }

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    // It was 1000 ms: one second of black screen at every start. Now it starts right away
    // (the Python services start in parallel, the dashboards retry by themselves).
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                     [](QObject *obj, const QUrl &) {
        qDebug() << "[T] QML root object created";
        if (auto *w = qobject_cast<QQuickWindow *>(obj)) {
            QObject::connect(w, &QQuickWindow::frameSwapped, w, []() {
                static bool done = false;
                if (!done) { done = true; qDebug() << "[T] FIRST FRAME shown"; }
            });
            // VOLTA_DIRTY=1: what makes the UI redraw (see DirtyProbe.h)
            #ifdef VOLTA_HAS_DIRTYPROBE
            if (qEnvironmentVariableIsSet("VOLTA_DIRTY")) new DirtyProbe(w, obj);
#endif
            // VOLTA_SHOT=file.png[:ms] saves a screenshot after ms (default 9000): for tests
            const QString shot = qEnvironmentVariable("VOLTA_SHOT");
            if (!shot.isEmpty()) {
                const int c = shot.lastIndexOf(QLatin1Char(':'));
                const QString file = c > 0 ? shot.left(c) : shot;
                const int ms = c > 0 ? shot.mid(c + 1).toInt() : 9000;
                QTimer::singleShot(ms, w, [w, file]() { w->grabWindow().save(file); qInfo().noquote() << "[shot]" << file; });
            }
        }
    });
    QTimer::singleShot(0, [&engine]() {
        qDebug() << "[T] loadFromModule start";
        engine.loadFromModule("VoltaApp", "Main");
        qDebug() << "[T] loadFromModule end";
    });

    return app.exec();
}