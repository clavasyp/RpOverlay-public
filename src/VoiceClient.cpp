#include "VoiceClient.h"
#include "AuthClient.h"

#include <QWebSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAbstractSocket>
#include <QDebug>

#include "Config.h"

VoiceClient::VoiceClient(AuthClient *auth, QObject *parent)
    : QObject(parent), auth_(auth)
{
    // Готовим состояния обоих источников заранее — onFrame может прийти для любого.
    for (const QString &src : {QStringLiteral("mic"), QStringLiteral("other")}) {
        auto *st = new SourceState;
        st->source = src;
        sources_.insert(src, st);
    }

    // Тикеты приходят асинхронно. Сопоставление с источником — по forSource.
    if (auth_) {
        // AuthClient шлёт (ticket, forSource); слот ждёт (forSource, ticket) — переставляем местами.
        connect(auth_, &AuthClient::wsTicketReady, this,
                [this](QString ticket, QString forSource) { onWsTicketReady(forSource, ticket); });
    }
}

VoiceClient::~VoiceClient()
{
    stop();
    qDeleteAll(sources_);
    sources_.clear();
}

void VoiceClient::start(QString sessionId, bool micOnly)
{
    // Если предыдущая фраза ещё дренажится (finish не завершён) — жёстко сбрасываем,
    // чтобы не переиспользовать сокет, которому уже послан {"type":"stop"}.
    if (finishing_)
        stop();

    sessionId_ = sessionId;
    micOnly_ = micOnly;
    running_ = true;
    finishing_ = false;
    if (finishTimer_) finishTimer_->stop();   // отменяем возможный незавершённый finish()

    for (SourceState *st : sources_) {
        st->stopSent = false;
        // В micOnly работаем только с микрофоном — звук игры не открываем.
        if (micOnly_ && st->source != QStringLiteral("mic"))
            continue;
        // Уже открыт/в процессе — не дёргаем повторно.
        if (st->open || st->ticketPending || st->socket)
            continue;
        beginConnect(*st);
    }
}

void VoiceClient::finish()
{
    // Мягкое завершение фразы. КЛЮЧЕВОЕ ОТЛИЧИЕ от stop(): не выбрасываем накопленный
    // буфер и НЕ рвём ещё-подключающийся сокет. Если соединение в процессе (ws-тикет
    // запрошен / идёт wss-handshake), даём ему завершиться — по onConnected дольём буфер
    // и пошлём {"type":"stop"}. Если сокет уже открыт — шлём {stop} сразу. Сервер по {stop}
    // доскажет final и закроет соединение сам.
    //
    // running_ намеренно ОСТАВЛЯЕМ true: иначе onWsTicketReady/onConnected/scheduleRetry
    // ранним выходом оборвали бы ещё-летящее подключение, и весь хвост речи (а при быстром
    // отпускании PTT — вообще вся фраза) потерялся бы. Новые кадры дальше не принимаем —
    // onFrame отбросит их по finishing_.
    if (finishing_)
        return;
    finishing_ = true;

    bool pendingOrOpen = false;
    for (SourceState *st : sources_) {
        // Плановое переоткрытие в режиме финализации не нужно (но ретрай подключения —
        // оставляем: ещё-неподключённому источнику он даёт шанс открыться и дослать буфер).
        st->reopening = false;
        if (st->reopenTimer) st->reopenTimer->stop();

        if (st->open && st->socket && st->socket->state() == QAbstractSocket::ConnectedState) {
            // Сокет открыт — дослать хвост аудио и попросить финал.
            flushBuffer(*st);
            st->socket->sendTextMessage(QStringLiteral("{\"type\":\"stop\"}"));
            st->stopSent = true;
            pendingOrOpen = true;       // держим открытым — final придёт текстовым сообщением
        } else if (st->ticketPending || st->socket || !st->buffer.isEmpty()
                   || (st->retryTimer && st->retryTimer->isActive())) {
            // Подключение ещё идёт (или ждёт ретрая) и есть что дослать — НЕ рвём и НЕ чистим
            // буфер. {stop} уйдёт из onConnected сразу после flushBuffer.
            st->stopSent = false;
            pendingOrOpen = true;
        } else {
            // Нечего досылать и нечего ждать — просто снести.
            teardownSocket(*st);
            st->buffer.clear();
            st->open = false;
        }
    }

    if (!pendingOrOpen) {
        finishing_ = false;
        running_ = false;
        return;
    }

    // Грейс: если сокет так и не открылся / сервер не прислал final и не закрыл — добиваем.
    if (!finishTimer_) {
        finishTimer_ = new QTimer(this);
        finishTimer_->setSingleShot(true);
        connect(finishTimer_, &QTimer::timeout, this, [this]() {
            // Жёсткий backstop полной финализации: тушим всё, как stop().
            running_ = false;
            finishing_ = false;
            for (SourceState *st : sources_) {
                st->reopening = false;
                st->ticketPending = false;
                st->stopSent = false;
                if (st->reopenTimer) st->reopenTimer->stop();
                if (st->retryTimer) st->retryTimer->stop();
                teardownSocket(*st);
                st->buffer.clear();
                st->open = false;
            }
        });
    }
    finishTimer_->start(kFinishGraceMs);
}

void VoiceClient::stop()
{
    running_ = false;
    finishing_ = false;
    if (finishTimer_) finishTimer_->stop();

    for (SourceState *st : sources_) {
        st->reopening = false;
        st->ticketPending = false;
        st->stopSent = false;
        if (st->reopenTimer) st->reopenTimer->stop();
        if (st->retryTimer) st->retryTimer->stop();
        teardownSocket(*st);
        st->buffer.clear();
        st->open = false;
    }
}

void VoiceClient::onFrame(QByteArray pcm16, QString source)
{
    if (!running_ || pcm16.isEmpty())
        return;
    // Фраза завершается (finish): новые кадры не принимаем — буфер уже дослан/дошлётся,
    // серверу отправлен/будет отправлен {stop}. Иначе аудио ушло бы ПОСЛЕ stop-маркера.
    if (finishing_)
        return;
    // В micOnly игнорируем всё, кроме микрофона (звук игры не стримим).
    if (micOnly_ && source != QStringLiteral("mic"))
        return;

    SourceState *st = sources_.value(source, nullptr);
    if (!st) {
        // Неизвестный источник — заводим состояние на лету.
        st = new SourceState;
        st->source = source;
        sources_.insert(source, st);
    }

    if (st->open && st->socket && st->socket->state() == QAbstractSocket::ConnectedState) {
        st->socket->sendBinaryMessage(pcm16);
        return;
    }

    // Сокет ещё не открыт (или переоткрывается) — буферизуем, чтобы не терять начало фразы.
    st->buffer.enqueue(pcm16);
    while (st->buffer.size() > kMaxBufferFrames)
        st->buffer.dequeue();  // защита от безграничного роста при долгом обрыве

    // Если ничего не инициировано (например, кадр пришёл до start() по источнику) — заводим коннект.
    if (running_ && !st->open && !st->ticketPending && !st->socket)
        beginConnect(*st);
}

void VoiceClient::beginConnect(SourceState &st)
{
    if (!auth_ || !running_)
        return;
    st.ticketPending = true;
    // Тикет одноразовый и короткоживущий (≈30с) — запрашиваем непосредственно перед open.
    auth_->requestWsTicket(st.source);
}

void VoiceClient::onWsTicketReady(QString forSource, QString ticket)
{
    SourceState *st = sources_.value(forSource, nullptr);
    if (!st)
        return;
    if (!st->ticketPending)
        return;  // не наш запрос / уже устаревший
    st->ticketPending = false;

    if (!running_)
        return;
    if (ticket.isEmpty()) {
        // Не удалось получить тикет — повторим с backoff.
        scheduleRetry(*st);
        return;
    }
    openSocket(*st, ticket);
}

void VoiceClient::openSocket(SourceState &st, const QString &ticket)
{
    // Если по какой-то причине остался старый сокет — снимаем его.
    teardownSocket(st);

    QUrl url(cfg().wsBaseUrl + QStringLiteral("/v1/voice/stream"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("ws_ticket"), ticket);
    q.addQueryItem(QStringLiteral("source"), st.source);
    q.addQueryItem(QStringLiteral("session_id"), sessionId_);
    url.setQuery(q);

    st.socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(st.socket, &QWebSocket::connected, this, &VoiceClient::onConnected);
    connect(st.socket, &QWebSocket::disconnected, this, &VoiceClient::onDisconnected);
    connect(st.socket, &QWebSocket::textMessageReceived, this, &VoiceClient::onTextMessage);
    connect(st.socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
            this, [this, sock = st.socket](QAbstractSocket::SocketError) {
                if (sock)
                    qWarning() << "[Voice] WS error:" << sock->errorString();
                // disconnected придёт следом и запустит retry/reopen.
            });

    st.socket->open(url);
}

void VoiceClient::onConnected()
{
    SourceState *st = stateForSocket(sender());
    if (!st)
        return;

    st->open = true;
    st->reopening = false;
    if (st->retryTimer)
        st->retryTimer->stop();

    qInfo() << "[Voice] WS connected source=" << st->source;

    flushBuffer(*st);

    if (finishing_) {
        // Финализация фразы началась ДО того, как сокет открылся (типично для быстрого
        // отпускания PTT): теперь буфер дослан — сразу просим финал и НЕ планируем reopen.
        if (!st->stopSent && st->socket
            && st->socket->state() == QAbstractSocket::ConnectedState) {
            st->socket->sendTextMessage(QStringLiteral("{\"type\":\"stop\"}"));
            st->stopSent = true;
        }
        return;
    }

    scheduleReopen(*st);  // через ~4 мин — плановое переоткрытие с новым тикетом
}

void VoiceClient::onDisconnected()
{
    SourceState *st = stateForSocket(sender());
    if (!st)
        return;

    const bool wasOpen = st->open;
    st->open = false;
    if (st->reopenTimer)
        st->reopenTimer->stop();

    // Сокет, инициировавший сигнал, уже не нужен — удалим отложенно.
    if (st->socket) {
        st->socket->deleteLater();
        st->socket = nullptr;
    }

    if (!running_)
        return;

    if (finishing_) {
        // Сервер закрыл сокет после final — нормальное завершение фразы. НЕ реконнектим
        // и НЕ ретраим (иначе после {stop} крутили бы бесполезные переподключения).
        st->buffer.clear();
        st->stopSent = false;
        return;
    }

    if (st->reopening) {
        // Плановое endless-переоткрытие: сразу запрашиваем новый тикет.
        st->reopening = false;
        beginConnect(*st);
    } else {
        // Неожиданное закрытие — повтор с backoff (кадры продолжают копиться в буфере).
        qWarning() << "[Voice] WS closed source=" << st->source << "wasOpen=" << wasOpen;
        scheduleRetry(*st);
    }
}

void VoiceClient::onTextMessage(const QString &message)
{
    SourceState *st = stateForSocket(sender());
    const QString fallbackSource = st ? st->source : QString();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[Voice] non-JSON WS message:" << message.left(200);
        return;
    }
    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    const QString text = obj.value(QStringLiteral("text")).toString();
    QString source = obj.value(QStringLiteral("source")).toString();
    if (source.isEmpty())
        source = fallbackSource;

    if (type.isEmpty())
        return;

    emit transcript(type, text, source);
}

void VoiceClient::flushBuffer(SourceState &st)
{
    if (!st.socket || st.socket->state() != QAbstractSocket::ConnectedState)
        return;
    while (!st.buffer.isEmpty())
        st.socket->sendBinaryMessage(st.buffer.dequeue());
}

void VoiceClient::scheduleReopen(SourceState &st)
{
    if (!st.reopenTimer) {
        st.reopenTimer = new QTimer(this);
        st.reopenTimer->setSingleShot(true);
        const QString src = st.source;
        connect(st.reopenTimer, &QTimer::timeout, this, [this, src]() {
            SourceState *s = sources_.value(src, nullptr);
            if (!s || !running_ || !s->open || !s->socket)
                return;
            // Помечаем плановое переоткрытие и закрываем текущий сокет —
            // в onDisconnected по флагу reopening возьмём новый тикет.
            s->reopening = true;
            s->open = false;
            s->socket->close();  // буфер не трогаем — кадры дольют после OPEN
        });
    }
    st.reopenTimer->start(kReopenMs);
}

void VoiceClient::scheduleRetry(SourceState &st)
{
    if (!running_)
        return;
    if (!st.retryTimer) {
        st.retryTimer = new QTimer(this);
        st.retryTimer->setSingleShot(true);
        const QString src = st.source;
        connect(st.retryTimer, &QTimer::timeout, this, [this, src]() {
            SourceState *s = sources_.value(src, nullptr);
            if (!s || !running_)
                return;
            if (s->open || s->ticketPending || s->socket)
                return;
            beginConnect(*s);
        });
    }
    st.retryTimer->start(kRetryMs);
}

void VoiceClient::teardownSocket(SourceState &st)
{
    if (!st.socket)
        return;
    QWebSocket *sock = st.socket;
    st.socket = nullptr;
    st.open = false;
    sock->disconnect(this);            // отписаться от его сигналов
    if (sock->state() != QAbstractSocket::UnconnectedState)
        sock->abort();
    sock->deleteLater();
}

VoiceClient::SourceState *VoiceClient::stateForSocket(QObject *socket)
{
    for (SourceState *st : sources_) {
        if (st->socket == socket)
            return st;
    }
    return nullptr;
}
