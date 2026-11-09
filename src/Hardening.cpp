#include "Hardening.h"
#include "Config.h"
#include "Obfusc.h"
#include "ThemidaMarks.h"   // RP_CHECK_* сторожа (no-op без -DWITH_THEMIDA_SDK)
#include "update_pubkey.h"   // RPOVERLAY_UPDATE_PUBKEY[32]

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QUrl>
#include <QDebug>

#include <openssl/evp.h>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace {
constexpr int kManifestTimeoutMs = 8000;
constexpr int kFirstCheckDelayMs = 12000;   // после Updater (8с), чтобы не толкаться за канал

// Критичный набор для integrity (а не все ~40 файлов — экономим CPU и держим сигнал острым):
// сам exe + апдейтер (подменяет файлы) + OpenSSL (проверка подписей) + Qt-сеть/ядро. Пути — как
// в manifest-v2 (относительно корня dist, прямые слэши). Файла нет в манифесте → молча пропускаем.
const char *kCriticalFiles[] = {
    "RpOverlay.exe", "RpUpdater.exe",
    "libcrypto-3-x64.dll", "libssl-3-x64.dll",
    "Qt6Network.dll", "Qt6Core.dll",
};

// ed25519: msg (сырые байты) против sig (64 байта) ключом pub32. (Идентично Updater/UpdateClient —
// в этом коде established-паттерн копировать крошечный верификатор, а не тащить общий модуль.)
bool ed25519Verify(const QByteArray &msg, const QByteArray &sig, const unsigned char *pub32)
{
    if (sig.size() != 64)
        return false;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub32, 32);
    if (!pkey)
        return false;
    bool ok = false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1)
        ok = EVP_DigestVerify(ctx,
                              reinterpret_cast<const unsigned char *>(sig.constData()),
                              static_cast<size_t>(sig.size()),
                              reinterpret_cast<const unsigned char *>(msg.constData()),
                              static_cast<size_t>(msg.size())) == 1;
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}
} // namespace

Hardening::Hardening(QObject *parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
    , periodic_(new QTimer(this))
{
    connect(periodic_, &QTimer::timeout, this, &Hardening::checkNow);
}

Hardening::~Hardening() = default;

// ---------------- анти-дебаг (мгновенно, без сети) ----------------
QJsonObject Hardening::antiDebugSnapshot()
{
    QJsonObject o;
#ifdef Q_OS_WIN
    o.insert(QStringLiteral("debugger"), bool(IsDebuggerPresent()));

    BOOL remote = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
    o.insert(QStringLiteral("remote_debugger"), bool(remote));

    // PEB напрямую (ловит обнуление BeingDebugged анти-анти-дебаг патчами реже, чем API, но дёшево).
    // x64: TEB self → +0x60 указатель на PEB; PEB.BeingDebugged=+0x02, PEB.NtGlobalFlag=+0xBC.
    // NtCurrentTeb() есть и в MSVC, и в MinGW-w64 (inline gs:[0x30]) → кросс-компиляторно.
    bool peb = false, heap = false;
  #if defined(_M_X64) || defined(__x86_64__)
    if (const auto *teb = reinterpret_cast<const unsigned char *>(NtCurrentTeb())) {
        const auto *pebPtr = *reinterpret_cast<const unsigned char *const *>(teb + 0x60);
        if (pebPtr) {
            peb  = pebPtr[0x02] != 0;
            const DWORD ntGlobalFlag = *reinterpret_cast<const DWORD *>(pebPtr + 0xBC);
            heap = (ntGlobalFlag & 0x70) != 0;   // FLG_HEAP_ENABLE_TAIL_CHECK|FREE_CHECK|VALIDATE_PARAMETERS
        }
    }
  #endif
    o.insert(QStringLiteral("peb_being_debugged"), peb);
    o.insert(QStringLiteral("heap_debug_flags"), heap);

    // --- B2: расширенный анти-дебаг (дёшево, без FP на легите) ---
    // NtQueryInformationProcess: ProcessDebugPort(7) / ProcessDebugObjectHandle(30) / ProcessDebugFlags(31).
    bool ntDbgPort = false, ntDbgObj = false, ntDbgFlags = false;
    {
        typedef LONG(NTAPI * NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        static NtQIP_t NtQIP = reinterpret_cast<NtQIP_t>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        if (NtQIP) {
            HANDLE self = GetCurrentProcess();
            DWORD_PTR port = 0;
            if (NtQIP(self, 7, &port, sizeof(port), nullptr) == 0 && port != 0)
                ntDbgPort = true;
            HANDLE dbgObj = nullptr;
            if (NtQIP(self, 30, &dbgObj, sizeof(dbgObj), nullptr) == 0 && dbgObj != nullptr) {
                ntDbgObj = true;
                CloseHandle(dbgObj);   // владеемый хендл — закрываем (иначе утечка под отладкой)
            }
            DWORD dflags = 1;
            if (NtQIP(self, 31, &dflags, sizeof(dflags), nullptr) == 0 && dflags == 0)
                ntDbgFlags = true;   // 0 = NoDebugInherit снят = процесс под отладкой
        }
    }
    o.insert(QStringLiteral("nt_debug_port"), ntDbgPort);
    o.insert(QStringLiteral("nt_debug_object"), ntDbgObj);
    o.insert(QStringLiteral("nt_debug_flags"), ntDbgFlags);

    // Hardware breakpoints (DR0–DR3) текущего потока — выставляет отладчик.
    bool hwbp = false;
  #if defined(_M_X64) || defined(__x86_64__)
    {
        CONTEXT ctx;
        ZeroMemory(&ctx, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(GetCurrentThread(), &ctx))
            hwbp = (ctx.Dr0 || ctx.Dr1 || ctx.Dr2 || ctx.Dr3);
    }
  #endif
    o.insert(QStringLiteral("hw_breakpoints"), hwbp);

    // Родительский процесс — имя в списке известных отладчиков/RE-тулов (мягкий сигнал).
    bool parentDbg = false;
    {
        const DWORD myPid = GetCurrentProcessId();
        DWORD parentPid = 0;
        QString parentName;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (pe.th32ProcessID == myPid) { parentPid = pe.th32ParentProcessID; break; }
                } while (Process32NextW(snap, &pe));
            }
            if (parentPid) {
                pe.dwSize = sizeof(pe);
                if (Process32FirstW(snap, &pe)) {
                    do {
                        if (pe.th32ProcessID == parentPid) {
                            parentName = QString::fromWCharArray(pe.szExeFile).toLower();
                            break;
                        }
                    } while (Process32NextW(snap, &pe));
                }
            }
            CloseHandle(snap);
        }
        if (!parentName.isEmpty()) {
            static const QStringList dbgNames = {
                OBF("x64dbg.exe"), OBF("x32dbg.exe"), OBF("x96dbg.exe"), OBF("ollydbg.exe"),
                OBF("ida.exe"), OBF("ida64.exe"), OBF("windbg.exe"), OBF("dnspy.exe"),
                OBF("cheatengine"),
            };
            for (const QString &n : dbgNames)
                if (parentName.contains(n)) { parentDbg = true; break; }
        }
    }
    o.insert(QStringLiteral("parent_debugger"), parentDbg);
#endif
    return o;
}

void Hardening::reportAntiDebug()
{
    const QJsonObject ad = antiDebugSnapshot();
    const bool any = ad.value(QStringLiteral("debugger")).toBool()
                  || ad.value(QStringLiteral("remote_debugger")).toBool()
                  || ad.value(QStringLiteral("peb_being_debugged")).toBool()
                  || ad.value(QStringLiteral("heap_debug_flags")).toBool()
                  || ad.value(QStringLiteral("nt_debug_port")).toBool()
                  || ad.value(QStringLiteral("nt_debug_object")).toBool()
                  || ad.value(QStringLiteral("nt_debug_flags")).toBool()
                  || ad.value(QStringLiteral("hw_breakpoints")).toBool()
                  || ad.value(QStringLiteral("parent_debugger")).toBool();
    if (any) {
        const QString detail = QString::fromUtf8(QJsonDocument(ad).toJson(QJsonDocument::Compact));
        qWarning().noquote() << "[hardening] анти-дебаг сигнал:" << detail;
        emit anomaly(QStringLiteral("debugger"), detail);
    }
}

QJsonObject Hardening::snapshot() const
{
    QJsonObject o = antiDebugSnapshot();
    o.insert(QStringLiteral("version"), QStringLiteral(APP_VERSION));
    o.insert(QStringLiteral("integrity_checked"), integrityChecked_);
    o.insert(QStringLiteral("integrity_ok"), integrityOk_);
    if (!tampered_.isEmpty()) {
        QJsonArray a;
        for (const QString &p : tampered_)
            a.append(p);
        o.insert(QStringLiteral("tampered"), a);
    }
    return o;
}

// ---------------- цикл ----------------
void Hardening::startPeriodic(int minutes)
{
    QTimer::singleShot(kFirstCheckDelayMs, this, &Hardening::checkNow);
    periodic_->start(qMax(1, minutes) * 60 * 1000);
}

void Hardening::checkThemidaProtection()
{
#if defined(WITH_THEMIDA_SDK)
    // Сторожа Themida (Слой A) — активны ТОЛЬКО в защищённом билде. Themida выставляет var в ожидаемое
    // значение, если файл защищён/не вскрыт; расхождение → вскрытие/дамп/инлайн-хук → tamper в телеметрию.
    // Блокирующий анти-дебаг отдаём самой Themida (binary-level); здесь — лишь СИГНАЛ серверу, не краш.
    unsigned int prot = 0;
    RP_CHECK_PROTECTION(prot, 0x33333337u);
    if (prot != 0x33333337u) {
        emit anomaly(QStringLiteral("tamper"), QStringLiteral("themida_unprotected_or_dumped"));
        return;
    }
    unsigned int ci = 0;
    RP_CHECK_CODE_INTEGRITY(ci, 0x55555551u);
    if (ci != 0x55555551u) {
        emit anomaly(QStringLiteral("tamper"), QStringLiteral("code_integrity_patch"));
        return;
    }
    unsigned int dbg = 0;
    RP_CHECK_DEBUGGER(dbg, 0x11111119u);
    if (dbg != 0x11111119u)
        emit anomaly(QStringLiteral("debugger"), QStringLiteral("themida_debugger"));
#endif
}

void Hardening::checkNow()
{
    reportAntiDebug();        // дёшево и без сети — всегда
    checkThemidaProtection(); // сторожа Themida (no-op в обычном билде)
    if (reply_)
        return;               // integrity уже в полёте — не дублируем
    fetchManifest();
}

int Hardening::compareSemver(const QString &a, const QString &b)
{
    auto parse = [](const QString &v) {
        QVector<int> out;
        for (const QString &seg : v.trimmed().split('.')) {
            int i = 0;
            while (i < seg.size() && seg.at(i).isDigit()) ++i;
            out.push_back(seg.left(i).toInt());
            if (i < seg.size()) break;
        }
        while (out.size() < 3) out.push_back(0);
        return out;
    };
    const QVector<int> va = parse(a), vb = parse(b);
    for (int i = 0; i < 3; ++i) {
        if (va.at(i) < vb.at(i)) return -1;
        if (va.at(i) > vb.at(i)) return 1;
    }
    return 0;
}

QString Hardening::sha256File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) { f.close(); return QString(); }
    f.close();
    return QString::fromLatin1(h.result().toHex());
}

void Hardening::fetchManifest()
{
    // integrity-сверка ДОЛЖНА брать манифест СВОЕГО канала, иначе хэши не сойдутся → ложный tamper.
    // Публичный QA-билд (RPOVERLAY_QA) — отдельный бинарь на manifest-v2-qa.json; full — manifest-v2.json.
#if defined(RPOVERLAY_LAWS)
    // laws-билд сверяется со СВОИМ каналом (иначе ложный tamper, как в инциденте QA 0.3.1)
    const QString manifestPath = QStringLiteral("/updates/manifest-v2-laws.json");
#elif defined(RPOVERLAY_QA)
    const QString manifestPath = QStringLiteral("/updates/manifest-v2-qa.json");
#else
    // full-канал с 0.4.1 = manifest-v2-full.json (старый manifest-v2.json — вечная QA-лечилка)
    const QString manifestPath = QStringLiteral("/updates/manifest-v2-full.json");
#endif
    QNetworkRequest req{QUrl(cfg().apiBaseUrl + manifestPath)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("Cache-Control", "no-cache");
    req.setTransferTimeout(kManifestTimeoutMs);
    reply_ = nam_->get(req);
    connect(reply_, &QNetworkReply::finished, this, &Hardening::onManifestFinished);
}

void Hardening::onManifestFinished()
{
    QNetworkReply *r = reply_;
    reply_ = nullptr;
    if (!r) return;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError)
        return;   // нет связи — тихо, integrity отложим до следующего тика

    const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
    const QString version      = o.value(QStringLiteral("version")).toString();
    const QString minSupported = o.value(QStringLiteral("min_supported")).toString();
    const bool    mandatory    = o.value(QStringLiteral("mandatory")).toBool(false);
    const QString sig          = o.value(QStringLiteral("sig")).toString();

    QVector<FileEntry> files;
    for (const QJsonValue &v : o.value(QStringLiteral("files")).toArray()) {
        const QJsonObject fo = v.toObject();
        FileEntry e;
        e.path   = fo.value(QStringLiteral("path")).toString();
        e.size   = static_cast<qint64>(fo.value(QStringLiteral("size")).toDouble());
        e.sha256 = fo.value(QStringLiteral("sha256")).toString().toLower();
        if (!e.path.isEmpty())
            files.push_back(e);
    }
    if (version.isEmpty() || files.isEmpty() || sig.isEmpty())
        return;
    if (!verifyManifestSig(version, minSupported, mandatory, files, sig))
        return;   // не доверяем неподписанному манифесту для integrity-сравнения
    // Сверять локальные файлы можно ТОЛЬКО с манифестом своей версии: если манифест новее —
    // у нас легитимно старые файлы (ждут апдейта), сравнение дало бы ложный tamper.
    if (compareSemver(version, QStringLiteral(APP_VERSION)) != 0)
        return;
    runIntegrity(version, files);
}

bool Hardening::verifyManifestSig(const QString &version, const QString &minSupported, bool mandatory,
                                  const QVector<FileEntry> &files, const QString &sigB64) const
{
    // Канон ДОЛЖЕН совпадать с release-v2.sh и Updater::verifyManifestSig.
    QVector<FileEntry> sorted = files;
    std::sort(sorted.begin(), sorted.end(),
              [](const FileEntry &a, const FileEntry &b) { return a.path < b.path; });
    QByteArray canon;
    canon += version.toUtf8() + "\n";
    canon += minSupported.toUtf8() + "\n";
    canon += (mandatory ? "1" : "0");
    canon += "\n";
    for (const FileEntry &f : sorted) {
        canon += f.path.toUtf8() + "\n";
        canon += QByteArray::number(f.size) + "\n";
        canon += f.sha256.toUtf8() + "\n";
    }
    const QByteArray digest = QCryptographicHash::hash(canon, QCryptographicHash::Sha256);
    return ed25519Verify(digest, QByteArray::fromBase64(sigB64.toUtf8()), RPOVERLAY_UPDATE_PUBKEY);
}

void Hardening::runIntegrity(const QString &version, const QVector<FileEntry> &files)
{
    const QString dir = QCoreApplication::applicationDirPath();
    QStringList bad;
    for (const char *crit : kCriticalFiles) {
        const QString rel = QString::fromLatin1(crit);
        // Найти запись в манифесте (нет → этот файл не релизный для данной версии, пропускаем).
        const FileEntry *want = nullptr;
        for (const FileEntry &f : files)
            if (f.path.compare(rel, Qt::CaseInsensitive) == 0) { want = &f; break; }
        if (!want)
            continue;
        const QString local = QDir(dir).filePath(rel);
        const QString got = sha256File(local);
        // Пустой got = файла нет/не читается. Раз он в манифесте нашей версии и критичен — это сигнал.
        if (got != want->sha256)
            bad.push_back(rel);
    }
    integrityChecked_ = true;
    tampered_ = bad;
    integrityOk_ = bad.isEmpty();
    if (!integrityOk_) {
        const QString detail = bad.join(QStringLiteral(", "));
        qWarning().noquote() << "[hardening] integrity-расхождение (" << version << "):" << detail;
        emit anomaly(QStringLiteral("tamper"), detail);
    } else {
        qInfo().noquote() << "[hardening] integrity OK для версии" << version;
    }
}
