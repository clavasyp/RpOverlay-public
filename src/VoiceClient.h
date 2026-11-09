#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QHash>
#include <QQueue>

QT_BEGIN_NAMESPACE
class QWebSocket;
class QTimer;
QT_END_NAMESPACE

class AuthClient;

// VoiceClient — потоковая отправка PCM-аудио в STT-бэкенд по WebSocket
// и приём транскрипта обратно.
//
// На каждый источник звука ("mic" — «ты», "other" — собеседник/loopback) держим
// отдельный QWebSocket. Перед открытием сокета запрашиваем у AuthClient одноразовый
// ws-тикет (он короткоживущий и расходуется при connect), собираем URL
//   {wsBaseUrl}/v1/voice/stream?ws_ticket=..&source=..&session_id=..
// и подключаемся (wss поддерживается QWebSocket нативно).
//
// Сервер шлёт ТОЛЬКО текстовые JSON-сообщения {type:'partial'|'final'|'error', text, source}
// — их парсим и эмитим transcript(). Кадры, пришедшие до открытия сокета, буферизуем.
//
// Endless-streaming: серверная сессия живёт <=5 мин, поэтому по таймеру (~4 мин)
// и при неожиданном закрытии переоткрываем сокет с НОВЫМ тикетом, не теряя кадры
// (на время переоткрытия они копятся в буфере и сливаются после OPEN).
class VoiceClient : public QObject {
    Q_OBJECT
public:
    explicit VoiceClient(AuthClient *auth, QObject *parent = nullptr);
    ~VoiceClient() override;

    // Запустить стриминг в рамках сессии sessionId.
    // micOnly=true → открываем ТОЛЬКО источник "mic" (режим Q&A / push-to-talk:
    // звук игры не нужен, чтобы бот не слышал посторонний звук).
    void start(QString sessionId, bool micOnly = false);
    // Жёстко остановить и оборвать все сокеты (смена режима / выход).
    void stop();
    // Мягко завершить фразу: дослать буфер, отправить серверу {"type":"stop"},
    // подержать сокеты открытыми (grace) чтобы дождаться финального транскрипта,
    // затем закрыть. Используется при отпускании PTT / выключении «Слушать» в Q&A,
    // иначе abort() рвёт соединение до прихода final и хвост речи теряется.
    void finish();

public slots:
    // Поступил кадр PCM LINEAR16 16kHz mono для источника source ("mic"|"other").
    // Буферизуем и шлём в сокет соответствующего источника.
    void onFrame(QByteArray pcm16, QString source);

signals:
    // Получено событие транскрипта: type ∈ {'partial','final','error'}.
    void transcript(QString type, QString text, QString source);

private slots:
    void onWsTicketReady(QString forSource, QString ticket);
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &message);

private:
    // Состояние одного источника звука.
    struct SourceState {
        QString source;                 // "mic" | "other"
        QWebSocket *socket = nullptr;    // текущий сокет (может пересоздаваться)
        QQueue<QByteArray> buffer;       // кадры в ожидании OPEN
        QTimer *reopenTimer = nullptr;   // ~4 мин до планового переоткрытия
        QTimer *retryTimer = nullptr;    // backoff при сбое/закрытии
        bool open = false;               // сокет открыт и готов принимать кадры
        bool ticketPending = false;      // ws-тикет уже запрошен, ждём ответа
        bool reopening = false;          // плановое переоткрытие (не сбрасывать буфер)
        bool stopSent = false;           // в режиме finish() уже послали серверу {"type":"stop"}
    };

    void beginConnect(SourceState &st);        // запросить тикет и (по ответу) открыть сокет
    void openSocket(SourceState &st, const QString &ticket);
    void flushBuffer(SourceState &st);
    void scheduleReopen(SourceState &st);      // плановое переоткрытие через ~4 мин
    void scheduleRetry(SourceState &st);       // повтор после сбоя
    void teardownSocket(SourceState &st);      // отсоединить и удалить текущий сокет
    SourceState *stateForSocket(QObject *socket);

    AuthClient *auth_ = nullptr;
    QString sessionId_;
    bool running_ = false;
    bool micOnly_ = false;                      // true → работаем только с источником "mic"
    bool finishing_ = false;                    // идёт мягкое завершение фразы (finish): дренаж до final
    QTimer *finishTimer_ = nullptr;             // грейс при finish(): добить сокеты, если сервер не закрыл
    QHash<QString, SourceState *> sources_;     // "mic","other" -> состояние

    static constexpr int kReopenMs = 4 * 60 * 1000;   // плановое переоткрытие <5 мин
    static constexpr int kRetryMs = 2000;             // backoff после сбоя
    static constexpr int kMaxBufferFrames = 600;      // потолок буфера (~ при сбое сети)
    static constexpr int kFinishGraceMs = 9000;       // ждём final после {"type":"stop"} до принуд. закрытия (chirp финализирует не мгновенно; +запас на хоп через RU-релей)
};
