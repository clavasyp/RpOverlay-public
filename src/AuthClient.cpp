#include "AuthClient.h"
#include "Config.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QCoreApplication>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace {
// За сколько секунд до истечения access обновляться заранее.
constexpr int kRefreshLeadSec = 60;
// Нижняя/верхняя границы интервала поллинга (защита от мусорных значений сервера).
constexpr int kMinPollSec = 2;
constexpr int kMaxPollSec = 60;
// Дополнительная "соль" к DPAPI как entropy — привязка контейнера к приложению.
const char *kDpapiEntropy = "RpOverlay.tokens.v1";
} // namespace

AuthClient::AuthClient(QObject *parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
    , pollTimer_(new QTimer(this))
    , refreshTimer_(new QTimer(this))
{
    pollTimer_->setSingleShot(false);
    connect(pollTimer_, &QTimer::timeout, this, &AuthClient::pollOnce);

    refreshTimer_->setSingleShot(true);
    connect(refreshTimer_, &QTimer::timeout, this, &AuthClient::refresh);
}

AuthClient::~AuthClient() = default;

// ============================ public API ============================

bool AuthClient::isAuthorized() const { return authorized_ && !accessToken_.isEmpty(); }
QString AuthClient::accessToken() const { return accessToken_; }
QString AuthClient::tier() const { return tier_; }

void AuthClient::begin()
{
    // Если в хранилище есть refresh — пробуем тихий вход без участия юзера.
    if (loadTokens() && !refreshToken_.isEmpty()) {
        qInfo() << "[auth] found stored refresh token, attempting silent refresh";
        refresh();
        return;
    }
    emit needKey();
}

void AuthClient::logout()
{
    const QString rt = refreshToken_;
    if (!rt.isEmpty()) {
        QJsonObject body{{"refresh_token", rt}};
        QNetworkReply *reply = postJson(QStringLiteral("/v1/auth/logout"),
                                        QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            onLogoutFinished(reply);
        });
    }
    // Локально гасим состояние немедленно, не дожидаясь сети.
    pollTimer_->stop();
    refreshTimer_->stop();
    deviceCode_.clear();
    clearTokens();
}

void AuthClient::requestWsTicket(QString forSource)
{
    if (!isAuthorized()) {
        emit authError(QStringLiteral("ws-ticket: нет авторизации"));
        return;
    }
    QNetworkRequest req{QUrl(cfg().apiBaseUrl + QStringLiteral("/v1/auth/ws-ticket"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
    // ws-ticket — POST без тела; шлём пустой JSON-объект для совместимости.
    QNetworkReply *reply = nam_->post(req, QByteArray("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, forSource]() {
        onWsTicketFinished(reply, forSource);
    });
}

// ============================ device flow ============================

void AuthClient::startDeviceFlow()
{
    QString appVersion = QCoreApplication::applicationVersion();
    if (appVersion.isEmpty())
        appVersion = QStringLiteral("0.1.0");
    QJsonObject body{
        {"device_id", deviceId()},
        {"app_version", appVersion},
        {"os", QStringLiteral("windows")},
    };
    QNetworkReply *reply = postJson(QStringLiteral("/v1/auth/device/start"),
                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onDeviceStartFinished(reply);
    });
}

void AuthClient::onDeviceStartFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        failAuth(QStringLiteral("device/start: ") + reply->errorString());
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    deviceCode_ = obj.value(QStringLiteral("device_code")).toString();
    if (deviceCode_.isEmpty()) {
        failAuth(QStringLiteral("device/start: пустой device_code"));
        return;
    }
    const QString userCode = obj.value(QStringLiteral("user_code")).toString();
    const int interval = obj.value(QStringLiteral("interval")).toInt(5);
    pollIntervalSec_ = qBound(kMinPollSec, interval, kMaxPollSec);

    const QString deepLink = QStringLiteral("https://t.me/%1?start=link_%2")
                                 .arg(cfg().botUsername, userCode);
    emit needLink(userCode, deepLink);

    // Запускаем поллинг подтверждения.
    pollTimer_->start(pollIntervalSec_ * 1000);
}

void AuthClient::pollOnce()
{
    if (deviceCode_.isEmpty()) {
        pollTimer_->stop();
        return;
    }
    QJsonObject body{{"device_code", deviceCode_}};
    QNetworkReply *reply = postJson(QStringLiteral("/v1/auth/device/poll"),
                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onPollFinished(reply);
    });
}

void AuthClient::onPollFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();

    // HTTP 400 при not_found/expired/denied — терминальная ошибка device-flow.
    if (http == 400) {
        const QString detail = obj.value(QStringLiteral("detail")).toString(QStringLiteral("expired"));
        pollTimer_->stop();
        deviceCode_.clear();
        failAuth(QStringLiteral("device/poll: ") + detail);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        // Транзиентная сетевая ошибка — не валим вход, ждём следующий тик.
        qWarning() << "[auth] poll transient error:" << reply->errorString();
        return;
    }

    const QString status = obj.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("approved")) {
        pollTimer_->stop();
        deviceCode_.clear();
        applyTokens(obj.value(QStringLiteral("access_token")).toString(),
                    obj.value(QStringLiteral("refresh_token")).toString(),
                    obj.value(QStringLiteral("access_expires_in")).toInt(0),
                    obj.value(QStringLiteral("tier")).toString());
        if (isAuthorized())
            emit authorized(tier_);
        return;
    }
    if (status == QLatin1String("consumed")) {
        // Заявку уже использовало другое устройство/окно — повторно вход не выдадут.
        pollTimer_->stop();
        deviceCode_.clear();
        failAuth(QStringLiteral("device/poll: заявка уже использована"));
        return;
    }
    // 'pending' | 'authorization_pending' — продолжаем ждать (ничего не делаем).
}

// ============================ refresh ============================

void AuthClient::requestRefresh()
{
    refresh();
}

void AuthClient::refresh()
{
    if (refreshToken_.isEmpty()) {
        // Нечем обновляться — заново через device-flow.
        emit needKey();
        return;
    }
    QJsonObject body{{"refresh_token", refreshToken_}};
    QNetworkReply *reply = postJson(QStringLiteral("/v1/auth/refresh"),
                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onRefreshFinished(reply);
    });
}

void AuthClient::onRefreshFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        // 401/403 — refresh недействителен/доступ потерян → чистим и идём в device-flow.
        if (http == 401 || http == 403) {
            qWarning() << "[auth] refresh rejected, http=" << http << "→ device flow";
            const bool wasAuthorized = authorized_;
            authorized_ = false;
            accessToken_.clear();
            refreshToken_.clear();
            tier_.clear();
            refreshTimer_->stop();
            clearTokens();
            if (wasAuthorized)
                emit authError(QStringLiteral("сессия истекла, требуется повторный вход"));
            emit needKey();
            return;
        }
        // Прочие (сетевые) ошибки — повторим попытку через короткий бэкофф.
        qWarning() << "[auth] refresh transient error:" << reply->errorString();
        refreshTimer_->start(15 * 1000);
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    applyTokens(obj.value(QStringLiteral("access_token")).toString(),
                obj.value(QStringLiteral("refresh_token")).toString(),
                obj.value(QStringLiteral("access_expires_in")).toInt(0),
                obj.value(QStringLiteral("tier")).toString());
    if (isAuthorized())
        emit authorized(tier_);
    else
        emit needKey();
}

// ============================ ws-ticket / logout ============================

void AuthClient::onWsTicketFinished(QNetworkReply *reply, const QString &forSource)
{
    reply->deleteLater();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        // Access мог протухнуть — попробуем обновить и подскажем повторить запрос.
        if (http == 401 || http == 403)
            refresh();
        emit authError(QStringLiteral("ws-ticket: ") + reply->errorString());
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    const QString ticket = obj.value(QStringLiteral("ticket")).toString();
    if (ticket.isEmpty()) {
        emit authError(QStringLiteral("ws-ticket: пустой ticket"));
        return;
    }
    emit wsTicketReady(ticket, forSource);
}

void AuthClient::onLogoutFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError)
        qWarning() << "[auth] logout error (игнорируем):" << reply->errorString();
}

// ============================ tokens / storage ============================

void AuthClient::applyTokens(const QString &access, const QString &refresh,
                             int accessExpiresIn, const QString &tier)
{
    if (access.isEmpty()) {
        failAuth(QStringLiteral("получен пустой access_token"));
        return;
    }
    accessToken_ = access;
    if (!refresh.isEmpty())
        refreshToken_ = refresh; // refresh ротируется — обновляем, если пришёл новый
    if (!tier.isEmpty())
        tier_ = tier;
    authorized_ = true;

    saveTokens();
    scheduleRefresh(accessExpiresIn);
}

void AuthClient::acceptTokens(const QString &access, const QString &refresh,
                              int accessExpiresIn, const QString &tier)
{
    // Активация по ключу прошла на сервере (LicenseClient) — кладём пару токенов в общий
    // механизм хранения/refresh и сигналим авторизацию (как ветка 'approved' device-flow).
    pollTimer_->stop();
    deviceCode_.clear();
    applyTokens(access, refresh, accessExpiresIn, tier);
    if (isAuthorized())
        emit authorized(tier_);
}

void AuthClient::scheduleRefresh(int accessExpiresIn)
{
    if (accessExpiresIn <= 0) {
        refreshTimer_->stop();
        return;
    }
    // Обновляемся за kRefreshLeadSec до истечения, но не реже, чем через 5 секунд.
    int delaySec = accessExpiresIn - kRefreshLeadSec;
    if (delaySec < 5)
        delaySec = qMax(5, accessExpiresIn / 2);
    refreshTimer_->start(delaySec * 1000);
}

void AuthClient::clearTokens()
{
    authorized_ = false;
    accessToken_.clear();
    refreshToken_.clear();
    tier_.clear();
    QFile::remove(tokensFilePath());
}

QString AuthClient::tokensFilePath()
{
    // %APPDATA%/RpOverlay/tokens.dat
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString dir = base;
    // AppDataLocation уже включает orgName/appName; если пусто — fallback на RpOverlay.
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/RpOverlay");
    return dir + QStringLiteral("/tokens.dat");
}

bool AuthClient::loadTokens()
{
    QFile f(tokensFilePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray enc = f.readAll();
    f.close();
    if (enc.isEmpty())
        return false;

    const QByteArray plain = dpapiUnprotect(enc);
    if (plain.isEmpty()) {
        qWarning() << "[auth] не удалось расшифровать tokens.dat (DPAPI), сбрасываю";
        QFile::remove(tokensFilePath());
        return false;
    }
    const QJsonObject obj = QJsonDocument::fromJson(plain).object();
    refreshToken_ = obj.value(QStringLiteral("refresh_token")).toString();
    tier_ = obj.value(QStringLiteral("tier")).toString();
    // access не сохраняем как долгоживущий — он короткоживущий и будет получен через refresh.
    return !refreshToken_.isEmpty();
}

void AuthClient::saveTokens() const
{
    QJsonObject obj{
        {"refresh_token", refreshToken_},
        {"tier", tier_},
    };
    const QByteArray plain = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    const QByteArray enc = dpapiProtect(plain);
    if (enc.isEmpty()) {
        qWarning() << "[auth] DPAPI protect failed, токены не сохранены";
        return;
    }
    const QString path = tokensFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[auth] не удалось открыть для записи:" << path;
        return;
    }
    f.write(enc);
    f.close();
}

// ============================ DPAPI (Windows) ============================

#ifdef Q_OS_WIN
QByteArray AuthClient::dpapiProtect(const QByteArray &plain)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
    in.cbData = static_cast<DWORD>(plain.size());

    QByteArray entropyBuf(kDpapiEntropy);
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE *>(entropyBuf.data());
    entropy.cbData = static_cast<DWORD>(entropyBuf.size());

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"RpOverlay tokens", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    QByteArray result(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    if (out.pbData)
        LocalFree(out.pbData);
    return result;
}

QByteArray AuthClient::dpapiUnprotect(const QByteArray &enc)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(enc.constData()));
    in.cbData = static_cast<DWORD>(enc.size());

    QByteArray entropyBuf(kDpapiEntropy);
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE *>(entropyBuf.data());
    entropy.cbData = static_cast<DWORD>(entropyBuf.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    QByteArray result(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    if (out.pbData)
        LocalFree(out.pbData);
    return result;
}
#else
// Не-Windows: DPAPI недоступен. Возвращаем как есть (plain) — на целевой платформе (Win)
// этот код не используется. Сделано, чтобы модуль компилировался и под другими ОС.
QByteArray AuthClient::dpapiProtect(const QByteArray &plain) { return plain; }
QByteArray AuthClient::dpapiUnprotect(const QByteArray &enc) { return enc; }
#endif

// ============================ helpers ============================

QNetworkReply *AuthClient::postJson(const QString &path, const QByteArray &body)
{
    QNetworkRequest req{QUrl(cfg().apiBaseUrl + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return nam_->post(req, body);
}

void AuthClient::failAuth(const QString &msg)
{
    qWarning() << "[auth]" << msg;
    emit authError(msg);
}
