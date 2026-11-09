#pragma once

#include <QObject>
#include <QJsonObject>
#include <QByteArray>
#include <QString>

class QNetworkAccessManager;

// Удалённый кил-свитч — клиентская часть (Слой F плана усиления защиты).
//
// Команда деактивации приходит в ответе /v1/license/verify (поле "command") при каждом запуске. Клиент
// САМ верифицирует подпись токена вшитым публичным ключом релиза (RPOVERLAY_UPDATE_PUBKEY) и проверяет,
// что target_pubkey == СВОЙ. Только при всех совпадениях: подписанный устройством ACK по обоим каналам →
// запуск рипера (копия RpUpdater.exe из %TEMP% в режиме --reap) → выход. Рипер сносит ТОЛЬКО зашитый
// allowlist своих папок.
//
// Инварианты безопасности: нет broadcast-формата (только target_pubkey), нет приватного ключа в клиенте
// (проверка асимметрична), одноразовый nonce + expiry (нет реплея/переноса), любая неудачная проверка →
// НИЧЕГО не делаем. Реальное лишение доступа даёт серверный лок (verify→hwid_banned); рип — чистка следов.
class KillSwitch : public QObject {
    Q_OBJECT
public:
    explicit KillSwitch(QObject *parent = nullptr);

    // Обработать поле "command" из ответа verify. {command_id, payload, sig}. Безопасно вызывать с пустым.
    void handle(const QJsonObject &command);

private:
    static bool ed25519VerifyUpdateKey(const QByteArray &msg, const QByteArray &sig);
    bool nonceUsed(const QString &nonce) const;
    void markNonceUsed(const QString &nonce);
    void sendAckThenQuit(const QString &commandId, const QString &ownPubB64, const QString &nonce);
    bool launchReaper();
    QString nonceStorePath() const;

    QNetworkAccessManager *nam_;
};
