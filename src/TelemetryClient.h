#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>

class QNetworkAccessManager;

// Дуал-сенд детектов клиента (анти-дебаг / tamper / неудача подписи апдейта) на сервер по ОБОИМ
// каналам сразу — основной (overlay.turgovbot.ru) И резервный relay (relay.invalid:8443), не
// полагаясь на то, какой канал сейчас активен. Если main заблокирован Cloudflare/DPI, а relay жив
// (или наоборот) — алерт всё равно доходит. Сервер дедупит по event_id (идемпотентно).
//
// Оффлайн-устойчивость: если ОБА канала недоступны — событие кладётся в DPAPI-запечатанную очередь
// (%APPDATA%/<app>/telemetry_queue.dat) и досылается при следующем запуске (flushQueued()).
//
// Слой D плана усиления защиты. Эндпоинт сервера: POST /v1/license/telemetry (НЕаутентифицированный,
// принимает и неактивированных клиентов — анти-дебаг срабатывает ещё до входа).
class TelemetryClient : public QObject {
    Q_OBJECT
public:
    explicit TelemetryClient(QObject *parent = nullptr);

    // Отправить детект. kind: "debugger" | "tamper" | "manifest_sig" | "hwid_mismatch" | …
    // detail — человекочитаемое описание (для tamper — список расходящихся файлов и т.п.).
    void report(const QString &kind, const QString &detail);

    // Досылка ранее не отправленных событий из DPAPI-очереди (вызывать один раз на старте).
    void flushQueued();

private:
    QByteArray buildEvent(const QString &kind, const QString &detail) const;
    void sendBoth(const QByteArray &eventJson);         // POST на main И relay; оба упали → enqueue
    void enqueue(const QByteArray &eventJson);          // в DPAPI-очередь (с капом размера)
    QString queueFilePath() const;

    QNetworkAccessManager *nam_;
};
