#include "LicenseClient.h"
#include "Config.h"
#include "Hwid.h"
#include "Hardening.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QMap>
#include <QDebug>

namespace {
// Алфавит и формат ключа — синхронны с сервером (services/license.py).
const QString kAlphabet = QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
const QString kPrefix   = QStringLiteral("RPOV");
constexpr int kBodyLen  = 20;   // 19 случайных + 1 check-char

// Вариант сборки → сервер гейтит активацию: "full" доступен только тестерам/админам,
// "qa" — обычным ключом (подписчики+), без тестер-гейта; "laws" — билд «поиск по законке»
// (универсальный ключ без HWID-привязки, тир laws = только /laws/*). Проверка laws ПЕРВОЙ:
// laws-билд определяет И RPOVERLAY_QA (наследует QA-вырезания).
#if defined(RPOVERLAY_LAWS)
const QString kVariant = QStringLiteral("laws");
#elif defined(RPOVERLAY_QA)
const QString kVariant = QStringLiteral("qa");
#else
const QString kVariant = QStringLiteral("full");
#endif

// Серверный код ошибки → человеческий текст для UI.
QString humanError(const QString &code)
{
    static const QMap<QString, QString> m = {
        {QStringLiteral("invalid_key_format"), QStringLiteral("Неверный формат ключа.")},
        {QStringLiteral("bad_signature"),      QStringLiteral("Ошибка подписи устройства. Перезапусти оверлей.")},
        {QStringLiteral("invalid_challenge"),  QStringLiteral("Срок запроса истёк — попробуй ещё раз.")},
        {QStringLiteral("pubkey_mismatch"),    QStringLiteral("Ключ привязан к другой установке. Сбрось привязку у админа.")},
        {QStringLiteral("invalid_key"),        QStringLiteral("Ключ недействителен. Возьми ключ в боте: /test → «🔑 Мой ключ».")},
        {QStringLiteral("key_revoked"),        QStringLiteral("Ключ отозван. Возьми новый: /test → «🔑 Мой ключ».")},
        {QStringLiteral("tier_lost"),          QStringLiteral("Доступ закрыт (нет подписки/тестера или аккаунт заблокирован).")},
        {QStringLiteral("hwid_banned"),        QStringLiteral("Это устройство заблокировано.")},
        {QStringLiteral("rebind_limit"),       QStringLiteral("Исчерпан лимит переноса ключа на новый ПК. Обратись к админу.")},
        {QStringLiteral("rebind_cooldown"),    QStringLiteral("Слишком частая смена ПК. Попробуй позже или обратись к админу.")},
        {QStringLiteral("hwid_mismatch"),      QStringLiteral("Ключ привязан к другому ПК. В боте: /test → «🔑 Мой ключ» → «🔄 Сменить ПК», затем активируй здесь.")},
        {QStringLiteral("not_activated"),      QStringLiteral("Это устройство не активировано. Введи ключ из бота (/test → «🔑 Мой ключ»).")},
        {QStringLiteral("tester_only"),        QStringLiteral("Полная версия — только для тестеров. Используй публичную Q&A-версию из бота.")},
    };
    return m.value(code, QStringLiteral("Не удалось активировать: %1").arg(code));
}
} // namespace

LicenseClient::LicenseClient(QObject *parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
{
}

bool LicenseClient::validateFormat(const QString &key)
{
    QString c;
    for (QChar ch : key.toUpper())
        if (ch.isLetterOrNumber())
            c += ch;
    if (!c.startsWith(kPrefix))
        return false;
    const QString body = c.mid(kPrefix.size());
    if (body.size() != kBodyLen)
        return false;
    for (QChar ch : body)
        if (!kAlphabet.contains(ch))
            return false;
    // weighted-checksum: idx = Σ (i+1)*alphabet.indexOf(payload[i]) mod 32
    const QString payload = body.left(kBodyLen - 1);
    long total = 0;
    for (int i = 0; i < payload.size(); ++i)
        total += static_cast<long>(i + 1) * kAlphabet.indexOf(payload.at(i));
    return body.at(kBodyLen - 1) == kAlphabet.at(static_cast<int>(total % 32));
}

QNetworkReply *LicenseClient::postJson(const QString &path, const QByteArray &body)
{
    QNetworkRequest req{QUrl(cfg().apiBaseUrl + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return nam_->post(req, body);
}

void LicenseClient::activate(const QString &key)
{
    if (!validateFormat(key)) {
        emit activateError(QStringLiteral("invalid_key_format"), humanError(QStringLiteral("invalid_key_format")));
        return;
    }
    const hwid::Fingerprint &fp = hwid::get();
    const QString hwidStr = fp.strictId;
    if (hwidStr.isEmpty() || fp.publicKey.isEmpty()) {
        emit activateError(QStringLiteral("hwid"), QStringLiteral("Не удалось определить железо этого ПК."));
        return;
    }
    QJsonObject body{{QStringLiteral("hwid"), hwidStr}};
    QNetworkReply *reply = postJson(QStringLiteral("/v1/license/challenge"),
                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, hwidStr]() {
        onChallengeFinished(reply, key, hwidStr);
    });
}

void LicenseClient::onChallengeFinished(QNetworkReply *reply, const QString &key, const QString &hwidStr)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        emit activateError(QStringLiteral("network"),
                           QStringLiteral("Нет связи с сервером: ") + reply->errorString());
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    const QString nonce = obj.value(QStringLiteral("nonce")).toString();
    if (nonce.isEmpty()) {
        emit activateError(QStringLiteral("invalid_challenge"), humanError(QStringLiteral("invalid_challenge")));
        return;
    }

    const hwid::Fingerprint &fp = hwid::get();
    const QByteArray sig = hwid::signChallenge(nonce.toUtf8());
    if (sig.isEmpty()) {
        emit activateError(QStringLiteral("bad_signature"), humanError(QStringLiteral("bad_signature")));
        return;
    }

    QJsonObject components;
    for (auto it = fp.components.constBegin(); it != fp.components.constEnd(); ++it)
        components.insert(it.key(), it.value());

    QString appVersion = QCoreApplication::applicationVersion();
    if (appVersion.isEmpty())
        appVersion = QStringLiteral("0.0.0");

    QJsonObject body{
        {QStringLiteral("key"), key},
        {QStringLiteral("hwid"), hwidStr},
        {QStringLiteral("pubkey"), QString::fromLatin1(fp.publicKey.toBase64())},
        {QStringLiteral("nonce"), nonce},
        {QStringLiteral("sig"), QString::fromLatin1(sig.toBase64())},
        {QStringLiteral("device_id"), deviceId()},
        {QStringLiteral("components"), components},
        {QStringLiteral("app_version"), appVersion},
        {QStringLiteral("os"), QStringLiteral("windows")},
        // Вариант сборки — сервер гейтит "full" только тестерам (tester_only при отказе).
        {QStringLiteral("variant"), kVariant},
        // Харденинг (Пиллар C): мгновенный анти-дебаг снимок (без сети) — сервер фиксирует
        // «ключ активировали под отладчиком». Сервер (license.activate) кладёт это в аудит-поле;
        // старый сервер просто игнорирует неизвестное поле (Pydantic) — обратная совместимость.
        {QStringLiteral("client_integrity"), Hardening::antiDebugSnapshot()},
    };
    QNetworkReply *r = postJson(QStringLiteral("/v1/license/activate"),
                                QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r]() { onActivateFinished(r); });
}

void LicenseClient::onActivateFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    const QByteArray payload = reply->readAll();
    const QJsonObject obj = QJsonDocument::fromJson(payload).object();

    if (reply->error() != QNetworkReply::NoError) {
        // Сервер заворачивает HTTPException в {"error":<код>,"code":"HTTP_4xx"} (api/main.py
        // http_exception_handler), а НЕ в {"detail":...}. Читаем "error", фолбэк на "detail".
        const QString code = obj.value(QStringLiteral("error"))
            .toString(obj.value(QStringLiteral("detail")).toString(QStringLiteral("network")));
        emit activateError(code, humanError(code));
        return;
    }
    const QString access = obj.value(QStringLiteral("access_token")).toString();
    if (access.isEmpty()) {
        emit activateError(QStringLiteral("empty"), QStringLiteral("Сервер вернул пустой токен."));
        return;
    }
    emit activated(access,
                   obj.value(QStringLiteral("refresh_token")).toString(),
                   obj.value(QStringLiteral("access_expires_in")).toInt(0),
                   obj.value(QStringLiteral("tier")).toString());
}

// ---- Перепроверка железа на каждом запуске (challenge → подпись → /verify) ----
void LicenseClient::verifyHardware()
{
    const hwid::Fingerprint &fp = hwid::get();
    const QString hwidStr = fp.strictId;
    if (hwidStr.isEmpty() || fp.publicKey.isEmpty()) {
        // Не смогли посчитать железо — НЕ фатально (нефатальный код), сессию не рвём.
        emit hardwareError(QStringLiteral("hwid"), QStringLiteral("Не удалось определить железо этого ПК."));
        return;
    }
    QJsonObject body{{QStringLiteral("hwid"), hwidStr}};
    QNetworkReply *reply = postJson(QStringLiteral("/v1/license/challenge"),
                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, hwidStr]() {
        onVerifyChallengeFinished(reply, hwidStr);
    });
}

void LicenseClient::onVerifyChallengeFinished(QNetworkReply *reply, const QString &hwidStr)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        // Нет связи — нефатально (код "network"): не разлогиниваем, повторим на следующем запуске.
        emit hardwareError(QStringLiteral("network"), reply->errorString());
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    const QString nonce = obj.value(QStringLiteral("nonce")).toString();
    if (nonce.isEmpty()) {
        emit hardwareError(QStringLiteral("invalid_challenge"), humanError(QStringLiteral("invalid_challenge")));
        return;
    }
    const hwid::Fingerprint &fp = hwid::get();
    const QByteArray sig = hwid::signChallenge(nonce.toUtf8());
    if (sig.isEmpty()) {
        emit hardwareError(QStringLiteral("bad_signature"), humanError(QStringLiteral("bad_signature")));
        return;
    }
    QJsonObject components;
    for (auto it = fp.components.constBegin(); it != fp.components.constEnd(); ++it)
        components.insert(it.key(), it.value());
    QString appVersion = QCoreApplication::applicationVersion();
    if (appVersion.isEmpty())
        appVersion = QStringLiteral("0.0.0");

    QJsonObject body{
        {QStringLiteral("hwid"), hwidStr},
        {QStringLiteral("pubkey"), QString::fromLatin1(fp.publicKey.toBase64())},
        {QStringLiteral("nonce"), nonce},
        {QStringLiteral("sig"), QString::fromLatin1(sig.toBase64())},
        {QStringLiteral("variant"), kVariant},   // тестер-гейт full-сборки на каждом запуске
        // Серверный НЕПРЕРЫВНЫЙ детект (Слой C): компоненты HWID + анти-дебаг снимок на КАЖДОМ запуске
        // → дрейф/spoofing/integrity видны постоянно, не только при активации. Сервер не блокирует verify
        // по ним (флаг+лог+градуированная реакция). Старый сервер игнорит неизвестные поля (совместимо).
        {QStringLiteral("components"), components},
        {QStringLiteral("client_integrity"), Hardening::antiDebugSnapshot()},
        {QStringLiteral("app_version"), appVersion},
        {QStringLiteral("os"), QStringLiteral("windows")},
    };
    QNetworkReply *r = postJson(QStringLiteral("/v1/license/verify"),
                                QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r]() { onVerifyFinished(r); });
}

void LicenseClient::onVerifyFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    if (reply->error() != QNetworkReply::NoError) {
        const QString code = obj.value(QStringLiteral("error"))
            .toString(obj.value(QStringLiteral("detail")).toString(QStringLiteral("network")));
        emit hardwareError(code, humanError(code));
        return;
    }
    emit hardwareOk(obj.value(QStringLiteral("tier")).toString());
    // Кил-свитч (Слой F): сервер мог приложить pending-команду для этого устройства. Передаём дальше —
    // KillSwitch сам верифицирует подпись токена и target_pubkey, прежде чем что-либо делать.
    const QJsonObject cmd = obj.value(QStringLiteral("command")).toObject();
    if (!cmd.isEmpty())
        emit commandReceived(cmd);
}
