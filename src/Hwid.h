#pragma once

#include <QString>
#include <QByteArray>
#include <QMap>

// Аппаратный отпечаток (HWID) для лицензионной привязки «1 ключ = 1 ПК».
//
// Собирает несколько стабильных идентификаторов железа (SMBIOS UUID + baseboard, MachineGuid,
// серийник системного диска, CPU, MAC) БЕЗ WMI/COM (только лёгкие WinAPI). Даёт:
//   • strictId   — опаковый sha256-hex (его сервер банит/индексирует),
//   • components — имя→sha256-hex каждого компонента (для нечёткого матча на сервере при дрейфе),
//   • publicKey  — ed25519-pub пер-машинного ключа (приват derive из железа, DPAPI-кэш).
// signChallenge() подписывает серверный nonce пер-машинным ключом — доказывает, что HWID посчитан
// НА ЭТОЙ машине (бьёт replay HWID с чужого ПК). Результат мемоизируется (расчёт <50мс, без WMI).
namespace hwid {

struct Fingerprint {
    QString               strictId;     // sha256-hex — опаковый HWID
    QMap<QString, QString> components;  // имя → sha256-hex компонента
    QByteArray            publicKey;    // ed25519 pub (raw 32 байта)
};

const Fingerprint &get();                            // мемоизировано (ленивый расчёт + DPAPI-кэш ключа)
QByteArray signChallenge(const QByteArray &nonce);   // ed25519 над "RPOVERLAY-CHAL-v1\n"+strictId+"\n"+nonce
void recompute();                                    // принудительный пересчёт (отладка/ребайнд)

} // namespace hwid
