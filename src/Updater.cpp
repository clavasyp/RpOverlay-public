#include "Updater.h"
#include "Config.h"
#include "Settings.h"
#include "update_pubkey.h"   // RPOVERLAY_UPDATE_PUBKEY[32]

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QProcess>
#include <QUrl>
#include <QDebug>

#include <openssl/evp.h>

#include <algorithm>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace {
constexpr int kManifestTimeoutMs = 8000;
constexpr int kDownloadIdleTimeoutMs = 60000;
constexpr int kFirstCheckDelayMs = 8000;

// ed25519: msg (сырые байты) против sig (64 байта) ключом pub32. (Идентично UpdateClient.)
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

Updater::Updater(QObject *parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
    , periodic_(new QTimer(this))
    , idle_(new QTimer(this))
{
    idle_->setSingleShot(true);
    connect(idle_, &QTimer::timeout, this, [this]() { if (reply_) reply_->abort(); });
    connect(periodic_, &QTimer::timeout, this, &Updater::checkNow);
}

Updater::~Updater() = default;

int Updater::compareSemver(const QString &a, const QString &b)
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

QString Updater::installDir() { return QCoreApplication::applicationDirPath(); }

QString Updater::stageRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/update");
}
QString Updater::stageDir() const { return stageRoot() + QStringLiteral("/staged/") + pendingVersion_; }

QString Updater::sha256File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) { f.close(); return QString(); }
    f.close();
    return QString::fromLatin1(h.result().toHex());
}

// --- Счётчик неудачных применений (размыкатель петли автообновления) ---
QString Updater::applyAttemptsPath() { return stageRoot() + QStringLiteral("/apply_attempts.txt"); }

void Updater::readApplyAttempts(QString &ver, int &count)
{
    ver.clear();
    count = 0;
    QFile f(applyAttemptsPath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    if (lines.size() >= 2) {
        ver = lines.at(0).trimmed();
        count = lines.at(1).trimmed().toInt();
    }
}

void Updater::bumpApplyAttempts(const QString &version)
{
    QString ver;
    int count = 0;
    readApplyAttempts(ver, count);
    if (ver != version)
        count = 0;   // новая версия — счётчик с нуля
    ++count;
    QDir().mkpath(stageRoot());
    QFile f(applyAttemptsPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write((version + QLatin1Char('\n') + QString::number(count) + QLatin1Char('\n')).toUtf8());
}

void Updater::clearApplyAttempts() { QFile::remove(applyAttemptsPath()); }

void Updater::startPeriodic(int hours)
{
    QTimer::singleShot(kFirstCheckDelayMs, this, &Updater::checkNow);
    periodic_->start(qMax(1, hours) * 3600 * 1000);
}

bool Updater::hasStagedUpdate() const { return stagedReady_; }
QString Updater::stagedVersion() const { return pendingVersion_; }

void Updater::finish(State s, const QString &msg)
{
    state_ = s;
    emit stateChanged(s, msg);
    if (s == State::Error && !msg.isEmpty())
        emit errorOccurred(msg);
}

// ---------------- check / manifest ----------------
void Updater::checkNow()
{
    if (state_ == State::Checking || state_ == State::Downloading)
        return;  // single-flight
    fetchManifest();
}

void Updater::fetchManifest()
{
    finish(State::Checking, QStringLiteral("Проверка обновлений…"));
    // Канал обновлений СТРОГО по варианту сборки (как в Hardening::fetchManifest):
    // QA-билд (RPOVERLAY_QA) — manifest-v2-qa.json, тестерский full — manifest-v2.json.
    // ⚠️ Инцидент 2026-07-02: оба варианта читали manifest-v2.json (легаси заморозки
    // QA-канала на 0.2.20). Пока версии каналов совпадали — баг спал; после релиза
    // full 0.4.0 каждый QA 0.3.3 при запуске блокирующе перекачивал себя ПОЛНОЙ
    // сборкой → variant=full → все не-тестеры ловили tester_only при активации.
    // ⚠️ Полный канал с 0.4.1 переехал на manifest-v2-full.json: старый путь
    // /updates/manifest-v2.json НАВСЕГДА оставлен «лечилкой» для QA-клиентов ≤0.3.3
    // с багом канала (там лежит QA-контент — они самоизлечаются при любом запуске,
    // хоть через месяцы). НЕ возвращать full-контент на старый путь!
    // laws-билд — СВОЙ канал manifest-v2-laws.json (проверка ПЕРВОЙ: laws определяет и RPOVERLAY_QA).
    // Урок инцидента выше: каждый вариант читает ТОЛЬКО свой манифест, иначе самообновится в чужой билд.
#if defined(RPOVERLAY_LAWS)
    const QString kManifestPath = QStringLiteral("/updates/manifest-v2-laws.json");
#elif defined(RPOVERLAY_QA)
    const QString kManifestPath = QStringLiteral("/updates/manifest-v2-qa.json");
#else
    const QString kManifestPath = QStringLiteral("/updates/manifest-v2-full.json");
#endif
    QNetworkRequest req{QUrl(cfg().apiBaseUrl + kManifestPath)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("Cache-Control", "no-cache");
    req.setTransferTimeout(kManifestTimeoutMs);
    reply_ = nam_->get(req);
    connect(reply_, &QNetworkReply::finished, this, &Updater::onManifestFinished);
}

void Updater::onManifestFinished()
{
    QNetworkReply *r = reply_;
    reply_ = nullptr;
    if (!r) return;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) {
        finish(State::Idle, QString());  // нет связи — тихо, остаёмся на текущей версии
        emit checkConcluded();           // не смогли проверить → fail-open: пускаем (не локаем оффлайн)
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
    pendingVersion_      = o.value(QStringLiteral("version")).toString();
    pendingNotes_        = o.value(QStringLiteral("notes")).toString();
    pendingMinSupported_ = o.value(QStringLiteral("min_supported")).toString();
    pendingMandatory_    = o.value(QStringLiteral("mandatory")).toBool(false);
    basePath_            = o.value(QStringLiteral("base_path")).toString();
    const QString sig    = o.value(QStringLiteral("sig")).toString();

    files_.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("files")).toArray()) {
        const QJsonObject fo = v.toObject();
        FileEntry e;
        e.path   = fo.value(QStringLiteral("path")).toString();
        e.size   = static_cast<qint64>(fo.value(QStringLiteral("size")).toDouble());
        e.sha256 = fo.value(QStringLiteral("sha256")).toString().toLower();
        if (!e.path.isEmpty())
            files_.push_back(e);
    }
    if (pendingVersion_.isEmpty() || basePath_.isEmpty() || files_.isEmpty() || sig.isEmpty()) {
        finish(State::Idle, QString());
        emit checkConcluded();
        return;
    }
    if (!verifyManifestSig(pendingVersion_, pendingMinSupported_, pendingMandatory_, files_, sig)) {
        finish(State::Error, QStringLiteral("Подпись манифеста не прошла — обновление отклонено."));
        emit checkConcluded();   // битый/чужой манифест → НЕ форсим (fail-open), пускаем
        return;
    }
    const bool launch = launchCheck_;
    launchCheck_ = false;   // последующие проверки (периодические/ретраи) — фоновые
    if (compareSemver(pendingVersion_, QStringLiteral(APP_VERSION)) <= 0) {
        clearApplyAttempts();            // мы уже на этой (или новее) версии → прошлые неудачи неактуальны
        finish(State::Idle, QString());  // не новее
        emit checkConcluded();
        return;
    }
    // РАЗМЫКАТЕЛЬ ПЕТЛИ АВТООБНОВЛЕНИЯ: если применение ЭТОЙ версии уже падало kMaxApplyAttempts раз
    // подряд (своп не встал — напр. AV держит свежескачанный бинарь дольше бюджета RpUpdater), больше
    // НЕ форсим — иначе бесконечный цикл «качаю → выключаюсь → снова качаю» (инцидент 2026-07-19).
    // Fail-open в приложение на текущей версии + updateStuck (main уведомит). Счётчик ведёт applyStaged;
    // при появлении ДРУГОЙ версии в манифесте — сбрасываем (новую пробуем заново).
    {
        QString atVer;
        int atCount = 0;
        readApplyAttempts(atVer, atCount);
        if (atVer != pendingVersion_) {
            clearApplyAttempts();
        } else if (atCount >= kMaxApplyAttempts) {
            finish(State::Idle, QString());
            emit updateStuck(pendingVersion_);
            emit checkConcluded();
            return;
        }
    }
    // СЕРВЕРНЫЙ форс: mandatory ИЛИ текущая ниже min_supported → старую версию НЕ пускаем вообще
    // (ретраи до победного). ОБНОВЛЕНИЕ ПРИ ЗАПУСКЕ: любая новая версия на стартовой проверке
    // качается СРАЗУ блокирующе (прогресс на сплэше) → применяется → запускается уже новая
    // («запускаю → качает с прогрессом → применяет → стартует новая»). Отличие от серверного
    // форса: при сбое сети fail-open (пускаем на текущей, докачаем фоном). Выключенный тумблер
    // автообновления уважаем — тогда блокирующего апдейта при запуске нет (только серверный форс).
    serverForced_ = pendingMandatory_
              || compareSemver(QStringLiteral(APP_VERSION), pendingMinSupported_) < 0;
    forced_ = serverForced_ || (launch && settings::autoUpdateEnabled());
    if (forced_) {
        emit mandatoryUpdate(pendingVersion_, pendingNotes_);
        planDownloads();   // независимо от autoUpdateEnabled; на readyToApply → авто-применение (forced_)
        return;
    }
    // Фоновая проверка (4-часовая) или автообновление выключено — пользователя не дёргаем.
    emit checkConcluded();
    if (!settings::autoUpdateEnabled()) {
        emit updateAvailable(pendingVersion_, pendingNotes_, pendingMandatory_);
        finish(State::Idle, QStringLiteral("Доступно обновление %1").arg(pendingVersion_));
        return;
    }
    planDownloads();
}

bool Updater::verifyManifestSig(const QString &version, const QString &minSupported, bool mandatory,
                                const QVector<FileEntry> &files, const QString &sigB64) const
{
    // Канон ДОЛЖЕН совпадать с release-v2.sh.
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

// ---------------- download (delta) ----------------
void Updater::planDownloads()
{
    toDownload_.clear();
    dlPos_ = 0;
    for (int i = 0; i < files_.size(); ++i) {
        const QString local = QDir(installDir()).filePath(files_[i].path);
        if (sha256File(local) != files_[i].sha256)   // нет/отличается → качаем
            toDownload_.push_back(i);
    }
    emit updateAvailable(pendingVersion_, pendingNotes_, pendingMandatory_);
    // РЕЗЮМ: stage ТЕКУЩЕЙ версии НЕ сносим — уже скачанные валидные файлы переиспользуем (downloadNext
    // пропускает их по sha). Раньше removeRecursively выбрасывал их каждый цикл → на relay (2.5 мин/файл)
    // полная перекачка при каждой неудаче свопа. Разные версии живут в разных stageDir → путаницы нет.
    // Но stage ДРУГИХ версий чистим (иначе копится: removeRecursively убран, а rmTree в RpUpdater no-op).
    {
        QDir stagedRoot(stageRoot() + QStringLiteral("/staged"));
        const QStringList olds = stagedRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &d : olds)
            if (d != pendingVersion_)
                QDir(stagedRoot.filePath(d)).removeRecursively();
    }
    QDir().mkpath(stageDir());
    finish(State::Downloading, QStringLiteral("Загрузка обновления %1…").arg(pendingVersion_));
    downloadNext();
}

void Updater::downloadNext()
{
    if (dlPos_ >= toDownload_.size()) {
        // Всё скачано (или дельта пустая) — копируем неизменённые? Нет: RpUpdater подменит только
        // staged-файлы; неизменённые остаются на месте. Ставим ready.
        finish(State::Verifying, QStringLiteral("Проверка файлов…"));
        stagedReady_ = true;
        writeStateFile(QStringLiteral("ready"));
        finish(State::ReadyToApply, QStringLiteral("Обновление %1 готово.").arg(pendingVersion_));
        emit readyToApply(pendingVersion_);
        if (forced_)
            applyStaged();   // форс — применяем немедленно (приложение выйдет → RpUpdater → рестарт)
        return;
    }
    const FileEntry &f = files_[toDownload_[dlPos_]];
    const QString dst = QDir(stageDir()).filePath(f.path);
    QDir().mkpath(QFileInfo(dst).absolutePath());

    // РЕЗЮМ: файл этой версии уже скачан и валиден в stage (прошлый цикл) → не качаем повторно.
    if (sha256File(dst) == f.sha256) {
        ++dlPos_;
        downloadNext();
        return;
    }

    out_ = new QFile(dst + QStringLiteral(".part"), this);
    if (!out_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finish(State::Error, QStringLiteral("Не могу создать %1").arg(out_->fileName()));
        return;
    }
    hash_ = new QCryptographicHash(QCryptographicHash::Sha256);

    QNetworkRequest req{QUrl(cfg().apiBaseUrl + basePath_ + f.path)};  // активный канал (main/relay)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    reply_ = nam_->get(req);
    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (reply_ && out_) { const QByteArray d = reply_->readAll(); out_->write(d); if (hash_) hash_->addData(d); }
    });
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 rcv, qint64) {
        idle_->start(kDownloadIdleTimeoutMs);
        emit progress(rcv, files_[toDownload_[dlPos_]].size, dlPos_, toDownload_.size());
    });
    connect(reply_, &QNetworkReply::finished, this, &Updater::onFileFinished);
    idle_->start(kDownloadIdleTimeoutMs);
}

void Updater::onFileFinished()
{
    idle_->stop();
    QNetworkReply *r = reply_;
    reply_ = nullptr;
    const FileEntry f = files_[toDownload_[dlPos_]];
    const QString partPath = QDir(stageDir()).filePath(f.path) + QStringLiteral(".part");

    if (!r || r->error() != QNetworkReply::NoError) {
        if (r) r->deleteLater();
        if (out_) { out_->close(); delete out_; out_ = nullptr; }
        if (hash_) { delete hash_; hash_ = nullptr; }
        QFile::remove(partPath);
        finish(State::Error, QStringLiteral("Сеть при загрузке %1").arg(f.path));
        return;
    }
    if (out_) { out_->write(r->readAll()); out_->close(); }
    const QString sha = hash_ ? QString::fromLatin1(hash_->result().toHex()) : QString();
    const qint64 got = QFileInfo(partPath).size();
    r->deleteLater();
    delete hash_; hash_ = nullptr;
    delete out_;  out_ = nullptr;

    if (got != f.size || sha != f.sha256) {
        QFile::remove(partPath);
        finish(State::Error, QStringLiteral("Битый файл %1 (size/sha)").arg(f.path));
        return;
    }
    const QString finalPath = QDir(stageDir()).filePath(f.path);
    QFile::remove(finalPath);
    if (!QFile::rename(partPath, finalPath)) {
        finish(State::Error, QStringLiteral("Не переименовать %1").arg(f.path));
        return;
    }
    ++dlPos_;
    downloadNext();
}

// ---------------- apply ----------------
void Updater::writeStateFile(const QString &status)
{
    QJsonObject st{
        {QStringLiteral("stagedVersion"), pendingVersion_},
        {QStringLiteral("status"), status},
    };
    QDir().mkpath(stageRoot());
    QFile f(stageRoot() + QStringLiteral("/state.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(st).toJson(QJsonDocument::Compact));
        f.close();
    }
}

void Updater::applyStaged()
{
    if (pendingVersion_.isEmpty() || !QDir(stageDir()).exists())
        return;

    // РАЗМЫКАТЕЛЬ в ЕДИНОЙ точке входа ОБОИХ путей (manifest-форс И maybeApplyOnLaunch). Путь launch
    // минует onManifestFinished, поэтому гейт нужен и здесь: если применение ЭТОЙ версии уже падало
    // kMaxApplyAttempts раз — НЕ применяем снова. Иначе при выжившем state.json (RpUpdater убит AV/
    // ребутом до чистки) maybeApplyOnLaunch молча спавнил бы RpUpdater каждый старт = «не запускается».
    // Чистим state.json (чтобы maybeApplyOnLaunch перестал перезапускать петлю) + уведомляем; fail-open
    // (applyingLaunch останется false → main пойдёт обычным стартом на текущей версии).
    {
        QString atVer;
        int atCount = 0;
        readApplyAttempts(atVer, atCount);
        if (atVer == pendingVersion_ && atCount >= kMaxApplyAttempts) {
            QFile::remove(stageRoot() + QStringLiteral("/state.json"));
            finish(State::Idle, QString());
            emit updateStuck(pendingVersion_);
            return;
        }
    }
    // Учитываем ПОПЫТКУ применения СРАЗУ — ДО записи apply.txt / запуска RpUpdater: даже если ниже
    // что-то упадёт (нет apply.txt / нет RpUpdater.exe — напр. закарантинен AV / startDetached), счётчик
    // растёт → и mandatory-ретрай (main), и launch-путь упрутся в размыкатель. При успехе применения
    // APP_VERSION вырастет → onManifestFinished (ветка «не новее») сбросит счётчик.
    bumpApplyAttempts(pendingVersion_);

    // apply.txt — простой key=value (UTF-8), чтобы RpUpdater.exe парсил БЕЗ Qt (иначе он держал бы
    // Qt6Core.dll открытым и не смог бы его подменить при апдейте Qt).
    const QString applyPath = stageRoot() + QStringLiteral("/apply.txt");
    QByteArray body;
    body += "installDir=" + QDir::toNativeSeparators(installDir()).toUtf8() + "\n";
    body += "stagedDir="  + QDir::toNativeSeparators(stageDir()).toUtf8() + "\n";
    body += "mainExe="    + QDir::toNativeSeparators(QDir(installDir()).filePath(QStringLiteral("RpOverlay.exe"))).toUtf8() + "\n";
    body += "mainPid="    + QByteArray::number(static_cast<qlonglong>(QCoreApplication::applicationPid())) + "\n";
    body += "relaunch=1\n";
    QFile f(applyPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finish(State::Error, QStringLiteral("Не записать apply.txt"));
        return;
    }
    f.write(body);
    f.close();

    const QString updater = QDir(installDir()).filePath(QStringLiteral("RpUpdater.exe"));
    if (!QFile::exists(updater)) {
        finish(State::Error, QStringLiteral("RpUpdater.exe не найден рядом с приложением"));
        return;
    }
    if (!QProcess::startDetached(updater, QStringList{QStringLiteral("--apply"), applyPath})) {
        finish(State::Error, QStringLiteral("Не запустить RpUpdater.exe"));
        return;
    }
    emit aboutToApply();   // главный код выходит → RpUpdater подменит файлы и перезапустит (bump — выше)
}

void Updater::maybeApplyOnLaunch()
{
    QFile f(stageRoot() + QStringLiteral("/state.json"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject st = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QString sv = st.value(QStringLiteral("stagedVersion")).toString();
    if (st.value(QStringLiteral("status")).toString() != QLatin1String("ready") || sv.isEmpty())
        return;
    if (compareSemver(sv, QStringLiteral(APP_VERSION)) <= 0) {
        // staged неактуален (уже стоит ≥) — подчистим.
        QDir(stageRoot() + QStringLiteral("/staged/") + sv).removeRecursively();
        QFile::remove(stageRoot() + QStringLiteral("/state.json"));
        return;
    }
    pendingVersion_ = sv;
    stagedReady_ = true;
    if (QDir(stageDir()).exists())
        applyStaged();   // тихое применение готового обновления на старте (Discord-стиль)
}
