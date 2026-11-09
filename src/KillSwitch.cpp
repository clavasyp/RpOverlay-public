#include "KillSwitch.h"
#include "Config.h"
#include "Hwid.h"
#include "update_pubkey.h"   // RPOVERLAY_UPDATE_PUBKEY[32]

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QDateTime>
#include <QTimer>
#include <QSet>
#include <QDebug>

#include <openssl/evp.h>

#include <memory>

namespace {
const QString kAckPath = QStringLiteral("/v1/license/command/ack");
}

KillSwitch::KillSwitch(QObject *parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
{
}

bool KillSwitch::ed25519VerifyUpdateKey(const QByteArray &msg, const QByteArray &sig)
{
    if (sig.size() != 64)
        return false;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, RPOVERLAY_UPDATE_PUBKEY, 32);
    if (!pkey)
        return false;
    bool ok = false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1)
        ok = EVP_DigestVerify(ctx,
                              reinterpret_cast<const unsigned char *>(sig.constData()), (size_t)sig.size(),
                              reinterpret_cast<const unsigned char *>(msg.constData()), (size_t)msg.size()) == 1;
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

QString KillSwitch::nonceStorePath() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/RpOverlay");
    return dir + QStringLiteral("/reaped_nonces.txt");
}

bool KillSwitch::nonceUsed(const QString &nonce) const
{
    QFile f(nonceStorePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QByteArray all = f.readAll();
    f.close();
    for (const QByteArray &line : all.split('\n'))
        if (QString::fromUtf8(line.trimmed()) == nonce)
            return true;
    return false;
}

void KillSwitch::markNonceUsed(const QString &nonce)
{
    const QString path = nonceStorePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(nonce.toUtf8());
        f.write("\n");
        f.close();
    }
}

void KillSwitch::handle(const QJsonObject &command)
{
    if (command.isEmpty())
        return;
    const QString payload = command.value(QStringLiteral("payload")).toString();
    const QString sigB64  = command.value(QStringLiteral("sig")).toString();
    QString commandId     = command.value(QStringLiteral("command_id")).toString();
    if (payload.isEmpty() || sigB64.isEmpty())
        return;

    // 1) Подпись токена ВШИТЫМ публичным ключом релиза над РОВНО payload-байтами (никакой реканонизации).
    if (!ed25519VerifyUpdateKey(payload.toUtf8(), QByteArray::fromBase64(sigB64.toUtf8())))
        return;                                   // плохая подпись → игнор (fail-safe)

    const QJsonObject tok = QJsonDocument::fromJson(payload.toUtf8()).object();
    if (tok.value(QStringLiteral("action")).toString() != QStringLiteral("reap"))
        return;

    // 2) Адресовано ИМЕННО нашей машине?
    const QString targetPub = tok.value(QStringLiteral("target_pubkey")).toString();
    const QString ownPub = QString::fromLatin1(hwid::get().publicKey.toBase64());
    if (targetPub.isEmpty() || ownPub.isEmpty() || targetPub != ownPub)
        return;                                   // не наш pubkey → игнор

    // 3) Не истёк и не повтор.
    const QString nonce = tok.value(QStringLiteral("nonce")).toString();
    if (nonce.isEmpty())
        return;
    const qint64 exp = tok.value(QStringLiteral("expires_at")).toString().toLongLong();
    if (exp > 0 && QDateTime::currentSecsSinceEpoch() > exp)
        return;                                   // истёк
    if (nonceUsed(nonce))
        return;                                   // реплей
    markNonceUsed(nonce);

    if (commandId.isEmpty())
        commandId = nonce;                        // сервер кладёт command_id = nonce

    qWarning().noquote() << "[killswitch] валидная reap-команда — запуск рипера + ACK";

    // 4) Запуск рипера (он дождётся нашего exit до 30с, затем снесёт allowlist своих папок).
    launchReaper();
    // 5) Подписанный устройством ACK по ОБОИМ каналам, ЗАТЕМ выход (когда ACK ушёл или по 8с-капу).
    sendAckThenQuit(commandId, ownPub, nonce);
}

void KillSwitch::sendAckThenQuit(const QString &commandId, const QString &ownPubB64, const QString &nonce)
{
    const QByteArray ackSig = hwid::signChallenge(nonce.toUtf8());  // подпись пер-машинным ключом над nonce
    QJsonObject body{
        {QStringLiteral("command_id"), commandId},
        {QStringLiteral("hwid_pubkey"), ownPubB64},
        {QStringLiteral("ack_sig"), QString::fromLatin1(ackSig.toBase64())},
    };
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QStringList uniq;
    for (const QString &b : {mainApiBaseUrl(), cfg().relayApiBaseUrl})
        if (!b.isEmpty() && !uniq.contains(b))
            uniq << b;

    auto quitOnce = std::make_shared<bool>(false);
    auto doQuit = [quitOnce]() {
        if (!*quitOnce) { *quitOnce = true; QCoreApplication::quit(); }
    };
    // Жёсткий потолок: выходим максимум через 8с, даже если ACK завис (рипер всё равно дождётся exit).
    QTimer::singleShot(8000, qApp, doQuit);

    if (uniq.isEmpty()) {
        QTimer::singleShot(300, qApp, doQuit);
        return;
    }
    auto remaining = std::make_shared<int>(uniq.size());
    for (const QString &base : uniq) {
        QNetworkRequest req{QUrl(base + kAckPath)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setTransferTimeout(8000);
        QNetworkReply *reply = nam_->post(req, payload);
        connect(reply, &QNetworkReply::finished, this, [reply, remaining, doQuit]() {
            reply->deleteLater();
            if (--(*remaining) == 0)
                QTimer::singleShot(200, qApp, doQuit);  // оба ACK ушли → выходим
        });
    }
}

bool KillSwitch::launchReaper()
{
    const QString installDir = QCoreApplication::applicationDirPath();
    const QString updater = installDir + QStringLiteral("/RpUpdater.exe");
    if (!QFile::exists(updater))
        return false;

    // Рипер запускаем из %TEMP% (копия), чтобы он мог снести installDir вместе с оригинальным RpUpdater.exe.
    const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString pid = QString::number(QCoreApplication::applicationPid());
    const QString reaperExe = tmp + QStringLiteral("/RpReaper_") + pid + QStringLiteral(".exe");
    QFile::remove(reaperExe);
    if (!QFile::copy(updater, reaperExe))
        return false;

    const QString reapTxt = tmp + QStringLiteral("/rpreap_") + pid + QStringLiteral(".txt");
    QFile f(reapTxt);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray b;
        b += "reap=RPOVERLAY_REAP_V1\n";
        b += "mainPid=" + QByteArray::number((qulonglong)QCoreApplication::applicationPid()) + "\n";
        b += "installDir=" + QDir::toNativeSeparators(installDir).toUtf8() + "\n";
        f.write(b);
        f.close();
    }
    return QProcess::startDetached(reaperExe, {QStringLiteral("--reap"), reapTxt});
}
