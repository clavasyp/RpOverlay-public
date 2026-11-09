#pragma once

#include <QString>

// Конфиг клиента. Лениво грузится из config.json при первом cfg() (рядом с exe / cwd).
struct AppConfig {
    QString apiBaseUrl;        // АКТИВНЫЙ http(s) base (основной ИЛИ резервный — см. setActiveChannel)
    QString wsBaseUrl;         // АКТИВНЫЙ ws(s) base
    QString serverId;          // напр. orlando
    QString botUsername;       // имя бота для дип-линка t.me/<bot>?start=link_<код>
    // Резервный канал — через RU-сервер-прокладку (он reverse-proxy'ит на основной домен).
    // Клиент переключается на него, если основной канал не отвечает (DPI-блок Cloudflare).
    QString relayApiBaseUrl;   // напр. https://relay.invalid:8443
    QString relayWsBaseUrl;    // напр. wss://relay.invalid:8443
};

const AppConfig &cfg();   // глобальный доступ (ленивая загрузка config.json)
QString deviceId();       // стабильный per-устройство id (persist в %APPDATA%)

// Переключить АКТИВНЫЙ канал: "main" (основной домен) или "relay" (через RU-прокладку).
// Подменяет cfg().apiBaseUrl/wsBaseUrl; вызывать ДО первых сетевых запросов. Идемпотентно.
void setActiveChannel(const QString &channel);
QString activeChannel();  // "main" | "relay"

// ОСНОВНОЙ http(s) base — ВСЕГДА главный домен, независимо от активного канала (cfg().apiBaseUrl
// после setActiveChannel("relay") становится relay). Нужен для дуал-сенда телеметрии/команд на ОБА
// канала сразу (main + relay), не полагаясь на то, какой канал сейчас активен. См. TelemetryClient.
const QString &mainApiBaseUrl();
