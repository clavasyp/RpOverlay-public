#include "Config.h"
#include "Obfusc.h"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

static AppConfig g_cfg;
static bool g_loaded = false;
static QString g_mainApi, g_mainWs;                       // основной канал (для возврата из relay)
static QString g_activeChannel = QStringLiteral("main");  // текущий активный канал

static QString findConfigFile()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        exeDir + "/config.json",
        exeDir + "/../config.json",        // запуск из build/
        QDir::currentPath() + "/config.json",
    };
    for (const QString &p : candidates)
        if (QFile::exists(p))
            return p;
    return QString();
}

const AppConfig &cfg()
{
    if (g_loaded)
        return g_cfg;
    g_loaded = true;

    // ПРОДАКШН-значения зашиты прямо здесь — это деплой-конфигурация (домен/сервер/бот),
    // а НЕ пользовательские настройки (мик/окно/хоткеи живут в settings.json). Поэтому
    // установщик НЕ кладёт никакой config.json рядом с exe — юзеру нечего там трогать.
    // Всё идёт через домен за Cloudflare → реальный IP сервера не утекает, трафик по TLS.
    // config.json рядом с exe — лишь НЕОБЯЗАТЕЛЬНЫЙ override (удобно для дева/смены домена
    // без пересборки): если файл есть, перечисленные ниже поля берутся из него.
    g_cfg.apiBaseUrl  = QStringLiteral("https://overlay.turgovbot.ru");
    g_cfg.wsBaseUrl   = QStringLiteral("wss://overlay.turgovbot.ru");
    g_cfg.serverId    = QStringLiteral("orlando");
    g_cfg.botUsername = QStringLiteral("turgov_bot");
    // Резервный канал через RU-сервер-прокладку (см. src/Connectivity.*, src/relay_cert.h).
    // IP обфусцирован (B1), чтобы не лежал открытым ASCII при дампе строк. ⚠️ relay_cert.h всё равно
    // содержит этот IP в SAN сертификата (Qt должен его распарсить) — полностью скрыть нельзя; это
    // лишь дополнительный слой к Themida STR_ENCRYPT.
    g_cfg.relayApiBaseUrl = OBF("https://relay.invalid:8443");
    g_cfg.relayWsBaseUrl  = OBF("wss://relay.invalid:8443");

    const QString path = findConfigFile();
    if (!path.isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            if (o.contains("apiBaseUrl"))       g_cfg.apiBaseUrl       = o.value("apiBaseUrl").toString(g_cfg.apiBaseUrl);
            if (o.contains("wsBaseUrl"))        g_cfg.wsBaseUrl        = o.value("wsBaseUrl").toString(g_cfg.wsBaseUrl);
            if (o.contains("serverId"))         g_cfg.serverId         = o.value("serverId").toString(g_cfg.serverId);
            if (o.contains("botUsername"))      g_cfg.botUsername      = o.value("botUsername").toString(g_cfg.botUsername);
            if (o.contains("relayApiBaseUrl"))  g_cfg.relayApiBaseUrl  = o.value("relayApiBaseUrl").toString(g_cfg.relayApiBaseUrl);
            if (o.contains("relayWsBaseUrl"))   g_cfg.relayWsBaseUrl   = o.value("relayWsBaseUrl").toString(g_cfg.relayWsBaseUrl);
        }
    }

    // Запоминаем основной канал (apiBaseUrl/wsBaseUrl сейчас = основной) для возврата из relay.
    g_mainApi = g_cfg.apiBaseUrl;
    g_mainWs  = g_cfg.wsBaseUrl;
    return g_cfg;
}

void setActiveChannel(const QString &channel)
{
    (void)cfg();  // гарантируем загрузку
    if (channel == QLatin1String("relay")) {
        g_cfg.apiBaseUrl = g_cfg.relayApiBaseUrl;
        g_cfg.wsBaseUrl  = g_cfg.relayWsBaseUrl;
        g_activeChannel  = QStringLiteral("relay");
    } else {
        g_cfg.apiBaseUrl = g_mainApi;
        g_cfg.wsBaseUrl  = g_mainWs;
        g_activeChannel  = QStringLiteral("main");
    }
}

QString activeChannel()
{
    (void)cfg();
    return g_activeChannel;
}

const QString &mainApiBaseUrl()
{
    (void)cfg();        // гарантируем загрузку → g_mainApi заполнен главным доменом
    return g_mainApi;
}

QString deviceId()
{
    static QString id;
    if (!id.isEmpty())
        return id;

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString path = dir + "/device_id.txt";

    QFile f(path);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        id = QString::fromUtf8(f.readAll()).trimmed();
    }
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(id.toUtf8());
    }
    return id;
}
