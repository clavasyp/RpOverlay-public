#include "LawsClient.h"

#include "AuthClient.h"
#include "Config.h"
#include "Connectivity.h"  // conn::reportNetworkResult/isTransport — авто-резерв (B3)

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QTimer>
#include <QDebug>

#include <memory>

// Поиск по каталогу — мгновенный (без LLM-генерации): держим КОРОТКИЙ общий transferTimeout,
// а не 105с Q&A. 12с: с запасом покрывает медленный канал через relay, но не морозит UI.
namespace {
constexpr int kLawsTimeoutMs = 12000;

// Тихо снять reply (по образцу AdviceClient::dropReply): отцепить сигналы, абортнуть, удалить.
void dropReply(QPointer<QNetworkReply> &reply, QObject *receiver)
{
    if (!reply)
        return;
    QObject::disconnect(reply, nullptr, receiver, nullptr);
    reply->abort();
    reply->deleteLater();
    reply = nullptr;
}

// Единый разбор ошибки laws-эндпоинтов → (code, человекочитаемый текст).
// 403/429 — спец-случаи беты; прочее — {"error","code"} из тела либо сеть/HTTP.
void mapLawsError(int httpStatus, const QByteArray &body, const QString &errorString,
                  QString &codeOut, QString &msgOut)
{
    if (httpStatus == 403) {
        codeOut = QStringLiteral("beta_closed");
        msgOut  = QStringLiteral("Поиск по законке в бете — на вашем тарифе или сервере пока недоступен.");
        return;
    }
    if (httpStatus == 429) {
        codeOut = QStringLiteral("rate_limited");
        msgOut  = QStringLiteral("Слишком много запросов — подождите минуту.");
        return;
    }
    const QJsonDocument edoc = QJsonDocument::fromJson(body);
    if (edoc.isObject()) {
        const QJsonObject eo = edoc.object();
        const QString err  = eo.value(QStringLiteral("error")).toString();
        const QString code = eo.value(QStringLiteral("code")).toString();
        if (!err.isEmpty()) {
            codeOut = code.isEmpty() ? QStringLiteral("server_error") : code;
            msgOut  = err;
            return;
        }
    }
    if (httpStatus > 0) {
        codeOut = QStringLiteral("http_%1").arg(httpStatus);
        msgOut  = QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(errorString);
    } else {
        codeOut = QStringLiteral("network");
        msgOut  = QStringLiteral("Сеть: %1").arg(errorString);
    }
}
} // namespace

QString LawsClient::cardCaption(const QVariantMap &card)
{
    const QString code  = card.value(QStringLiteral("code")).toString();
    const QString title = card.value(QStringLiteral("title")).toString();
    if (!title.isEmpty())
        return code.isEmpty() ? title : QStringLiteral("[%1] %2").arg(code, title);
    const QString ref = card.value(QStringLiteral("article_ref")).toString();
    if (!ref.isEmpty())
        return ref;
    if (!code.isEmpty())
        return QStringLiteral("[%1]").arg(code);
    return QStringLiteral("Статья №%1").arg(card.value(QStringLiteral("id")).toInt());
}

LawsClient::LawsClient(AuthClient *auth, QObject *parent)
    : QObject(parent), auth_(auth)
{
    nam_ = new QNetworkAccessManager(this);
}

LawsClient::~LawsClient()
{
    cancelAll();
}

QString LawsClient::apiUrl(const QString &path) const
{
    // На каждый запрос заново: активный канал (main/relay) мутируется на лету.
    QString base = cfg().apiBaseUrl;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base + path;
}

void LawsClient::cancelAll()
{
    dropReply(searchReply_, this);
    dropReply(articleReply_, this);
    dropReply(favReply_, this);
}

void LawsClient::cancelArticle()
{
    dropReply(articleReply_, this);
}

// ---------------------------------------------------------------------------
//  POST /v1/overlay/laws/search — гибридный поиск по каталогу законки.
// ---------------------------------------------------------------------------
void LawsClient::requestSearch(const QString &query, const QString &serverId, int limit)
{
    // Новый поиск отменяет предыдущий незавершённый: юзер перепечатал запрос — старая
    // выдача уже не нужна и не должна перетереть новую (или погасить чужой failsafe бара).
    dropReply(searchReply_, this);
    pendingQuery_    = query;
    pendingServerId_ = serverId;
    pendingLimit_    = limit;

    QJsonObject body;
    body.insert(QStringLiteral("server_id"), serverId.isEmpty() ? cfg().serverId : serverId);
    body.insert(QStringLiteral("query"), query);
    body.insert(QStringLiteral("limit"), limit);

    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/laws/search")))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kLawsTimeoutMs);
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());

    searchReply_ = nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(searchReply_, &QNetworkReply::finished, this, &LawsClient::onSearchFinished);
}

void LawsClient::onSearchFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    if (reply == searchReply_)
        searchReply_ = nullptr;

    const QByteArray data = reply->readAll();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netErr = reply->error();

    // Авто-резерв (B3): транспортный провал на main двигает счётчик; HTTP-ответ/успех — сбрасывает.
    // Если авто-резерв ПЕРЕКЛЮЧИЛ канал на relay — один раз переотправляем запрос по новому каналу,
    // чтобы юзер не увидел ошибку (свитч происходит на 2-м провале → без ретрая была бы 2-я ошибка).
    const bool transport = (netErr != QNetworkReply::NoError && httpStatus == 0
                            && conn::isTransport(static_cast<int>(netErr)));
    if (conn::reportNetworkResult(transport) && !searchRetried_) {
        searchRetried_ = true;
        requestSearch(pendingQuery_, pendingServerId_, pendingLimit_);   // повтор по новому (relay) каналу
        return;
    }

    if (netErr != QNetworkReply::NoError) {
        searchRetried_ = false;
        QString code, msg;
        mapLawsError(httpStatus, data, reply->errorString(), code, msg);
        qWarning().noquote() << "[laws] search error:" << code << msg;
        emit searchError(code, msg);
        return;
    }
    searchRetried_ = false;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit searchError(QStringLiteral("bad_response"),
                         QStringLiteral("Некорректный ответ поиска."));
        return;
    }
    const QJsonObject o = doc.object();
    emit searchReady(pendingQuery_,
                     o.value(QStringLiteral("results")).toArray().toVariantList(),
                     o.value(QStringLiteral("related")).toArray().toVariantList(),
                     o.value(QStringLiteral("degraded")).toBool());
}

// ---------------------------------------------------------------------------
//  GET /v1/overlay/laws/article/{id} — статья целиком (+related).
// ---------------------------------------------------------------------------
void LawsClient::requestArticle(int id)
{
    dropReply(articleReply_, this);   // открыли другую статью — прошлую не ждём
    pendingArticleId_ = id;           // для ретрая по новому каналу после авто-резерва

    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/laws/article/%1").arg(id)))};
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kLawsTimeoutMs);
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());

    articleReply_ = nam_->get(req);
    connect(articleReply_, &QNetworkReply::finished, this, &LawsClient::onArticleFinished);
}

void LawsClient::onArticleFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    if (reply == articleReply_)
        articleReply_ = nullptr;

    const QByteArray data = reply->readAll();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netErr = reply->error();

    const bool transport = (netErr != QNetworkReply::NoError && httpStatus == 0
                            && conn::isTransport(static_cast<int>(netErr)));
    if (conn::reportNetworkResult(transport) && !articleRetried_) {
        articleRetried_ = true;
        requestArticle(pendingArticleId_);   // повтор по новому (relay) каналу
        return;
    }

    if (netErr != QNetworkReply::NoError) {
        articleRetried_ = false;
        QString code, msg;
        mapLawsError(httpStatus, data, reply->errorString(), code, msg);
        qWarning().noquote() << "[laws] article error:" << code << msg;
        emit articleError(code, msg);
        return;
    }
    articleRetried_ = false;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit articleError(QStringLiteral("bad_response"),
                          QStringLiteral("Некорректный ответ статьи."));
        return;
    }
    emit articleReady(doc.object().toVariantMap());
}

// ---------------------------------------------------------------------------
//  GET /v1/overlay/laws/article/{id} для АВТООБНОВЛЕНИЯ избранного. Отдельный reply/сигналы —
//  фоновая докачка закреплённых статей НЕ должна конфликтовать с интерактивным requestArticle
//  (тот одноразовый и рендерит вид статьи) и не гоняет авто-резерв. HubWindow вызывает по очереди.
// ---------------------------------------------------------------------------
void LawsClient::refreshFavorite(int id)
{
    dropReply(favReply_, this);
    favId_ = id;

    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/laws/article/%1").arg(id)))};
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kLawsTimeoutMs);
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());

    favReply_ = nam_->get(req);
    connect(favReply_, &QNetworkReply::finished, this, &LawsClient::onFavFinished);
}

void LawsClient::onFavFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    if (reply == favReply_)
        favReply_ = nullptr;
    const int id = favId_;

    const QByteArray data = reply->readAll();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netErr = reply->error();

    if (netErr != QNetworkReply::NoError) {
        // ТОЛЬКО 404 = статья реально удалена → убрать из избранного. Прочее (сеть/5xx/403/429) —
        // транзиентно/гейт: избранное НЕ трогаем, лишь двигаем очередь.
        if (httpStatus == 404) emit favoriteMissing(id);
        else                   emit favoriteRefreshFailed(id);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit favoriteRefreshFailed(id);
        return;
    }
    emit favoriteRefreshed(id, doc.object().toVariantMap());
}

// ---------------------------------------------------------------------------
//  GET /v1/overlay/servers — ради флага search (гейт беты). Логика ретраев —
//  по образцу AdviceClient::onServersFinished (401→refresh, транспорт→авто-резерв).
// ---------------------------------------------------------------------------
void LawsClient::requestServers()
{
    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/servers")))};
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kLawsTimeoutMs);
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, &LawsClient::onServersFinished);
}

void LawsClient::onServersFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();

    const QByteArray data = reply->readAll();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netErr = reply->error();

    if (netErr != QNetworkReply::NoError) {
        // 401/403 — access протух: обновляем токен и ОДИН раз перезапрашиваем. Ретрай — СТРОГО
        // после фактического обновления (requestRefresh асинхронный: немедленный повтор ушёл бы
        // со СТАРЫМ Bearer и словил бы второй 401): одноразовый connect на authorized + таймаут-
        // предохранитель (refresh мог кончиться needKey/сетью — тогда отдаём ошибку, защёлку
        // снимаем). Контекст this в connect — guard времени жизни клиента.
        if ((httpStatus == 401 || httpStatus == 403) && auth_ && !serversRetried_) {
            serversRetried_ = true;
            auto conn = std::make_shared<QMetaObject::Connection>();
            auto *retryTimeout = new QTimer(this);
            retryTimeout->setSingleShot(true);
            *conn = connect(auth_, &AuthClient::authorized, this,
                            [this, conn, retryTimeout](const QString &) {
                                QObject::disconnect(*conn);
                                retryTimeout->stop();
                                retryTimeout->deleteLater();
                                requestServers();   // повтор уже со СВЕЖИМ Bearer
                            });
            connect(retryTimeout, &QTimer::timeout, this, [this, conn, retryTimeout]() {
                QObject::disconnect(*conn);
                retryTimeout->deleteLater();
                serversRetried_ = false;   // разрешить следующую попытку (новый заход в раздел)
                emit serversError(QStringLiteral("Список серверов: авторизация не обновилась."));
            });
            retryTimeout->start(8000);
            auth_->requestRefresh();
            return;
        }
        // Транспортный провал на main → авто-резерв (B3). Если канал переключился — один ретрай.
        const bool transport = (httpStatus == 0 && conn::isTransport(static_cast<int>(netErr)));
        if (conn::reportNetworkResult(transport) && !serversRetried_) {
            serversRetried_ = true;
            requestServers();   // повтор уже по новому (relay) каналу
            return;
        }
        serversRetried_ = false;
        emit serversError(QStringLiteral("Список серверов: %1").arg(reply->errorString()));
        return;
    }

    conn::reportNetworkResult(false);
    serversRetried_ = false;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit serversError(QStringLiteral("Некорректный ответ списка серверов."));
        return;
    }
    emit serversReady(doc.object().value(QStringLiteral("projects")).toArray().toVariantList());
}
