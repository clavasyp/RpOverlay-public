#include "Connectivity.h"
#include "Config.h"
#include "Settings.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QDebug>

namespace {
constexpr int kMainProbeMs  = 2500;   // проба основного канала
constexpr int kRelayProbeMs = 3500;   // проба резервного канала (доп. хоп RU→CF)

// Сколько подряд транспортных провалов на main терпим до авто-переключения на relay (B3).
constexpr int kAutoRelayFailThreshold = 2;
// Счётчик подряд-фейлов активного канала (сбрасывается любым успехом/HTTP-ответом).
int g_consecutiveNetFails = 0;

// Решающая API-проба: GET {base}/v1/overlay/servers БЕЗ токена. Эндпоинт за Bearer →
// быстрый 401, но это значит, что API ЖИВ и доезжает (а не режется DPI на уровне CF/SNI).
// true = получили ЛЮБОЙ HTTP-код (401/200/4xx/5xx) за таймаут. false = таймаут / код 0 /
// транспортная ошибка (host not found, connection refused, TLS) — канал реально не доходит.
// ВАЖНО: статик-манифест /updates/* проходит через CF-кэш даже когда API порезан, поэтому
// решающей пробой служит ИМЕННО API-эндпоинт.
bool probeApi(const QString &base, int timeoutMs)
{
    if (base.isEmpty())
        return false;

    QNetworkAccessManager nam;
    nam.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    QNetworkRequest req{QUrl(base + QStringLiteral("/v1/overlay/servers"))};
    req.setRawHeader("Cache-Control", "no-cache");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = nam.get(req);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    bool ok = false;
    if (!timedOut) {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // ЛЮБОЙ валидный HTTP-код = API доехал (даже 401/403/5xx — это уже ОТВЕТ сервера, а
        // не блок канала). code==0 без таймаута = транспортная ошибка (TLS/refused/host) → резать.
        ok = (code > 0);
    }
    if (timedOut)
        reply->abort();
    reply->deleteLater();
    return ok;
}
}  // namespace

namespace conn {

QString chooseChannel(const QString &mode)
{
    const QString m = mode.trimmed().isEmpty() ? QStringLiteral("auto") : mode.trimmed().toLower();
    g_consecutiveNetFails = 0;   // явный выбор канала обнуляет рантайм-счётчик авто-резерва

    if (m == QLatin1String("main")) {
        qInfo() << "[conn] mode=main (форс основной)";
        settings::setConnLast(QStringLiteral("main"));   // connLast должен отражать реальный канал
        return QStringLiteral("main");
    }
    if (m == QLatin1String("relay")) {
        qInfo() << "[conn] mode=relay (форс резервный)";
        settings::setConnLast(QStringLiteral("relay"));
        return QStringLiteral("relay");
    }

    // auto: всегда пробуем основной (само-восстановление, если сеть разблокировалась).
    // РЕШАЮЩАЯ проба — по API-эндпоинту (статик-манифест проходит CF-кэш даже при порезанном API).
    // ВАЖНО: пробим ИМЕННО основной домен (mainApiBaseUrl()), а НЕ cfg().apiBaseUrl — последний
    // мутируется setActiveChannel() и после ухода на relay указывает на relay; иначе «Авто» при
    // активном relay пробует relay, считает основной живым и садит клиента на заблокированный main.
    if (probeApi(mainApiBaseUrl(), kMainProbeMs)) {
        qInfo() << "[conn] auto: основной канал (API) работает";
        settings::setConnLast(QStringLiteral("main"));
        return QStringLiteral("main");
    }

    qWarning() << "[conn] auto: основной API недоступен → пробую резервный (RU-прокладка)";
    if (probeApi(cfg().relayApiBaseUrl, kRelayProbeMs)) {
        qInfo() << "[conn] auto: резервный канал (API) работает → переключаюсь";
        settings::setConnLast(QStringLiteral("relay"));
        return QStringLiteral("relay");
    }

    qWarning() << "[conn] auto: и основной, и резервный API недоступны — остаюсь на основном";
    settings::setConnLast(QStringLiteral("main"));   // connLast должен отражать реальный канал
    return QStringLiteral("main");
}

bool isTransport(int qNetworkError)
{
    // Транспортные провалы (канал не доезжает) — повод для авто-резерва. Ответы сервера
    // (Content*, Protocol*, любые HTTP 4xx/5xx) НЕ считаем: канал-то жив.
    switch (static_cast<QNetworkReply::NetworkError>(qNetworkError)) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:   // наш transferTimeout/abort по таймауту (B2)
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::UnknownNetworkError:
        return true;
    default:
        return false;
    }
}

bool reportNetworkResult(bool netError)
{
    // Авто-резерв работает только когда сейчас активен main и режим auto: при форс-main/relay
    // юзер выбрал канал явно — не перебиваем его. (При relay уже на резерве — переключать некуда.)
    if (activeChannel() != QLatin1String("main"))
        { g_consecutiveNetFails = 0; return false; }
    if (settings::connMode().compare(QLatin1String("auto"), Qt::CaseInsensitive) != 0)
        { g_consecutiveNetFails = 0; return false; }

    if (!netError) {
        g_consecutiveNetFails = 0;   // успех/HTTP-ответ → канал жив
        return false;
    }

    if (++g_consecutiveNetFails < kAutoRelayFailThreshold)
        return false;

    qWarning().noquote() << "[conn] авто-резерв:" << g_consecutiveNetFails
                         << "подряд сетевых провалов на main → переключаюсь на relay";
    g_consecutiveNetFails = 0;
    setActiveChannel(QStringLiteral("relay"));
    settings::setConnLast(QStringLiteral("relay"));
    return true;
}

}  // namespace conn
