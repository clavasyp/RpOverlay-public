#pragma once

// Self-signed сертификат RU-сервера-прокладки (резервный канал, см. src/Connectivity.*).
// ПУБЛИЧНАЯ часть — не секрет (как update_pubkey.h). Клиент добавляет её в доверенные
// (QSslConfiguration::addCaCertificate), чтобы TLS к резервному каналу
// https://relay.invalid:8443 валидировался (cert имеет SAN IP:relay.invalid).
// Приватный ключ лежит ТОЛЬКО на RU-сервере (/etc/caddy/relay.key), в репозиторий не попадает.
// Ротация: перевыпустить на сервере (openssl ... -addext subjectAltName=IP:...), обновить этот
// PEM и выпустить новую версию клиента (долетит авто-апдейтом).
static const char *RPOVERLAY_RELAY_CERT_PEM = R"PEM(
)PEM";
