#pragma once

// ed25519 ПУБЛИЧНЫЙ ключ для проверки подписи установщика в авто-апдейте (UpdateClient).
// 32 сырых байта (raw SubjectPublicKeyInfo без 12-байтового DER-префикса).
//
// Приватный ключ (подпись релизов) лежит ТОЛЬКО на машине релиза, ВНЕ репозитория:
//   %USERPROFILE%\.rpoverlay-release\ed25519_private.pem   (НЕ коммитить!)
// release.sh подписывает им sha256-дайджест установщика; manifest.json несёт base64-подпись.
// Ротация ключа = перегенерировать пару, обновить эти байты и пересобрать клиент.
static const unsigned char RPOVERLAY_UPDATE_PUBKEY[32] = {
    0xc1, 0x8c, 0xfc, 0xf6, 0x4d, 0xe9, 0xe1, 0x29, 0x17, 0x8f, 0x51, 0xb5,
    0xa2, 0x2d, 0x38, 0x3b, 0xde, 0x7e, 0xf5, 0xe6, 0x4d, 0x46, 0xe2, 0x9e,
    0x4a, 0x85, 0x9b, 0x5b, 0xfa, 0xf5, 0x31, 0x0e
};
