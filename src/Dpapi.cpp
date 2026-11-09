#include "Dpapi.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <wincrypt.h>
#endif

namespace dpapi {

#ifdef Q_OS_WIN

QByteArray protect(const QByteArray &plain, const QByteArray &entropy)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
    in.cbData = static_cast<DWORD>(plain.size());

    QByteArray eb(entropy);
    DATA_BLOB ent{};
    ent.pbData = reinterpret_cast<BYTE *>(eb.data());
    ent.cbData = static_cast<DWORD>(eb.size());

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"RpOverlay", eb.isEmpty() ? nullptr : &ent, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    QByteArray result(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    if (out.pbData)
        LocalFree(out.pbData);
    return result;
}

QByteArray unprotect(const QByteArray &enc, const QByteArray &entropy)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(enc.constData()));
    in.cbData = static_cast<DWORD>(enc.size());

    QByteArray eb(entropy);
    DATA_BLOB ent{};
    ent.pbData = reinterpret_cast<BYTE *>(eb.data());
    ent.cbData = static_cast<DWORD>(eb.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, eb.isEmpty() ? nullptr : &ent, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    QByteArray result(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    if (out.pbData)
        LocalFree(out.pbData);
    return result;
}

#else  // не-Windows — passthrough (целевая платформа Win; здесь не используется)

QByteArray protect(const QByteArray &plain, const QByteArray &) { return plain; }
QByteArray unprotect(const QByteArray &enc, const QByteArray &) { return enc; }

#endif

} // namespace dpapi
