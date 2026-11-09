#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QPointer>

class AuthClient;
class QNetworkAccessManager;
class QNetworkReply;

// LawsClient — клиент поиска по законке (бета; серверный гибридный поиск fts+trgm+вектор).
//
//   POST {apiBaseUrl}/v1/overlay/laws/search        (Bearer) → {query, results:[Card], related:[Rel], degraded}
//   GET  {apiBaseUrl}/v1/overlay/laws/article/{id}  (Bearer) → карточка статьи целиком (+related)
//   GET  {apiBaseUrl}/v1/overlay/servers            (Bearer) → флаг search у серверов (гейт беты)
//
// Bearer — из AuthClient (как AdviceClient). URL строится на КАЖДЫЙ запрос через cfg().apiBaseUrl
// (активный канал мутируется setActiveChannel/авто-резервом). Исходы репортятся в
// conn::reportNetworkResult (авто-резерв B3). Таймауты КОРОТКИЕ (~12с): поиск по каталогу
// мгновенный — не держим юзера 105с как Q&A-генерацию.
//
// Ошибки — ОТДЕЛЬНЫМИ сигналами (не adviceError бара): searchError/articleError(code, human).
// Спец-коды: 403 → beta_closed («поиск в бете, тир не допущен»), 429 → rate_limited
// («подождите минуту»); прочие — {"error","code"} из тела либо сеть/HTTP.
class LawsClient : public QObject {
    Q_OBJECT
public:
    explicit LawsClient(AuthClient *auth, QObject *parent = nullptr);
    ~LawsClient() override;

    // Поиск по законке ВЫБРАННОГО сервера. Новый запрос отменяет предыдущий незавершённый.
    void requestSearch(const QString &query, const QString &serverId, int limit = 8);

    // Полная статья по id (из выдачи/избранного/related). Новый запрос отменяет предыдущий.
    void requestArticle(int id);

    // Фоновое автообновление ОДНОЙ закреплённой статьи (id): отдельный reply/сигналы, НЕ мешает
    // интерактивному requestArticle (тот одноразовый и рендерит вид статьи). 200 → favoriteRefreshed;
    // 404 → favoriteMissing (статья удалена); прочая ошибка → favoriteRefreshFailed (избранное не трогаем).
    void refreshFavorite(int id);

    // Список серверов (тот же /v1/overlay/servers, что у AdviceClient) — ради флага search
    // (каталог построен И тир юзера допущен): динамический гейт беты в UI.
    void requestServers();

    // Отменить незавершённые поиск/статью (смена режима бара, логаут, закрытие страницы).
    void cancelAll();

    // Отменить ТОЛЬКО летящую статью: навигация в хабе (новый поиск / возврат к выдаче или
    // истории) не должна дождаться устаревшей статьи — та отрендерилась бы ПОВЕРХ свежего вида.
    void cancelArticle();

    // Заголовок карточки статьи для UI (общий у хаба и бара): title (с кодом) → сырой
    // article_ref → код → «Статья №id». Поля могут быть null по контракту API.
    static QString cardCaption(const QVariantMap &card);

signals:
    // results/related — списки map'ов из API как есть (Card/Rel), см. шапку файла.
    void searchReady(QString query, QVariantList results, QVariantList related, bool degraded);
    void searchError(QString code, QString message);
    void articleReady(QVariantMap article);
    void articleError(QString code, QString message);
    void favoriteRefreshed(int id, QVariantMap article);   // автообновление: снимок статьи актуален
    void favoriteMissing(int id);                          // автообновление: статья удалена (404)
    void favoriteRefreshFailed(int id);                    // автообновление: транзиентный сбой (не трогать)
    void serversReady(QVariantList projects);   // [{project_id, display_name, servers:[{server_id, …, search}]}]
    void serversError(QString msg);

private:
    void onSearchFinished();
    void onArticleFinished();
    void onFavFinished();
    void onServersFinished();

    QString apiUrl(const QString &path) const;

    AuthClient *auth_ = nullptr;
    QNetworkAccessManager *nam_ = nullptr;

    QPointer<QNetworkReply> searchReply_;    // активный поиск (отменяемый)
    QPointer<QNetworkReply> articleReply_;   // активная загрузка статьи (отменяемая)
    QPointer<QNetworkReply> favReply_;       // фоновое автообновление избранного (своя очередь в хабе)
    int     favId_ = 0;                      // id статьи текущего автообновления избранного
    QString pendingQuery_;                   // запрос активного поиска (вернём в searchReady)
    QString pendingServerId_;                // server_id активного поиска (для ретрая по новому каналу)
    int     pendingLimit_ = 8;               // limit активного поиска (для ретрая)
    int     pendingArticleId_ = 0;           // id активной статьи (для ретрая по новому каналу)
    bool    searchRetried_ = false;          // поиск уже переотправлен после авто-переключения канала
    bool    articleRetried_ = false;         // статья уже переотправлена после авто-переключения канала
    bool serversRetried_ = false;            // /servers уже ретраили (401→refresh / авто-резерв)
};
