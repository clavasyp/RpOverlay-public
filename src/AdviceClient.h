#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QVariantList>
#include <QByteArray>
#include <QPointer>

class AuthClient;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// AdviceClient — клиент эндпоинтов «совета» и «чек-листа» голосового оверлея.
//
//   POST {apiBaseUrl}/v1/overlay/advise/stream  (Bearer) — SSE-стрим:
//        sources → delta(ы) → done | error
//   POST {apiBaseUrl}/v1/overlay/checklist      (Bearer) — обычный JSON:
//        {server_id, mode, items:[{id, text, status, note}]}
//
// Bearer-токен берётся из AuthClient. Совет читается ПОТОКОВО (по readyRead),
// буфер SSE накапливается и режется по "\n\n", из событий берутся строки "data:".
// Новый requestAdvice/requestChecklist/requestQa отменяет предыдущий незавершённый запрос
// своего типа. Ошибки чек-листа НЕ идут в adviceError (он вспомогателен и летит параллельно
// SSE-совету — его 429/5xx не должны рушить живой стрим), только в qWarning.
class AdviceClient : public QObject {
    Q_OBJECT
public:
    explicit AdviceClient(AuthClient *auth, QObject *parent = nullptr);
    ~AdviceClient() override;

    // Потоковый совет по ситуации. mode ∈ advocate|prosecutor|general.
    // deep — глубокий разбор (Pro-модель). sessionId — для rolling-памяти диалога.
    // serverId — ВЫБРАННАЯ законка (пусто → cfg().serverId).
    // projectId — игровой проект для режима «Обзвон» (выбор свода правил); пусто → единый свод Majestic.
    void requestAdvice(QString situation, QString mode, bool deep, QString sessionId,
                       QString serverId = QString(), QString projectId = QString());

    // Чек-лист процедуры под роль (один JSON-ответ). serverId — выбранная законка (пусто → cfg().serverId).
    void requestChecklist(QString situation, QString mode, QString sessionId, QString serverId = QString());

    // Быстрый Q&A «как у бота»: вопрос → краткий ответ по законке ВЫБРАННОГО сервера (не-стрим JSON).
    void requestQa(QString question, QString serverId, QString sessionId);

    // Список серверов законки (проект→сервер) для пикера в Q&A-режиме.
    void requestServers();

    // Проекты/сервера со сводом правил для режима «Обзвон» (GET /v1/overlay/leadership/servers).
    void requestLeadershipServers();

    // Отменить текущий незавершённый advise-стрим И параллельный чек-лист (если есть):
    // оба — артефакты одной сцены, и переживший cancelAdvice чек-лист влезал бы в чужой этап.
    void cancelAdvice();

    // Отменить незавершённый Q&A-запрос. Зовётся окном из setMode/onLogout симметрично
    // cancelAdvice: запоздавший qaAnswer не должен глушить чужой failsafe нового режима.
    void cancelQa();

signals:
    // Имена/сигнатуры выровнены под слоты OverlayWindow.
    void sources(QStringList sources, int chunksUsed);
    void delta(QString text);
    void done();
    void adviceError(QString msg);
    void checklistReady(QVariantList items);
    void qaAnswer(QString answer);            // ответ Q&A (HTML, рендерить через setHtml)
    void serversReady(QVariantList projects); // [{project_id, display_name, servers:[{server_id, display_name}]}]
    void serversError(QString msg);           // не удалось получить список серверов (позволяет ретрай)
    void leadershipServersReady(QVariantList projects); // проекты/сервера со сводом «Обзвона»
    void leadershipServersError(QString msg);

private:
    void onAdviceReadyRead();
    void onAdviceFinished();
    void onChecklistFinished();
    void onQaFinished();
    void onServersFinished();
    void onLeadershipServersFinished();

    // Разобрать накопленный SSE-буфер: вырезать завершённые события ("...\n\n"),
    // из каждого взять "data:"-строку и обработать её как JSON.
    void drainSseBuffer();
    void handleSseData(const QByteArray &jsonStr);

    QString apiUrl(const QString &path) const;

    AuthClient *auth_ = nullptr;
    QNetworkAccessManager *nam_ = nullptr;

    QPointer<QNetworkReply> adviceReply_;     // активный SSE-стрим (отменяемый)
    QTimer *adviceIdle_ = nullptr;             // idle-таймаут SSE: abort при N с тишины (как idle_ в Updater)
    QPointer<QNetworkReply> checklistReply_;  // активный запрос чек-листа (отменяется вместе со стримом)
    QPointer<QNetworkReply> qaReply_;         // активный Q&A-запрос (отменяемый: cancelQa/новый requestQa)
    QByteArray sseBuffer_;                     // накопитель сырого SSE-потока
    bool adviceErrored_ = false;               // event:error уже отдан → не дублировать на finished
    bool adviceFinishedEmitted_ = false;       // done/error уже отдан → не отдавать повторно
    bool serversRetried_ = false;              // /servers уже ретраили (401→refresh / авто-резерв) — не зацикливать
    bool ldrServersRetried_ = false;           // /leadership/servers уже ретраили — не зацикливать
};
