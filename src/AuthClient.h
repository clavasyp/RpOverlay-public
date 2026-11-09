#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// AuthClient — device-code авторизация оверлея (OAuth 2.0 Device Authorization Grant, упрощённый).
//
// Поток:
//   begin()
//     ├─ если в DPAPI-хранилище есть refresh_token → сразу refresh() (без участия юзера);
//     └─ иначе POST /v1/auth/device/start → emit needLink(user_code, deepLink)
//          → поллинг POST /v1/auth/device/poll каждые `interval` сек
//          → status='approved' → сохранить токены → emit authorized(tier).
//   Авто-refresh по таймеру (за ~60с до истечения access_expires_in).
//   requestWsTicket(source): POST /v1/auth/ws-ticket (Bearer) → emit wsTicketReady(ticket, source).
//   logout(): POST /v1/auth/logout + очистка локального хранилища.
//
// Хранение: Windows DPAPI (CryptProtectData/CryptUnprotectData) в %APPDATA%/RpOverlay/tokens.dat.
class AuthClient : public QObject {
    Q_OBJECT
public:
    explicit AuthClient(QObject *parent = nullptr);
    ~AuthClient() override;

    void begin();
    bool isAuthorized() const;
    QString accessToken() const;
    QString tier() const;
    void requestWsTicket(QString forSource);
    void logout();
    // Принудительно обновить access по refresh-токену (зовут клиенты при 401 на своих эндпоинтах,
    // напр. AdviceClient на /v1/overlay/servers). Без refresh-токена → emit needKey, как refresh().
    void requestRefresh();
    // Принять токены от LicenseClient (активация по ключу) — кладёт в хранилище + emit authorized.
    void acceptTokens(const QString &access, const QString &refresh, int accessExpiresIn, const QString &tier);

signals:
    void needLink(QString userCode, QString deepLink);  // legacy device-flow (не используется при ключ-флоу)
    void needKey();                                      // нет валидного токена → требуется ввод ключа
    void authorized(QString tier);
    void authError(QString msg);
    void wsTicketReady(QString ticket, QString forSource);

private:
    // --- сетевые шаги ---
    void startDeviceFlow();
    void pollOnce();
    void refresh();

    // --- разбор ответов ---
    void onDeviceStartFinished(QNetworkReply *reply);
    void onPollFinished(QNetworkReply *reply);
    void onRefreshFinished(QNetworkReply *reply);
    void onWsTicketFinished(QNetworkReply *reply, const QString &forSource);
    void onLogoutFinished(QNetworkReply *reply);

    // --- токены/хранилище ---
    void applyTokens(const QString &access, const QString &refresh, int accessExpiresIn, const QString &tier);
    void scheduleRefresh(int accessExpiresIn);
    void clearTokens();
    bool loadTokens();
    void saveTokens() const;

    // --- helpers ---
    QNetworkReply *postJson(const QString &path, const QByteArray &body);
    static QByteArray dpapiProtect(const QByteArray &plain);
    static QByteArray dpapiUnprotect(const QByteArray &enc);
    static QString tokensFilePath();
    void failAuth(const QString &msg);

private:
    QNetworkAccessManager *nam_ = nullptr;
    QTimer *pollTimer_ = nullptr;     // device/poll каждые interval сек
    QTimer *refreshTimer_ = nullptr;  // авто-refresh до истечения access

    QString deviceCode_;   // секрет текущей device-flow заявки
    int pollIntervalSec_ = 5;

    QString accessToken_;
    QString refreshToken_;
    QString tier_;
    bool authorized_ = false;
};
