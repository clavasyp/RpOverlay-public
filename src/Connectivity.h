#pragma once

#include <QString>

// Connectivity — выбор канала связи на старте.
//
// Основной канал: напрямую на overlay.turgovbot.ru (за Cloudflare).
// Резервный канал: через RU-сервер-прокладку (cfg().relayApiBaseUrl), если основной
// недоступен (провайдерский DPI режет Cloudflare по SNI). RU-сервер reverse-proxy'ит
// запросы на тот же основной домен.
//
// chooseChannel() синхронный (под сплэшем): быстрая HTTP-проба. Возвращает "main"|"relay".
// ВАЖНО: звать ДО создания сетевых клиентов; результат применять через setActiveChannel().
// Если используется резервный канал — перед пробой relay в дефолтный QSslConfiguration
// должен быть добавлен self-signed серт релея (см. relay_cert.h, делается в main.cpp).
namespace conn {

// mode: "auto" (проба→фолбэк) | "main" (форс основной) | "relay" (форс резервный).
// Пишет итог в settings::setConnLast(). Возвращает выбранный канал ("main"|"relay").
QString chooseChannel(const QString &mode);

// --- Авто-резерв в рантайме (B3) ---
// Клиенты (AdviceClient/AuthClient) сообщают сюда исход реального запроса на АКТИВНЫЙ канал.
// Учитываются ТОЛЬКО транспортные провалы (таймаут/connection refused/host not found/
// remote-closed) — НЕ 4xx/5xx от самого сервера (значит канал-то жив). N подряд таких на
// канале main → авто-переключение на relay (setActiveChannel("relay")) с пометкой connLast.
// Любой УСПЕХ (или ответ с HTTP-кодом) обнуляет счётчик.
//
// netError — true, если это транспортный провал (см. conn::isTransport()).
// Возвращает true, если ИМЕННО этот вызов выполнил авто-переключение на relay
// (вызывающему стоит повторить запрос/переинициализироваться).
bool reportNetworkResult(bool netError);

// Классификатор: транспортный ли это провал (имеет смысл для авто-резерва), а не ответ сервера.
// Принимает QNetworkReply::NetworkError как int, чтобы не тянуть QtNetwork в заголовок.
bool isTransport(int qNetworkError);

}  // namespace conn
