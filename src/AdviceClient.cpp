#include "AdviceClient.h"

#include "AuthClient.h"
#include "Config.h"
#include "Connectivity.h"  // conn::reportNetworkResult/isTransport — авто-резерв (B3)
#include "Settings.h"   // settings::contextLimit() → history_limit в запросах

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QUrl>
#include <QDebug>

// Клиентские таймауты. Семантика Qt setTransferTimeout — ОБЩАЯ (вся передача должна уложиться
// в N мс от старта запроса; НЕ сбрасывается при данных — ср. Updater: им покрыт только крошечный
// манифест, а для докачки там отдельный idle-таймер). Поэтому:
//  • НЕ-стримовые qa/checklist/servers (один JSON-ответ) → общий setTransferTimeout = kHttpTimeoutMs.
//    105с: СТРОГО > серверного дедлайна 90с (успеет дойти ответ/быстрый 503), но < CF-cut ~120с.
//  • SSE advise/stream (данные приходят постепенно) → общий таймаут убил бы здоровый стрим; вместо
//    него idle-таймер kSseIdleTimeoutMs, сбрасываемый на readyRead, abort при тишине (≈ idle_ Updater).
//    95с: серверный дедлайн 90с + малый запас — полуоткрытый TCP без данных рвём, медленный стрим нет.
namespace {
constexpr int kHttpTimeoutMs    = 105000;  // qa/checklist/servers: общий таймаут запроса
constexpr int kSseIdleTimeoutMs = 95000;   // advise/stream: таймаут бездействия (сброс на readyRead)
} // namespace

// Тихо снять reply: отцепить сигналы (abort не должен породить finished-каскад в слоты),
// абортнуть и удалить. Общее для advise-стрима, чек-листа и Q&A.
static void dropReply(QPointer<QNetworkReply> &reply, QObject *receiver)
{
    if (!reply)
        return;
    QObject::disconnect(reply, nullptr, receiver, nullptr);
    reply->abort();
    reply->deleteLater();
    reply = nullptr;
}

AdviceClient::AdviceClient(AuthClient *auth, QObject *parent)
    : QObject(parent), auth_(auth)
{
    nam_ = new QNetworkAccessManager(this);
    // idle-таймаут SSE: если за kSseIdleTimeoutMs не пришло ни байта — рвём (полуоткрытый TCP).
    // abort даст onAdviceFinished с ошибкой → adviceError в UI (не «вечное Думаю»).
    adviceIdle_ = new QTimer(this);
    adviceIdle_->setSingleShot(true);
    connect(adviceIdle_, &QTimer::timeout, this, [this]() { if (adviceReply_) adviceReply_->abort(); });
}

AdviceClient::~AdviceClient()
{
    cancelAdvice();
}

QString AdviceClient::apiUrl(const QString &path) const
{
    QString base = cfg().apiBaseUrl;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base + path;
}

void AdviceClient::cancelAdvice()
{
    if (adviceIdle_) adviceIdle_->stop();
    dropReply(adviceReply_, this);
    // Чек-лист летит ПАРАЛЛЕЛЬНО advise-стриму и раньше был fire-and-forget: его запоздавший
    // ответ переживал setMode/onLogout/failsafe (все они зовут cancelAdvice) и влезал бы в
    // чужой этап (например, рендерился в режиме «Общий») — отменяем вместе со стримом.
    dropReply(checklistReply_, this);
    sseBuffer_.clear();
}

void AdviceClient::cancelQa()
{
    // Q&A-reply раньше не трекался вовсе: запоздавший ответ глушил в окне ЧУЖОЙ failsafe
    // (сценовый разбор/новый вопрос) — см. guard в OverlayWindow::onQaAnswer. Отмена здесь —
    // вторая линия: setMode/onLogout рубят запрос уходящего режима/аккаунта под корень.
    dropReply(qaReply_, this);
}

void AdviceClient::requestAdvice(QString situation, QString mode, bool deep, QString sessionId, QString serverId, QString projectId)
{
    // Новый запрос отменяет предыдущий незавершённый стрим.
    cancelAdvice();

    adviceErrored_ = false;
    adviceFinishedEmitted_ = false;
    sseBuffer_.clear();

    QJsonObject body;
    body.insert(QStringLiteral("server_id"), serverId.isEmpty() ? cfg().serverId : serverId);
    body.insert(QStringLiteral("situation"), situation);
    body.insert(QStringLiteral("deep"), deep);
    if (!mode.isEmpty())
        body.insert(QStringLiteral("mode"), mode);
    if (!projectId.isEmpty())
        body.insert(QStringLiteral("project_id"), projectId);   // «Обзвон»: выбор свода правил (иначе фолбэк Majestic)
    if (!sessionId.isEmpty())
        body.insert(QStringLiteral("session_id"), sessionId);
    body.insert(QStringLiteral("history_limit"), settings::contextLimit());
    // A/B модели совета: "auto" — решает сервер; иначе просим конкретную (flash/lite).
    const QString tier = settings::adviceModelTier();
    if (tier != QLatin1String("auto"))
        body.insert(QStringLiteral("model_tier"), tier);

    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/advise/stream")))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "text/event-stream");
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    adviceReply_ = nam_->post(req, payload);

    connect(adviceReply_, &QNetworkReply::readyRead, this, &AdviceClient::onAdviceReadyRead);
    connect(adviceReply_, &QNetworkReply::finished, this, &AdviceClient::onAdviceFinished);
    if (adviceIdle_) adviceIdle_->start(kSseIdleTimeoutMs);   // тишина > kSseIdleTimeoutMs → abort
}

void AdviceClient::onAdviceReadyRead()
{
    if (!adviceReply_)
        return;
    if (adviceIdle_) adviceIdle_->start(kSseIdleTimeoutMs);   // пришли данные — продлеваем окно тишины
    sseBuffer_.append(adviceReply_->readAll());
    drainSseBuffer();
}

void AdviceClient::drainSseBuffer()
{
    // События SSE разделены пустой строкой ("\n\n"); нагрузка — в строках "data:".
    int sep;
    while ((sep = sseBuffer_.indexOf("\n\n")) != -1) {
        const QByteArray rawEvent = sseBuffer_.left(sep);
        sseBuffer_.remove(0, sep + 2);

        // В событии может быть несколько строк; берём первую "data:".
        const QList<QByteArray> lines = rawEvent.split('\n');
        for (const QByteArray &line : lines) {
            // Срезаем возможный завершающий '\r' (CRLF-переводы строк).
            QByteArray l = line;
            if (l.endsWith('\r'))
                l.chop(1);
            if (!l.startsWith("data:"))
                continue;
            QByteArray jsonStr = l.mid(5).trimmed();
            if (!jsonStr.isEmpty())
                handleSseData(jsonStr);
            break;
        }
    }
}

void AdviceClient::handleSseData(const QByteArray &jsonStr)
{
    if (adviceFinishedEmitted_)
        return;

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonStr, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return; // битый кадр — молча пропускаем, как и Electron-клиент

    const QJsonObject obj = doc.object();
    const QString event = obj.value(QStringLiteral("event")).toString();

    if (event == QLatin1String("sources")) {
        QStringList srcList;
        const QJsonArray arr = obj.value(QStringLiteral("sources")).toArray();
        srcList.reserve(arr.size());
        for (const QJsonValue &v : arr)
            srcList.append(v.toString());
        const int chunksUsed = obj.value(QStringLiteral("chunks_used")).toInt();
        emit sources(srcList, chunksUsed);
    } else if (event == QLatin1String("delta")) {
        emit delta(obj.value(QStringLiteral("text")).toString());
    } else if (event == QLatin1String("done")) {
        adviceFinishedEmitted_ = true;
        emit done();
    } else if (event == QLatin1String("error")) {
        adviceErrored_ = true;
        adviceFinishedEmitted_ = true;
        QString msg = obj.value(QStringLiteral("message")).toString();
        if (msg.isEmpty())
            msg = QStringLiteral("Ошибка сервера при генерации совета.");
        emit adviceError(msg);
    }
}

void AdviceClient::onAdviceFinished()
{
    if (adviceIdle_) adviceIdle_->stop();
    QNetworkReply *reply = adviceReply_;
    if (!reply)
        return;

    // Дочитать остаток буфера (на случай данных без хвостового readyRead).
    sseBuffer_.append(reply->readAll());
    drainSseBuffer();

    const QNetworkReply::NetworkError netErr = reply->error();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Авто-резерв (B3): транспортный провал на main двигает счётчик; HTTP-ответ/успех — сбрасывает.
    conn::reportNetworkResult(netErr != QNetworkReply::NoError
                              && httpStatus == 0
                              && conn::isTransport(static_cast<int>(netErr)));

    adviceReply_ = nullptr;
    reply->deleteLater();

    if (adviceFinishedEmitted_) {
        sseBuffer_.clear();
        return; // done/error уже отдан из потока
    }

    // Отмена пользователем (abort через cancelAdvice) сюда не дойдёт — коннекты сняты.
    if (netErr != QNetworkReply::NoError) {
        adviceFinishedEmitted_ = true;
        QString msg = QStringLiteral("Сеть: %1").arg(reply->errorString());
        if (httpStatus > 0)
            msg = QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(reply->errorString());
        emit adviceError(msg);
    } else if (httpStatus != 0 && httpStatus != 200) {
        adviceFinishedEmitted_ = true;
        emit adviceError(QStringLiteral("HTTP %1").arg(httpStatus));
    } else {
        // Поток закрылся без явного done — считаем завершённым.
        adviceFinishedEmitted_ = true;
        emit done();
    }
    sseBuffer_.clear();
}

void AdviceClient::requestChecklist(QString situation, QString mode, QString sessionId, QString serverId)
{
    QJsonObject body;
    body.insert(QStringLiteral("server_id"), serverId.isEmpty() ? cfg().serverId : serverId);
    body.insert(QStringLiteral("situation"), situation);
    if (!mode.isEmpty())
        body.insert(QStringLiteral("mode"), mode);
    if (!sessionId.isEmpty())
        body.insert(QStringLiteral("session_id"), sessionId);
    body.insert(QStringLiteral("history_limit"), settings::contextLimit());

    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/checklist")))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kHttpTimeoutMs);   // > 90с дедлайна, < ~120с CF-cut (общий: один JSON-ответ)
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());

    // Новый чек-лист отменяет предыдущий незавершённый (как requestAdvice со стримом):
    // иначе чек-лист ПРОШЛОЙ сцены мог влезть под совет новой.
    dropReply(checklistReply_, this);

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    checklistReply_ = nam_->post(req, payload);
    connect(checklistReply_, &QNetworkReply::finished, this, &AdviceClient::onChecklistFinished);
}

void AdviceClient::onChecklistFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    if (reply == checklistReply_)
        checklistReply_ = nullptr;

    const QByteArray data = reply->readAll();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        QString msg = QStringLiteral("Сеть: %1").arg(reply->errorString());
        if (httpStatus > 0) {
            const QJsonDocument edoc = QJsonDocument::fromJson(data);
            const QString detail = edoc.isObject()
                                       ? edoc.object().value(QStringLiteral("detail")).toString()
                                       : QString();
            msg = detail.isEmpty()
                      ? QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(reply->errorString())
                      : QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(detail);
        }
        // НЕ adviceError: чек-лист вспомогателен и летит ПАРАЛЛЕЛЬНО advise-стриму — его
        // ошибка (типовой кейс: 429 квоты Gemini) рушила бы в окне ЖИВОЙ SSE-совет (затёртый
        // текст, погашенный failsafe, сбитая фаза). Лейбл чек-листа просто останется скрыт.
        qWarning().noquote() << "[advice] чек-лист не получен:" << msg;
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        qWarning().noquote() << "[advice] чек-лист: некорректный ответ (не JSON-объект)";
        return;
    }

    const QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();
    emit checklistReady(items.toVariantList());
}

// ---------------------------------------------------------------------------
//  Q&A (как у бота): POST /v1/overlay/qa — один JSON-ответ {answer, ...}.
//  server_id = ВЫБРАННЫЙ в пикере (не cfg().serverId).
// ---------------------------------------------------------------------------
void AdviceClient::requestQa(QString question, QString serverId, QString sessionId)
{
    QJsonObject body;
    body.insert(QStringLiteral("server_id"), serverId.isEmpty() ? cfg().serverId : serverId);
    body.insert(QStringLiteral("question"), question);
    if (!sessionId.isEmpty())
        body.insert(QStringLiteral("session_id"), sessionId);
    body.insert(QStringLiteral("history_limit"), settings::contextLimit());

    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/qa")))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kHttpTimeoutMs);   // > 90с дедлайна, < ~120с CF-cut (общий: один JSON-ответ)
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());

    // Новый вопрос отменяет предыдущий незавершённый: после failsafe Thinking окно снова в Idle
    // и юзер может спросить ещё — поздний ответ Q1 иначе перетёр бы пару «вопрос-ответ» Q2
    // и погасил бы его страховку.
    cancelQa();

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    qaReply_ = nam_->post(req, payload);
    connect(qaReply_, &QNetworkReply::finished, this, &AdviceClient::onQaFinished);
}

void AdviceClient::onQaFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    if (reply == qaReply_)
        qaReply_ = nullptr;

    const QByteArray data = reply->readAll();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netErr = reply->error();

    // Авто-резерв (B3): транспортный провал на main двигает счётчик; HTTP-ответ/успех — сбрасывает.
    conn::reportNetworkResult(netErr != QNetworkReply::NoError
                              && httpStatus == 0
                              && conn::isTransport(static_cast<int>(netErr)));

    if (netErr != QNetworkReply::NoError) {
        QString msg = QStringLiteral("Сеть: %1").arg(reply->errorString());
        if (httpStatus > 0) {
            const QJsonDocument edoc = QJsonDocument::fromJson(data);
            const QString detail = edoc.isObject()
                                       ? edoc.object().value(QStringLiteral("detail")).toString()
                                       : QString();
            msg = detail.isEmpty()
                      ? QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(reply->errorString())
                      : QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(detail);
        }
        emit adviceError(msg);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit adviceError(QStringLiteral("Некорректный ответ Q&A."));
        return;
    }
    emit qaAnswer(doc.object().value(QStringLiteral("answer")).toString());
}

// ---------------------------------------------------------------------------
//  Список серверов: GET /v1/overlay/servers → {projects:[{project_id, display_name,
//  servers:[{server_id, display_name}]}]}. Отдаём как QVariantList в UI.
// ---------------------------------------------------------------------------
void AdviceClient::requestServers()
{
    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/servers")))};
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());
    req.setTransferTimeout(kHttpTimeoutMs);   // короткий GET, но не висеть до CF-524 на полуоткрытом TCP

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, &AdviceClient::onServersFinished);
}

void AdviceClient::onServersFinished()
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
        // 401/403 — access протух: обновляем токен и ОДИН раз перезапрашиваем (образец
        // AuthClient::onWsTicketFinished). serversRetried_ защёлкивает от зацикливания.
        if ((httpStatus == 401 || httpStatus == 403) && auth_ && !serversRetried_) {
            serversRetried_ = true;
            auth_->requestRefresh();
            requestServers();
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

    conn::reportNetworkResult(false);   // успех → сбросить счётчик авто-резерва
    serversRetried_ = false;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit serversError(QStringLiteral("Некорректный ответ списка серверов."));
        return;
    }
    emit serversReady(doc.object().value(QStringLiteral("projects")).toArray().toVariantList());
}

// ---------------------------------------------------------------------------
//  Своды «Обзвона»: GET /v1/overlay/leadership/servers → {projects:[...]} — те же поля, что
//  у /servers, но только проекты/сервера с проиндексированным сводом лидерских правил.
// ---------------------------------------------------------------------------
void AdviceClient::requestLeadershipServers()
{
    QNetworkRequest req{QUrl(apiUrl(QStringLiteral("/v1/overlay/leadership/servers")))};
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Bearer ") + auth_->accessToken().toUtf8());
    req.setTransferTimeout(kHttpTimeoutMs);   // короткий GET, но не висеть до CF-524 на полуоткрытом TCP

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, &AdviceClient::onLeadershipServersFinished);
}

void AdviceClient::onLeadershipServersFinished()
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
        // 401/403 — access протух: refresh + ОДИН ретрай (защёлка ldrServersRetried_).
        if ((httpStatus == 401 || httpStatus == 403) && auth_ && !ldrServersRetried_) {
            ldrServersRetried_ = true;
            auth_->requestRefresh();
            requestLeadershipServers();
            return;
        }
        // Транспортный провал на main → авто-резерв (B3); переключился канал — один ретрай.
        const bool transport = (httpStatus == 0 && conn::isTransport(static_cast<int>(netErr)));
        if (conn::reportNetworkResult(transport) && !ldrServersRetried_) {
            ldrServersRetried_ = true;
            requestLeadershipServers();
            return;
        }
        ldrServersRetried_ = false;
        emit leadershipServersError(QStringLiteral("Своды «Обзвона»: %1").arg(reply->errorString()));
        return;
    }

    conn::reportNetworkResult(false);   // успех → сбросить счётчик авто-резерва
    ldrServersRetried_ = false;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit leadershipServersError(QStringLiteral("Некорректный ответ сводов «Обзвона»."));
        return;
    }
    emit leadershipServersReady(doc.object().value(QStringLiteral("projects")).toArray().toVariantList());
}
