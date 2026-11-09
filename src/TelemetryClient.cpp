#include "TelemetryClient.h"
#include "Config.h"
#include "Hwid.h"
#include "Hardening.h"
#include "Dpapi.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>

#include <memory>

namespace {
const QString  kPath          = QStringLiteral("/v1/license/telemetry");
const QByteArray kQueueEntropy = QByteArrayLiteral("RpOverlay.telemetry.v1");  // отделяет blob от tokens.dat
constexpr int  kMaxQueue      = 200;     // кап офлайн-очереди (защита от безграничного роста)

// Координатор «оба канала упали» для одного события: общий счётчик по двум ответам.
struct SendState {
    int        pending = 0;
    bool       anyOk   = false;
    QByteArray json;
};
} // namespace

TelemetryClient::TelemetryClient(QObject *parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
{
}

QByteArray TelemetryClient::buildEvent(const QString &kind, const QString &detail) const
{
    const hwid::Fingerprint &fp = hwid::get();
    QString appVersion = QCoreApplication::applicationVersion();
    if (appVersion.isEmpty())
        appVersion = QStringLiteral("0.0.0");

    QJsonObject ev{
        {QStringLiteral("event_id"),    QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("kind"),        kind},
        {QStringLiteral("detail"),      detail.left(1000)},
        {QStringLiteral("hwid"),        fp.strictId},
        {QStringLiteral("hwid_pubkey"), QString::fromLatin1(fp.publicKey.toBase64())},
        {QStringLiteral("integrity"),   Hardening::antiDebugSnapshot()},
        {QStringLiteral("app_version"), appVersion},
        {QStringLiteral("os"),          QStringLiteral("windows")},
    };
    return QJsonDocument(ev).toJson(QJsonDocument::Compact);
}

void TelemetryClient::report(const QString &kind, const QString &detail)
{
    sendBoth(buildEvent(kind, detail));
}

void TelemetryClient::sendBoth(const QByteArray &eventJson)
{
    // ОБА базовых URL явно: основной (всегда главный домен) + relay. Не зависит от активного канала.
    QStringList uniq;
    for (const QString &b : {mainApiBaseUrl(), cfg().relayApiBaseUrl})
        if (!b.isEmpty() && !uniq.contains(b))
            uniq << b;
    if (uniq.isEmpty()) {
        enqueue(eventJson);
        return;
    }

    auto state = std::make_shared<SendState>();
    state->pending = uniq.size();
    state->json    = eventJson;

    for (const QString &base : uniq) {
        QNetworkRequest req{QUrl(base + kPath)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(8000);
        QNetworkReply *reply = nam_->post(req, eventJson);
        connect(reply, &QNetworkReply::finished, this, [this, reply, state]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError)
                state->anyOk = true;
            if (--state->pending == 0 && !state->anyOk)
                enqueue(state->json);   // оба канала упали → в офлайн-очередь, дошлём на старте
        });
    }
}

QString TelemetryClient::queueFilePath() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/RpOverlay");
    return dir + QStringLiteral("/telemetry_queue.dat");
}

void TelemetryClient::enqueue(const QByteArray &eventJson)
{
    const QString path = queueFilePath();

    QJsonArray arr;
    QFile f(path);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        const QByteArray sealed = f.readAll();
        f.close();
        const QByteArray plain = dpapi::unprotect(sealed, kQueueEntropy);
        if (!plain.isEmpty())
            arr = QJsonDocument::fromJson(plain).array();
    }

    const QJsonObject obj = QJsonDocument::fromJson(eventJson).object();
    if (!obj.isEmpty())
        arr.append(obj);
    while (arr.size() > kMaxQueue)          // держим только последние kMaxQueue
        arr.removeFirst();

    const QByteArray plain  = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    const QByteArray sealed = dpapi::protect(plain, kQueueEntropy);
    if (sealed.isEmpty()) {                 // DPAPI не сработал — НЕ пишем plaintext (приватность)
        qWarning().noquote() << "[telemetry] DPAPI seal не удался — событие отброшено";
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(sealed);
        out.close();
    }
}

void TelemetryClient::flushQueued()
{
    const QString path = queueFilePath();
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return;
    const QByteArray sealed = f.readAll();
    f.close();
    const QByteArray plain = dpapi::unprotect(sealed, kQueueEntropy);
    QFile::remove(path);                    // чистим очередь; не дошедшие снова упадут в enqueue
    if (plain.isEmpty())
        return;

    const QJsonArray arr = QJsonDocument::fromJson(plain).array();
    for (const QJsonValue &v : arr) {
        if (v.isObject())
            sendBoth(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }
}
