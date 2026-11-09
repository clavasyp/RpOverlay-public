#pragma once

#include <QByteArray>

// Тонкая обёртка над Windows DPAPI (CryptProtectData/CryptUnprotectData).
// entropy — дополнительная соль, разделяющая назначения (токены ≠ hwid-ключ): один и тот же
// блоб нельзя расшифровать «не своим» назначением. Привязка к текущему Windows-юзеру+машине.
// На не-Windows — passthrough (чтобы модуль собирался и под другими ОС; целевая платформа — Win).
namespace dpapi {
QByteArray protect(const QByteArray &plain, const QByteArray &entropy);
QByteArray unprotect(const QByteArray &enc, const QByteArray &entropy);
}
