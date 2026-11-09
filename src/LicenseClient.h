#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>

class QNetworkAccessManager;
class QNetworkReply;

// LicenseClient — активация оверлея по лицензионному КЛЮЧУ с привязкой к железу (HWID).
//
// Поток (заменяет device-code):
//   activate(key)
//     ├─ HWID = hwid::get().strictId
//     ├─ POST /v1/license/challenge {hwid} → nonce
//     ├─ sig = hwid::signChallenge(nonce)  (ed25519 пер-машинным ключом)
//     └─ POST /v1/license/activate {key, hwid, pubkey, nonce, sig, device_id, components}
//          → 200 → emit activated(access, refresh, ttl, tier)  (далее AuthClient::acceptTokens)
//          → 4xx → emit activateError(code, human)
class LicenseClient : public QObject {
    Q_OBJECT
public:
    explicit LicenseClient(QObject *parent = nullptr);

    void activate(const QString &key);
    // Перепроверка ВСЕХ условий активации для ТЕКУЩЕЙ машины — зовётся на КАЖДОМ запуске (даже после
    // refresh-входа). challenge→подпись пер-машинным ключом→/v1/license/verify. Сервер сверяет подпись
    // с pubkey, привязанным к активному ключу → ловит копирование токенов на другой ПК / смену железа.
    void verifyHardware();
    // Локальная пре-валидация формата (префикс RPOV + длина + алфавит + контрольный символ).
    // Формула check-char ДОЛЖНА совпадать с сервером (services/license.py::_checksum_char).
    static bool validateFormat(const QString &key);

signals:
    void activated(QString access, QString refresh, int accessExpiresIn, QString tier);
    void activateError(QString code, QString human);
    void hardwareOk(QString tier);                    // verify прошёл: эта машина = привязанная
    void hardwareError(QString code, QString human);  // verify не прошёл (код решает, фатально ли)
    void commandReceived(const QJsonObject &command); // кил-свитч (Слой F): pending-команда из ответа verify

private:
    void onChallengeFinished(QNetworkReply *reply, const QString &key, const QString &hwid);
    void onActivateFinished(QNetworkReply *reply);
    void onVerifyChallengeFinished(QNetworkReply *reply, const QString &hwid);
    void onVerifyFinished(QNetworkReply *reply);
    QNetworkReply *postJson(const QString &path, const QByteArray &body);

    QNetworkAccessManager *nam_ = nullptr;
};
