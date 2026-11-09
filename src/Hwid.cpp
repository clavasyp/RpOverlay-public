#include "Hwid.h"
#include "Dpapi.h"
#include "Obfusc.h"

#include <QCryptographicHash>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtGlobal>

#include <cstring>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <windows.h>
#  include <winioctl.h>
#  include <cpuid.h>
#endif

#include <openssl/evp.h>

namespace {

// Версионированные префиксы доменного разделения. Канон challenge ОБЯЗАН совпадать с сервером
// (services/license.py: b"RPOVERLAY-CHAL-v1\n" + hwid + b"\n" + nonce).
const QByteArray kHwidPrefix     = QByteArrayLiteral("RPOVERLAY-HWID-v1\n");
const QByteArray kCompPrefix     = QByteArrayLiteral("RPOVERLAY-COMP-v1\n");
const QByteArray kChalPrefix     = QByteArrayLiteral("RPOVERLAY-CHAL-v1\n");
const QByteArray kKeySeedSalt    = QByteArrayLiteral("RPOVERLAY-KEYSEED-v1\n");
const QByteArray kHwidKeyEntropy = QByteArrayLiteral("RpOverlay.hwidkey.v1");

QByteArray sha256(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256);
}
QString sha256hex(const QByteArray &b)
{
    return QString::fromLatin1(sha256(b).toHex());
}

// Нормализация компонента: upper+trim, известный мусор/нули/FF → пусто (вес 0).
QString norm(QString s)
{
    s = s.trimmed().toUpper();
    if (s.isEmpty())
        return QString();
    // Список «мусорных» серийников обфусцирован (B1): не лежит открытым ASCII — реверсеру сложнее
    // понять, какие значения занулять, чтобы подделать стабильный фингерпринт под VM/спуфинг.
    static const QStringList junk = {
        OBF("TO BE FILLED BY O.E.M."), OBF("DEFAULT STRING"),
        OBF("NONE"), OBF("SYSTEM SERIAL NUMBER"),
        OBF("SERIAL NUMBER"), OBF("NOT SPECIFIED"),
        OBF("BASE BOARD SERIAL NUMBER"), OBF("DEFAULT"),
        OBF("0"), OBF("00000000-0000-0000-0000-000000000000"),
        OBF("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"),
    };
    if (junk.contains(s))
        return QString();
    QString flat = s;
    flat.remove(' ');
    flat.remove('-');
    if (flat.isEmpty())
        return QString();
    bool allF = true, all0 = true;
    for (QChar c : flat) {
        if (c != QLatin1Char('F')) allF = false;
        if (c != QLatin1Char('0')) all0 = false;
    }
    if (allF || all0)
        return QString();
    return s;
}

#ifdef Q_OS_WIN

QString regMachineGuid()
{
    wchar_t buf[256];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                     L"MachineGuid", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        return QString::fromWCharArray(buf);
    return QString();
}

// SMBIOS (RawSMBIOSData): 8-байтовый заголовок, далее DMI-структуры. Парсим Type 1 (System UUID)
// и Type 2 (Baseboard serial). Только чтение прошивки, без COM/WMI.
struct SmbiosVals { QString systemUuid; QString baseboardSerial; };

SmbiosVals parseSmbios()
{
    SmbiosVals v;
    const DWORD RSMB = (DWORD('R') << 24) | (DWORD('S') << 16) | (DWORD('M') << 8) | DWORD('B');
    UINT need = GetSystemFirmwareTable(RSMB, 0, nullptr, 0);
    if (!need)
        return v;
    QByteArray buf(static_cast<int>(need), Qt::Uninitialized);
    UINT got = GetSystemFirmwareTable(RSMB, 0, buf.data(), need);
    if (!got || got > need || buf.size() < 8)
        return v;

    const quint8 *p = reinterpret_cast<const quint8 *>(buf.constData());
    quint32 dmiLen = 0;
    std::memcpy(&dmiLen, p + 4, 4);               // RawSMBIOSData.Length
    const quint8 *data = p + 8;
    const quint8 *end = data + qMin<quint32>(dmiLen, static_cast<quint32>(buf.size() - 8));

    const quint8 *cur = data;
    while (cur + 4 <= end) {
        const quint8 type = cur[0];
        const quint8 len = cur[1];
        if (len < 4 || cur + len > end)
            break;
        const quint8 *formatted = cur;
        const quint8 *strs = cur + len;

        auto getStr = [&](quint8 idx) -> QString {
            if (idx == 0)
                return QString();
            const quint8 *s = strs;
            quint8 n = 1;
            while (s < end && *s) {
                const char *begin = reinterpret_cast<const char *>(s);
                while (s < end && *s) ++s;
                if (n == idx)
                    return QString::fromLatin1(begin, static_cast<int>(reinterpret_cast<const char *>(s) - begin));
                ++s;  // пропустить терминатор
                ++n;
            }
            return QString();
        };

        if (type == 1 && len >= 0x19) {
            const quint8 *u = formatted + 8;       // UUID, 16 байт
            char b[40];
            std::snprintf(b, sizeof(b),
                "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6],
                u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            v.systemUuid = QString::fromLatin1(b);
        } else if (type == 2 && len >= 0x08) {
            v.baseboardSerial = getStr(formatted[0x07]);
        }

        // Перейти к следующей структуре: после форматированной области — набор строк (двойной NUL).
        const quint8 *s = strs;
        if (s < end && *s == 0)
            ++s;                                   // нет строк: одиночный NUL
        else
            while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) ++s;
        cur = s + 2;
    }
    return v;
}

QString diskSerial()
{
    HANDLE h = CreateFileW(L"\\\\.\\PhysicalDrive0", 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return QString();
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {0};
    DWORD ret = 0;
    QString serial;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &ret, nullptr)) {
        auto *d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR *>(buf);
        if (d->SerialNumberOffset && d->SerialNumberOffset < sizeof(buf))
            serial = QString::fromLatin1(reinterpret_cast<const char *>(buf) + d->SerialNumberOffset).trimmed();
    }
    CloseHandle(h);
    return serial;
}

QString cpuId()
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    char vendor[13] = {0};
    if (__get_cpuid(0, &a, &b, &c, &d)) {
        std::memcpy(vendor + 0, &b, 4);
        std::memcpy(vendor + 4, &d, 4);
        std::memcpy(vendor + 8, &c, 4);
    }
    unsigned sig = 0;
    if (__get_cpuid(1, &a, &b, &c, &d))
        sig = a;                                   // family/model/stepping
    return QString::fromLatin1(vendor) + QLatin1Char('-') + QString::number(sig, 16);
}

QString macs()
{
    QStringList out;
    ULONG sz = 15000;
    QByteArray buf(static_cast<int>(sz), 0);
    auto *aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST;
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &sz);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(static_cast<int>(sz));
        aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &sz);
    }
    if (r != NO_ERROR)
        return QString();

    // Префиксы MAC виртуалок (Hyper-V/VMware/VBox/…) обфусцированы (B1): открытый список = прямая
    // подсказка, какой MAC подделать для обхода VM-детекта (инцидент §6). Themida STR_ENCRYPT добьёт.
    static const QStringList virtOui = {
        OBF("00:15:5D"), OBF("00:05:69"), OBF("00:0C:29"),
        OBF("00:50:56"), OBF("08:00:27"), OBF("0A:00:27"),
        OBF("00:03:FF"), OBF("02:"),  // locally-administered
    };
    for (auto *p = aa; p; p = p->Next) {
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK || p->IfType == IF_TYPE_TUNNEL)
            continue;
        if (p->PhysicalAddressLength != 6)
            continue;
        const BYTE *m = p->PhysicalAddress;
        char b[18];
        std::snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        QString mac = QString::fromLatin1(b);
        if (mac == QLatin1String("00:00:00:00:00:00"))
            continue;
        bool virt = false;
        for (const auto &o : virtOui)
            if (mac.startsWith(o)) { virt = true; break; }
        if (virt)
            continue;
        if (!out.contains(mac))
            out << mac;
    }
    out.sort();
    return out.join(QLatin1Char(','));
}

#else  // не-Windows: заглушки (целевая платформа — Win)
QString regMachineGuid() { return QString(); }
struct SmbiosVals { QString systemUuid, baseboardSerial; };
SmbiosVals parseSmbios() { return {}; }
QString diskSerial() { return QString(); }
QString cpuId() { return QString(); }
QString macs() { return QString(); }
#endif

// Упорядоченный набор нормализованных компонентов (QMap сортирует по ключу → детерминизм).
QMap<QString, QString> rawComponents()
{
    QMap<QString, QString> c;
    SmbiosVals smb = parseSmbios();
    c[QStringLiteral("smbiosUuid")]      = norm(smb.systemUuid);
    c[QStringLiteral("baseboardSerial")] = norm(smb.baseboardSerial);
    c[QStringLiteral("machineGuid")]     = norm(regMachineGuid());
    c[QStringLiteral("diskSerial")]      = norm(diskSerial());
    c[QStringLiteral("cpu")]             = norm(cpuId());
    c[QStringLiteral("mac")]             = norm(macs());
    return c;
}

// --- ed25519 (OpenSSL EVP) ---
QByteArray ed25519Pub(const QByteArray &seed32)
{
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(seed32.constData()), 32);
    if (!pk)
        return {};
    QByteArray pub(32, 0);
    size_t publen = 32;
    bool ok = EVP_PKEY_get_raw_public_key(pk, reinterpret_cast<unsigned char *>(pub.data()), &publen) == 1;
    EVP_PKEY_free(pk);
    if (!ok)
        return {};
    pub.resize(static_cast<int>(publen));
    return pub;
}

QByteArray ed25519Sign(const QByteArray &seed32, const QByteArray &msg)
{
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(seed32.constData()), 32);
    if (!pk)
        return {};
    QByteArray sig(64, 0);
    size_t siglen = 64;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = ctx
        && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pk) == 1
        && EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(sig.data()), &siglen,
                          reinterpret_cast<const unsigned char *>(msg.constData()),
                          static_cast<size_t>(msg.size())) == 1;
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    if (!ok)
        return {};
    sig.resize(static_cast<int>(siglen));
    return sig;
}

QString keySeedPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/hwid_key.dat");
}

// Приват-seed: берём из DPAPI-кэша (стабилен на этой машине/юзере); если нет — derive из железа
// и кэшируем. DPAPI привязан к юзеру+машине → на чужом ПК кэш не расшифруется (анти-replay).
QByteArray loadOrDeriveSeed(const QMap<QString, QString> &comps)
{
    QFile f(keySeedPath());
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        QByteArray enc = f.readAll();
        f.close();
        QByteArray seed = dpapi::unprotect(enc, kHwidKeyEntropy);
        if (seed.size() == 32)
            return seed;
    }
    // derive из стабильных компонентов
    QByteArray stable = kKeySeedSalt;
    for (const QString &k : {QStringLiteral("smbiosUuid"), QStringLiteral("machineGuid"),
                             QStringLiteral("diskSerial"), QStringLiteral("cpu")})
        stable += (k + QLatin1Char('=') + comps.value(k) + QLatin1Char('\n')).toUtf8();
    QByteArray seed = sha256(stable);              // 32 байта
    QByteArray enc = dpapi::protect(seed, kHwidKeyEntropy);
    if (!enc.isEmpty()) {
        QDir().mkpath(QFileInfo(keySeedPath()).absolutePath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(enc);
            f.close();
        }
    }
    return seed;
}

// Кэш в процессе.
struct Cache {
    hwid::Fingerprint fp;
    QByteArray seed;
    bool ready = false;
};
Cache &cache()
{
    static Cache c;
    return c;
}

void compute()
{
    Cache &c = cache();
    QMap<QString, QString> comps = rawComponents();

    QByteArray strictBuf = kHwidPrefix;
    QMap<QString, QString> compHashes;
    for (auto it = comps.constBegin(); it != comps.constEnd(); ++it) {
        strictBuf += (it.key() + QLatin1Char('=') + it.value() + QLatin1Char('\n')).toUtf8();
        if (!it.value().isEmpty())
            compHashes[it.key()] = sha256hex(kCompPrefix + (it.key() + QLatin1Char('=') + it.value()).toUtf8());
    }
    c.fp.strictId = sha256hex(strictBuf);
    c.fp.components = compHashes;

    c.seed = loadOrDeriveSeed(comps);
    c.fp.publicKey = ed25519Pub(c.seed);
    c.ready = true;
}

} // namespace

namespace hwid {

const Fingerprint &get()
{
    Cache &c = cache();
    if (!c.ready)
        compute();
    return c.fp;
}

QByteArray signChallenge(const QByteArray &nonce)
{
    const Fingerprint &fp = get();
    Cache &c = cache();
    if (c.seed.size() != 32)
        return {};
    QByteArray msg = kChalPrefix + fp.strictId.toUtf8() + QByteArrayLiteral("\n") + nonce;
    return ed25519Sign(c.seed, msg);
}

void recompute()
{
    cache().ready = false;
    compute();
}

} // namespace hwid
