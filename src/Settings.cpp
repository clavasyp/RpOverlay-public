#include "Settings.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>

static AudioSettings g_audio;
static WindowSettings g_window;
static ServerSettings g_server;
static LeadershipSettings g_leadership;  // выбор проекта/сервера свода «Обзвона»
static bool g_autoUpdate = true;   // авто-обновление при запуске
static bool g_overlayAutostart = false;  // бар сразу при старте (старое поведение ≤0.3.x); дефолт — хаб первым
static double g_localDiarThreshold = 1.1;  // порог кластеризации диаризации (ниже → больше спикеров)
static QString g_uiTheme = QStringLiteral("majestic");  // тема (только для окна настроек, см. src/Theme.*)
static bool g_debugMode = false;   // режим отладки (консоль + лог-файл)
static int g_contextLimit = 10;    // память диалога: последних сообщений (0..10), дефолт 10
static QString g_autoAdviceSensitivity = QStringLiteral("normal"); // чувствительность: quiet|normal|active
static QString g_adviceModelTier = QStringLiteral("auto");         // A/B модель совета: auto|flash|lite
static QString g_modelsPath;   // каталог STT-моделей ("" = дефолт: папка установки, см. ModelManager)
static bool g_loaded = false;

// --- Канал связи (основной/резервный через RU-прокладку) ---
static QString g_connMode = QStringLiteral("auto");  // auto|main|relay
static QString g_connLast;                           // main|relay|"" (итог прошлой авто-пробы)

static QString settingsFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/settings.json";
}

static void ensureLoaded()
{
    if (g_loaded)
        return;
    g_loaded = true;

    g_audio.micDeviceId = QString();
    g_audio.gameProcessExe = QString();

    QFile f(settingsFilePath());
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.contains("micDeviceId"))
            g_audio.micDeviceId = o.value("micDeviceId").toString(g_audio.micDeviceId);
        if (o.contains("gameProcessExe"))
            g_audio.gameProcessExe = o.value("gameProcessExe").toString(g_audio.gameProcessExe);
        if (o.contains("autoUpdate"))
            g_autoUpdate = o.value("autoUpdate").toBool(g_autoUpdate);
        if (o.contains("overlayAutostart"))
            g_overlayAutostart = o.value("overlayAutostart").toBool(g_overlayAutostart);
        if (o.contains("localDiarThreshold"))
            // верх 1.15: ≥1.2 роняет диаризатор sherpa на многоголосой сцене (см. SherpaPipeline::diarizeGuarded)
            g_localDiarThreshold = qBound(0.5, o.value("localDiarThreshold").toDouble(g_localDiarThreshold), 1.15);
        if (o.contains("theme"))
            g_uiTheme = o.value("theme").toString(g_uiTheme);
        if (o.contains("debug"))
            g_debugMode = o.value("debug").toBool(g_debugMode);
        if (o.contains("contextLimit"))
            g_contextLimit = qBound(0, o.value("contextLimit").toInt(g_contextLimit), 10);

        // Окно — вложенный объект "window". Ключи старых сборок (w/h/collapsed*) игнорируются
        // молча — бар 0.3.0 их не использует.
        const QJsonObject w = o.value("window").toObject();
        if (!w.isEmpty()) {
            g_window.opacity      = w.value("opacity").toDouble(g_window.opacity);
            g_window.fontScalePct = w.value("fontScalePct").toInt(g_window.fontScalePct);
            g_window.hasGeometry  = w.value("hasGeometry").toBool(g_window.hasGeometry);
            g_window.x            = w.value("x").toInt(g_window.x);
            g_window.y            = w.value("y").toInt(g_window.y);
            g_window.monitorIndex = w.value("monitorIndex").toInt(g_window.monitorIndex);
            g_window.captureInvisible = w.value("captureInvisible").toBool(g_window.captureInvisible);
            g_window.stealthMode      = w.value("stealthMode").toBool(g_window.stealthMode);
            g_window.hkToggle       = w.value("hkToggle").toString(g_window.hkToggle);
            g_window.hkClickThrough = w.value("hkClickThrough").toString(g_window.hkClickThrough);
            g_window.hkAsk          = w.value("hkAsk").toString(g_window.hkAsk);
            g_window.hkCollapse     = w.value("hkCollapse").toString(g_window.hkCollapse);
            g_window.hkPushToTalk   = w.value("hkPushToTalk").toString(g_window.hkPushToTalk);
            g_window.hkLocalRecord  = w.value("hkLocalRecord").toString(g_window.hkLocalRecord);
            g_window.hkLocalAnswer  = w.value("hkLocalAnswer").toString(g_window.hkLocalAnswer);
            // Кламп как у слайдера диалога (5..120): файл правят и руками, а в triggerSceneAnalysis
            // localWindowSec * 16000 — знаковое int-умножение (>134217 — signed overflow/UB;
            // отрицательное молча брало бы весь буфер). Симметрично qBound у localDiarThreshold.
            g_window.localWindowSec = qBound(5, w.value("localWindowSec").toInt(g_window.localWindowSec), 120);
        }

        // Сервер/проект/режим — вложенный объект "server".
        const QJsonObject s = o.value("server").toObject();
        if (!s.isEmpty()) {
            g_server.projectId   = s.value("projectId").toString(g_server.projectId);
            g_server.serverId    = s.value("serverId").toString(g_server.serverId);
            g_server.defaultMode = s.value("defaultMode").toString(g_server.defaultMode);
        }

        // Свод «Обзвона» (проект/сервер) — вложенный объект "leadership".
        const QJsonObject ld = o.value("leadership").toObject();
        if (!ld.isEmpty()) {
            g_leadership.projectId = ld.value("projectId").toString(g_leadership.projectId);
            g_leadership.serverId  = ld.value("serverId").toString(g_leadership.serverId);
        }

        // Канал связи — вложенный объект "connection".
        const QJsonObject d = o.value("connection").toObject();
        if (!d.isEmpty()) {
            g_connMode = d.value("mode").toString(g_connMode);
            g_connLast = d.value("last").toString(g_connLast);
        }

        // Авто-советы (сценовые режимы) — вложенный объект "autoAdvice".
        const QJsonObject aa = o.value("autoAdvice").toObject();
        if (!aa.isEmpty())
            g_autoAdviceSensitivity = aa.value("sensitivity").toString(g_autoAdviceSensitivity);
        if (o.contains("adviceModel"))
            g_adviceModelTier = o.value("adviceModel").toString(g_adviceModelTier);
        g_modelsPath = o.value("modelsPath").toString(g_modelsPath);
    }
}

namespace settings {

const AudioSettings &audio()
{
    ensureLoaded();
    return g_audio;
}

void setMicDeviceId(const QString &id)
{
    ensureLoaded();
    g_audio.micDeviceId = id;
    save();
}

void setGameProcessExe(const QString &exe)
{
    ensureLoaded();
    g_audio.gameProcessExe = exe;
    save();
}

const WindowSettings &window()
{
    ensureLoaded();
    return g_window;
}

void setWindow(const WindowSettings &w)
{
    ensureLoaded();
    g_window = w;
    save();
}

const ServerSettings &server()
{
    ensureLoaded();
    return g_server;
}

void setServer(const ServerSettings &s)
{
    ensureLoaded();
    g_server = s;
    save();
}

const LeadershipSettings &leadership()
{
    ensureLoaded();
    return g_leadership;
}

void setLeadership(const LeadershipSettings &l)
{
    ensureLoaded();
    g_leadership = l;
    save();
}

bool autoUpdateEnabled()
{
    ensureLoaded();
    return g_autoUpdate;
}

void setAutoUpdate(bool on)
{
    ensureLoaded();
    g_autoUpdate = on;
    save();
}

bool overlayAutostart()
{
    ensureLoaded();
    return g_overlayAutostart;
}

void setOverlayAutostart(bool on)
{
    ensureLoaded();
    g_overlayAutostart = on;
    save();
}

double localDiarThreshold()
{
    ensureLoaded();
    return g_localDiarThreshold;
}

void setLocalDiarThreshold(double t)
{
    ensureLoaded();
    g_localDiarThreshold = qBound(0.5, t, 1.15);   // верх 1.15: выше — краш-зона sherpa (diarizeGuarded)
    save();
}

QString uiTheme()
{
    ensureLoaded();
    return g_uiTheme;
}

void setUiTheme(const QString &name)
{
    ensureLoaded();
    g_uiTheme = name;
    save();
}

bool debugMode()
{
    ensureLoaded();
    return g_debugMode;
}

void setDebugMode(bool on)
{
    ensureLoaded();
    g_debugMode = on;
    save();
}

int contextLimit()
{
    ensureLoaded();
    return g_contextLimit;
}

void setContextLimit(int n)
{
    ensureLoaded();
    g_contextLimit = qBound(0, n, 10);
    save();
}

QString autoAdviceSensitivity()
{
    ensureLoaded();
    return g_autoAdviceSensitivity;
}

void setAutoAdviceSensitivity(const QString &name)
{
    ensureLoaded();
    // принимаем только известные пресеты; всё прочее → normal
    g_autoAdviceSensitivity =
        (name == QStringLiteral("quiet") || name == QStringLiteral("active"))
            ? name
            : QStringLiteral("normal");
    save();
}

const AutoAdviceTuning &autoAdviceTuning()
{
    ensureLoaded();
    // Пресет разворачивается в числа гейта. Поля в порядке структуры:
    // voiceMarginDb, vadHangoverMs, minUtteranceMs, cooldownMs, retriggerDeltaSec, maxDisputeMs,
    // requireAdjacency, suppressWhileMicHot, vadThreshold.
    static const AutoAdviceTuning kQuiet  { 9.0f, 1000, 1400, 10000, 1.8f, 12000, true,  true,  0.60f };
    static const AutoAdviceTuning kNormal { 6.0f,  700,  900,  7000, 1.2f,  9000, false, true,  0.50f };
    static const AutoAdviceTuning kActive { 4.0f,  450,  500,  4000, 0.8f,  6000, false, false, 0.45f };
    if (g_autoAdviceSensitivity == QStringLiteral("quiet"))  return kQuiet;
    if (g_autoAdviceSensitivity == QStringLiteral("active")) return kActive;
    return kNormal;
}

QString adviceModelTier()
{
    ensureLoaded();
    return g_adviceModelTier;
}

void setAdviceModelTier(const QString &t)
{
    ensureLoaded();
    g_adviceModelTier = (t == QStringLiteral("flash") || t == QStringLiteral("lite"))
                            ? t
                            : QStringLiteral("auto");
    save();
}

QString modelsPath()
{
    ensureLoaded();
    return g_modelsPath;
}

void setModelsPath(const QString &path)
{
    ensureLoaded();
    g_modelsPath = path.trimmed();
    save();
}

QString connMode()
{
    ensureLoaded();
    return g_connMode;
}

void setConnMode(const QString &mode)
{
    ensureLoaded();
    g_connMode = mode;
    save();
}

QString connLast()
{
    ensureLoaded();
    return g_connLast;
}

void setConnLast(const QString &v)
{
    ensureLoaded();
    g_connLast = v;
    save();
}

void save()
{
    ensureLoaded();

    QJsonObject o;
    o.insert("micDeviceId", g_audio.micDeviceId);
    o.insert("gameProcessExe", g_audio.gameProcessExe);
    o.insert("autoUpdate", g_autoUpdate);
    o.insert("overlayAutostart", g_overlayAutostart);
    o.insert("localDiarThreshold", g_localDiarThreshold);
    o.insert("theme", g_uiTheme);
    o.insert("debug", g_debugMode);
    o.insert("contextLimit", g_contextLimit);

    QJsonObject w;
    w.insert("opacity", g_window.opacity);
    w.insert("fontScalePct", g_window.fontScalePct);
    w.insert("hasGeometry", g_window.hasGeometry);
    w.insert("x", g_window.x);
    w.insert("y", g_window.y);
    w.insert("monitorIndex", g_window.monitorIndex);
    w.insert("captureInvisible", g_window.captureInvisible);
    w.insert("stealthMode", g_window.stealthMode);
    w.insert("hkToggle", g_window.hkToggle);
    w.insert("hkClickThrough", g_window.hkClickThrough);
    w.insert("hkAsk", g_window.hkAsk);
    w.insert("hkCollapse", g_window.hkCollapse);
    w.insert("hkPushToTalk", g_window.hkPushToTalk);
    w.insert("hkLocalRecord", g_window.hkLocalRecord);
    w.insert("hkLocalAnswer", g_window.hkLocalAnswer);
    w.insert("localWindowSec", g_window.localWindowSec);
    o.insert("window", w);

    QJsonObject s;
    s.insert("projectId", g_server.projectId);
    s.insert("serverId", g_server.serverId);
    s.insert("defaultMode", g_server.defaultMode);
    o.insert("server", s);

    QJsonObject ld;
    ld.insert("projectId", g_leadership.projectId);
    ld.insert("serverId", g_leadership.serverId);
    o.insert("leadership", ld);

    QJsonObject d;
    d.insert("mode", g_connMode);
    d.insert("last", g_connLast);
    o.insert("connection", d);

    QJsonObject aa;
    aa.insert("sensitivity", g_autoAdviceSensitivity);
    o.insert("autoAdvice", aa);
    o.insert("adviceModel", g_adviceModelTier);
    o.insert("modelsPath", g_modelsPath);

    QFile f(settingsFilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

} // namespace settings
