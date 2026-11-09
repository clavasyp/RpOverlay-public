#include "ModelManager.h"
#include "Config.h"
#include "Settings.h"        // settings::modelsPath() — пользовательский каталог моделей
#include "Connectivity.h"    // conn::reportNetworkResult/isTransport — авто-резерв B3 для загрузки модели
#include "update_pubkey.h"   // RPOVERLAY_UPDATE_PUBKEY[32]

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <openssl/evp.h>

#include <algorithm>

namespace {
constexpr int kManifestTimeoutMs = 8000;
constexpr int kDownloadIdleTimeoutMs = 60000;   // нет данных 60с → обрыв (резюм при ретрае)
// Сколько подряд транспортных обрывов текущего файла ретраим (накопление до порога авто-резерва B3
// = 2 → уход на relay, + пара попыток на relay). Без ретрая модель вечно висела бы на «0 из 218»,
// т.к. DPI режет крупное тело CF-канала, а провал сам по себе на relay не переключался.
constexpr int kMaxNetRetries = 6;

// ed25519: msg против sig (64 байта) ключом pub32. (Идентично Updater/UpdateClient.)
bool ed25519Verify(const QByteArray &msg, const QByteArray &sig, const unsigned char *pub32) {
    if (sig.size() != 64) return false;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub32, 32);
    if (!pkey) return false;
    bool ok = false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1)
        ok = EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(sig.constData()),
                              static_cast<size_t>(sig.size()),
                              reinterpret_cast<const unsigned char *>(msg.constData()),
                              static_cast<size_t>(msg.size())) == 1;
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

// Можно ли писать в каталог: создаём его и пробуем временный файл. Папка установки per-user
// (%LocalAppData%\Programs\RpOverlay) обычно записываема; проверка защищает от edge-кейсов
// (ручная установка в Program Files под админом → {app} только для чтения обычному юзеру).
bool dirWritable(const QString &dir) {
    QDir().mkpath(dir);
    QFile probe(QDir(dir).filePath(QStringLiteral(".wtest")));
    if (!probe.open(QIODevice::WriteOnly)) return false;
    probe.close();
    probe.remove();
    return true;
}
}  // namespace

// Встроенный дефолт-список (фолбэк до/без манифеста): STT + диаризация (сценовые режимы
// Адвокат/Прокурор/Общий используют разделение спикеров).
const QVector<ModelManager::ModelFile> &ModelManager::bakedFiles() {
    static const ModelFile kStt[] = {
        {QStringLiteral("sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16/encoder.int8.onnx"),
         224570814, QStringLiteral("0ec54e5f130a91c0f228d21795ede10b31c992a1f73e45dcd827e843a71a9b50")},
        {QStringLiteral("sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16/decoder.onnx"),
         3331651, QStringLiteral("5d1cfef155d8e07f213bd2f3229b5f6c5c29355c7fa5c1b9eb62cb1925725254")},
        {QStringLiteral("sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16/joiner.onnx"),
         1440448, QStringLiteral("fd1d02f45c2ad3d6b67cc149811ad794ab4b020ed49a0a9e2790a8619d1cddd8")},
        {QStringLiteral("sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16/tokens.txt"),
         196, QStringLiteral("17cc514451bcceac9c280068c71502f8448f99e9fb1456b8d0761651fd0392f2")},
    };
    static const QVector<ModelFile> kFiles = [] {
        QVector<ModelFile> v(std::begin(kStt), std::end(kStt));
#ifndef RPOVERLAY_LAWS
        // laws-билд качает ТОЛЬКО STT (голосовой поиск): диаризация нужна лишь сценовым
        // режимам full-билда — 34 МБ юзеру laws-билда ни к чему.
        v.push_back({QStringLiteral("sherpa-onnx-pyannote-segmentation-3-0/model.onnx"),
                     5992913, QStringLiteral("220ad67ca923bef2fa91f2390c786097bf305bceb5e261d4af67b38e938e1079")});
        v.push_back({QStringLiteral("campplus_zh_en.onnx"),
                     28281164, QStringLiteral("aa3cfc16963a10586a9393f5035d6d6b57e98d358b347f80c2a30bf4f00ceba2")});
#endif
        return v;
    }();
    return kFiles;
}

ModelManager::ModelManager(QObject *parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
    idle_ = new QTimer(this);
    idle_->setSingleShot(true);
    connect(idle_, &QTimer::timeout, this, [this]() { if (reply_) reply_->abort(); });
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, [this]() { startNext(); });   // повтор ТОГО ЖЕ файла (dlPos_ не двигаем)
    files_ = bakedFiles();   // дефолт до получения манифеста
    if (localComplete()) status_ = Status::Ready;
}

ModelManager::~ModelManager() { cancel(); }

bool ModelManager::dirHasStt(const QString &dir) {
    for (const ModelFile &f : bakedFiles())
        if (!fileOk(dir, f, /*sha*/false)) return false;
    return true;
}

QString ModelManager::downloadDir() {
    // 1) Явный путь из настроек — абсолютный приоритет (юзер сменил каталог моделей вручную).
    const QString custom = settings::modelsPath();
    if (!custom.isEmpty()) return custom;
    // 2) Если ПОЛНАЯ модель уже лежит в старом каталоге (%LocalAppData%\<Org>\<App>\models) — оставляем
    //    её там: не гоняем ~229 МБ повторно у существующих юзеров при смене дефолта на папку установки.
    const QString legacy = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                               .filePath(QStringLiteral("models"));
    if (dirHasStt(legacy)) return legacy;
    // 3) Дефолт для НОВЫХ загрузок — ПАПКА УСТАНОВКИ (рядом с exe и вшитым silero_vad.onnx).
    //    Установка per-user (%LocalAppData%\Programs\RpOverlay) → каталог записываемый.
    const QString appModels = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models"));
    if (dirWritable(appModels)) return appModels;
    // 4) Фолбэк — старый AppLocalData (если папку установки вдруг нельзя писать, напр. Program Files).
    return legacy;
}

QString ModelManager::localVersionPath() {
    return QDir(downloadDir()).filePath(QStringLiteral("model_version.txt"));
}
QString ModelManager::readLocalVersion() {
    QFile f(localVersionPath());
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}
void ModelManager::writeLocalVersion(const QString &v) {
    QDir().mkpath(downloadDir());
    QFile f(localVersionPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(v.toUtf8());
}

QString ModelManager::modelsDir() {
    // downloadDir() уже резолвит настройку/legacy/папку установки — где искать модель совпадает с тем,
    // куда качаем. Плюс dev-фолбэк (запуск из build-дерева: ../thirdparty/models).
    const QString dl = downloadDir();
    if (dirHasStt(dl)) return dl;
    const QString dev =
        QDir(QCoreApplication::applicationDirPath() + "/../thirdparty/models").absolutePath();
    if (dirHasStt(dev)) return dev;
    return dl;  // целевой каталог скачивания (может быть неполным)
}

qint64 ModelManager::totalBytes() const {
    qint64 t = 0;
    for (const ModelFile &f : files_) t += f.size;
    return t;
}

bool ModelManager::fileOk(const QString &dir, const ModelFile &f, bool checkSha) {
    const QString path = QDir(dir).filePath(f.rel);
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() != f.size) return false;
    if (!checkSha) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&file)) return false;
    return h.result().toHex() == f.sha256.toLatin1();
}

bool ModelManager::localComplete() const {
    const QString dir = modelsDir();
    for (const ModelFile &f : files_)
        if (!fileOk(dir, f, /*sha*/false)) return false;
    return !files_.isEmpty();
}

QString ModelManager::baseUrl() const {
    QString b = cfg().apiBaseUrl;
    while (b.endsWith('/')) b.chop(1);
    return b + QStringLiteral("/updates/models/");
}

QString ModelManager::manifestUrl() const {
    QString b = cfg().apiBaseUrl;
    while (b.endsWith('/')) b.chop(1);
#ifdef RPOVERLAY_LAWS
    // laws-билд: свой STT-only манифест (в полном models-manifest.json лежит и диаризация —
    // она бы скачалась зря). Генерится gen-models-manifest.sh при релизе laws-канала.
    return b + QStringLiteral("/updates/models-laws-manifest.json");
#else
    // С 0.3.0 канал моделей один: models-manifest.json (полный: STT + диаризация).
    // models-qa-manifest.json принадлежит замороженному QA-каналу 0.2.20 — НЕ трогаем.
    return b + QStringLiteral("/updates/models-manifest.json");
#endif
}

void ModelManager::setStatus(Status s, const QString &msg) {
    status_ = s;
    emit statusChanged(static_cast<int>(s), msg);
}

// ---------------- launch gate: manifest → delta → blocking/background ----------------
void ModelManager::prepareForLaunch() {
    // Зовётся на КАЖДЫЙ authorized, включая тихий refresh токена. Гасим отложенный ретрай: иначе в
    // retry-паузе (retry_ ждёт, reply_==nullptr) fetchManifest ниже займёт reply_ манифест-запросом, а
    // проснувшийся retry_→startNext перетёр бы reply_ файловым GET → onManifestFetched заберёт чужой
    // reply как манифест, planDelta сбросит toDownload_/dlPos_ под живой загрузкой → порча состояния.
    if (retry_) retry_->stop();
    // Если загрузка реально В ПОЛЁТЕ (reply_ занят) — тихий refresh НЕ трогаем её манифест-запросом
    // (тот же класс перетирания reply_). Активная закачка сама доиграет и эмитит modelsReady.
    if (reply_) return;
    emittedReady_ = false;
    netRetries_ = 0;
    fetchManifest();
}

void ModelManager::fetchManifest() {
    QNetworkRequest req{QUrl(manifestUrl())};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("Cache-Control", "no-cache");
    req.setTransferTimeout(kManifestTimeoutMs);
    reply_ = nam_->get(req);
    connect(reply_, &QNetworkReply::finished, this, &ModelManager::onManifestFetched);
}

void ModelManager::onManifestFetched() {
    QNetworkReply *r = reply_;
    reply_ = nullptr;
    bool gotManifest = false;
    bool switched = false;
    if (r) {
        // Транспортный провал даже мелкого манифеста → в счётчик авто-резерва (при DPI манифест обычно
        // проходит, но учитываем на всякий). Ушли на relay → перезапросим манифест уже с резерва.
        const bool transport = (r->error() != QNetworkReply::NoError)
                               && conn::isTransport(static_cast<int>(r->error()));
        switched = conn::reportNetworkResult(transport);
        if (r->error() == QNetworkReply::NoError) {
            const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
            const QString version = o.value(QStringLiteral("model_version")).toString();
            const QString sig = o.value(QStringLiteral("sig")).toString();
            QVector<ModelFile> mf;
            for (const QJsonValue &v : o.value(QStringLiteral("files")).toArray()) {
                const QJsonObject fo = v.toObject();
                ModelFile e;
                e.rel = fo.value(QStringLiteral("path")).toString();
                e.size = static_cast<qint64>(fo.value(QStringLiteral("size")).toDouble());
                e.sha256 = fo.value(QStringLiteral("sha256")).toString().toLower();
                if (!e.rel.isEmpty()) mf.push_back(e);
            }
            if (!version.isEmpty() && !mf.isEmpty() && !sig.isEmpty()
                && verifyManifestSig(version, mf, sig)) {
                files_ = mf;               // манифест — источник истины (даёт обновления)
                manifestVersion_ = version;
                gotManifest = true;
            }
        }
        r->deleteLater();
    }
    if (switched) { fetchManifest(); return; }   // ушли на relay → перезапросить манифест уже с него
    // Нет/битый/неподписанный манифест → остаёмся на baked-списке (фолбэк оффлайн).
    planDelta();
}

bool ModelManager::verifyManifestSig(const QString &version, const QVector<ModelFile> &files,
                                     const QString &sigB64) const {
    // Канон ДОЛЖЕН совпадать с gen-models-manifest.sh: model_version\n затем по файлам (сорт по path)
    // path\n size\n sha256\n  → sha256(canon) → ed25519.
    QVector<ModelFile> sorted = files;
    std::sort(sorted.begin(), sorted.end(),
              [](const ModelFile &a, const ModelFile &b) { return a.rel < b.rel; });
    QByteArray canon;
    canon += version.toUtf8() + "\n";
    for (const ModelFile &f : sorted) {
        canon += f.rel.toUtf8() + "\n";
        canon += QByteArray::number(f.size) + "\n";
        canon += f.sha256.toUtf8() + "\n";
    }
    const QByteArray digest = QCryptographicHash::hash(canon, QCryptographicHash::Sha256);
    return ed25519Verify(digest, QByteArray::fromBase64(sigB64.toUtf8()), RPOVERLAY_UPDATE_PUBKEY);
}

void ModelManager::planDelta() {
    const QString dir = modelsDir();
    const bool sameVersion = (!manifestVersion_.isEmpty() && manifestVersion_ == readLocalVersion());
    toDownload_.clear();
    dlPos_ = 0;
    doneBytes_ = 0;
    bool anyMissing = false;
    for (int i = 0; i < files_.size(); ++i) {
        const ModelFile &f = files_[i];
        const bool present = fileOk(dir, f, /*sha*/false);   // быстрый чек по размеру
        if (!present) { toDownload_.push_back(i); anyMissing = true; continue; }
        // Файл на месте по размеру. Если версия сменилась — досверим sha (ловим изменённые файлы).
        if (!sameVersion && manifestVersion_.size() && !fileOk(dir, f, /*sha*/true))
            toDownload_.push_back(i);
    }

    if (toDownload_.isEmpty()) {
        if (!manifestVersion_.isEmpty()) writeLocalVersion(manifestVersion_);
        setStatus(Status::Ready, QStringLiteral("Модель готова."));
        if (!emittedReady_) { emittedReady_ = true; emit modelsReady(); }
        return;
    }
    if (anyMissing) {
        // Модели нет — ОБЯЗАТЕЛЬНАЯ загрузка, оверлей ждёт.
        blockingMode_ = true;
        QDir().mkpath(downloadDir());
        cancelling_ = false;
        setStatus(Status::Downloading, QStringLiteral("Скачиваю модель распознавания…"));
        emit blockingDownload(QStringLiteral("Скачиваю модель распознавания…"));
        startNext();
    } else {
        // Модель работоспособна, но манифест новее — отдаём оверлей сразу, обновляемся в ФОНЕ.
        if (!emittedReady_) { emittedReady_ = true; emit modelsReady(); }
        blockingMode_ = false;
        QDir().mkpath(downloadDir());
        cancelling_ = false;
        setStatus(Status::Downloading, QStringLiteral("Обновляю модель в фоне…"));
        startNext();
    }
}

// ---------------- download (delta) ----------------
void ModelManager::startDownload() {
    if (status_ == Status::Downloading) return;
    // Взводим «занят» ДО синхронной пробы канала: conn::chooseChannel крутит ВЛОЖЕННЫЙ QEventLoop до ~6с
    // (main 2.5с + relay 3.5с — ровно DPI-кейс). Пока проба крутится, кнопка настроек оставалась бы
    // активной, второй клик проходил бы гард выше (status_ ещё не Downloading) → ДВА QNetworkReply в один
    // .part → битый файл/SHA. Ставим статус здесь → реентрантный клик отсекается гардом до chooseChannel.
    setStatus(Status::Downloading, QStringLiteral("Проверяю канал связи…"));
    netRetries_ = 0;
    // Ручной повтор: ПЕРЕВЫБРАТЬ канал (auto перепробует main→relay). Иначе кнопка «Повторить» вечно
    // бьёт в тот же зафиксированный мёртвый канал (жалоба юзера). Проба синхронная (~3с) — под кнопкой
    // настроек допустимо; relay-серт уже в дефолтном QSslConfiguration (добавлен в main.cpp при старте).
    setActiveChannel(conn::chooseChannel(settings::connMode()));
    // Ручной (кнопка настроек): пересчитать дельту против текущего files_ и качать.
    blockingMode_ = false;
    QDir().mkpath(downloadDir());
    cancelling_ = false;
    toDownload_.clear();
    dlPos_ = 0;
    doneBytes_ = 0;
    const QString dir = modelsDir();
    for (int i = 0; i < files_.size(); ++i)
        if (!fileOk(dir, files_[i], /*sha*/false)) toDownload_.push_back(i);
    if (toDownload_.isEmpty()) { setStatus(Status::Ready, QStringLiteral("Модели уже на месте.")); return; }
    setStatus(Status::Downloading, QStringLiteral("Начинаю загрузку…"));
    startNext();
}

void ModelManager::startNext() {
    if (cancelling_) return;   // отменили/удаляют во время retry-паузы — retry_ мог уже проснуться; не воскрешаем загрузку
    if (dlPos_ >= toDownload_.size()) {
        if (!manifestVersion_.isEmpty()) writeLocalVersion(manifestVersion_);
        setStatus(Status::Ready, QStringLiteral("Готово."));
        emit progress(1, 1, toDownload_.size(), toDownload_.size());
        if (blockingMode_ && !emittedReady_) { emittedReady_ = true; emit modelsReady(); }
        blockingMode_ = false;
        return;
    }
    const ModelFile &f = files_[toDownload_[dlPos_]];
    const QString dst = QDir(downloadDir()).filePath(f.rel);
    QDir().mkpath(QFileInfo(dst).absolutePath());

    const QString partPath = dst + QStringLiteral(".part");
    // Резюм: если .part уже есть и НЕ длиннее цели — продолжаем с конца (Append), иначе с нуля.
    qint64 have = QFileInfo(partPath).size();
    if (have < 0 || have >= f.size) { QFile::remove(partPath); have = 0; }
    resumeOffset_ = have;
    metaHandled_ = false;

    out_ = new QFile(partPath, this);
    const QIODevice::OpenMode mode = (resumeOffset_ > 0)
        ? (QIODevice::WriteOnly | QIODevice::Append)
        : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!out_->open(mode)) {
        finishWithError(QStringLiteral("Не могу создать файл: %1").arg(out_->fileName()));
        return;
    }
    hash_ = new QCryptographicHash(QCryptographicHash::Sha256);
    if (resumeOffset_ > 0) {
        // Засидить хеш уже записанными байтами .part (продолжаем подсчёт SHA256 с N).
        QFile seed(partPath);
        if (seed.open(QIODevice::ReadOnly) && hash_->addData(&seed) && seed.size() == resumeOffset_) {
            seed.close();
        } else {
            // не смогли дохешировать → безопасно перекачать с нуля
            seed.close();
            out_->close(); delete out_; out_ = nullptr;
            QFile::remove(partPath);
            resumeOffset_ = 0;
            out_ = new QFile(partPath, this);
            if (!out_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                finishWithError(QStringLiteral("Не могу создать файл: %1").arg(out_->fileName()));
                return;
            }
            delete hash_;
            hash_ = new QCryptographicHash(QCryptographicHash::Sha256);
        }
    }

    qint64 total = 0;
    for (int i : toDownload_) total += files_[i].size;
    QNetworkRequest req{QUrl(baseUrl() + f.rel)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (resumeOffset_ > 0) {
        req.setRawHeader("Range", "bytes=" + QByteArray::number(resumeOffset_) + "-");
        const QString etag = readPartEtag(partPath);
        if (!etag.isEmpty())   // If-Range: докачать только если файл на сервере не изменился
            req.setRawHeader("If-Range", etag.toUtf8());
    }
    reply_ = nam_->get(req);
    connect(reply_, &QNetworkReply::metaDataChanged, this, &ModelManager::onMetaData);
    connect(reply_, &QNetworkReply::readyRead, this, &ModelManager::onReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &ModelManager::onFinished);
    connect(reply_, &QNetworkReply::downloadProgress, this, [this, total](qint64 r, qint64) {
        idle_->start(kDownloadIdleTimeoutMs);
        emit progress(doneBytes_ + resumeOffset_ + r, total, dlPos_, toDownload_.size());
    });
    idle_->start(kDownloadIdleTimeoutMs);
}

// Пришли заголовки: решаем, как трактовать тело. 206 — докачка (пишем в Append, хеш уже
// засижен). 200 — сервер отдаёт ВЕСЬ файл с нуля (Range проигнорен / изменился) → урезаем .part и
// сбрасываем хеш. 416 — диапазон недостижим (файл целиком/сменился) → урезаем и перекачиваем с 0.
void ModelManager::onMetaData() {
    if (metaHandled_ || !reply_ || !out_) return;
    metaHandled_ = true;
    const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString partPath = out_->fileName();
    // Сохранить ETag для будущих If-Range-ретраев этого файла.
    const QByteArray etag = reply_->rawHeader("ETag");
    if (!etag.isEmpty()) writePartEtag(partPath, QString::fromLatin1(etag));
    if (code == 206) return;   // докачка — всё уже готово (Append + засиженный хеш)
    if (resumeOffset_ == 0) return;   // запрашивали с нуля — 200 ожидаем, ничего сбрасывать не нужно
    // 200 (или 416, или иной не-206) при запросе с офсетом: начинаем файл заново.
    out_->close();
    if (!out_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finishWithError(QStringLiteral("Не могу перезаписать %1").arg(partPath));
        return;
    }
    if (hash_) hash_->reset();
    resumeOffset_ = 0;
}

void ModelManager::onReadyRead() {
    if (!reply_ || !out_) return;
    if (!metaHandled_) onMetaData();   // гарантируем разбор 206/200/416 ДО записи тела
    if (!out_) return;                 // onMetaData мог завершить с ошибкой
    const QByteArray d = reply_->readAll();
    out_->write(d);
    if (hash_) hash_->addData(d);
}

void ModelManager::onFinished() {
    idle_->stop();
    if (cancelling_) return;
    QNetworkReply *rep = reply_;
    const ModelFile f = files_[toDownload_[dlPos_]];

    if (rep) {
        const int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 416) {
            // Range недостижим: на сервере файл уже целиком/изменился. Урезаем .part и
            // перекачаем с нуля на следующем заходе (НЕ считаем фатальной сетевой ошибкой).
            const QString partPath = QDir(downloadDir()).filePath(f.rel) + QStringLiteral(".part");
            if (out_) { out_->close(); delete out_; out_ = nullptr; }
            if (hash_) { delete hash_; hash_ = nullptr; }
            rep->deleteLater(); reply_ = nullptr;
            QFile::remove(partPath);
            QFile::remove(partEtagPath(partPath));
            startNext();   // dlPos_ не двигаем — тот же файл, теперь с нуля
            return;
        }
    }

    if (!rep || rep->error() != QNetworkReply::NoError) {
        const QString errStr = rep ? rep->errorString() : QStringLiteral("нет ответа");
        const int err = rep ? static_cast<int>(rep->error())
                            : static_cast<int>(QNetworkReply::UnknownNetworkError);
        const bool transport = conn::isTransport(err);
        // Авто-резерв B3: раньше ModelManager был ИСКЛЮЧЁН отсюда → при DPI-резе крупного тела CF-канала
        // (idle-abort / remote-closed после ~16 КБ) модель вечно висела на «0 из 218» и НИКОГДА не
        // уходила на живой relay. Теперь репортим транспортный обрыв: 2 подряд на main+auto → conn
        // переключит на relay (setActiveChannel), и мы повторяем ТОТ ЖЕ файл уже через резерв.
        const bool switched = conn::reportNetworkResult(transport);
        if (rep) { rep->deleteLater(); reply_ = nullptr; }
        if (out_) { out_->close(); delete out_; out_ = nullptr; }   // .part ОСТАВЛЯЕМ → докачка по Range
        if (hash_) { delete hash_; hash_ = nullptr; }
        // Транспортный обрыв → ретраим ТОТ ЖЕ файл (dlPos_ не двигаем; startNext перечитает .part и
        // запросит Range). switched → уже на relay (быстрый повтор); иначе копим провалы до порога.
        // Не-транспортная ошибка или исчерпан бюджет → в Error (кнопка «Повторить» перевыберет канал).
        if (transport && (switched || netRetries_ < kMaxNetRetries)) {
            ++netRetries_;
            setStatus(Status::Downloading, switched
                ? QStringLiteral("Переключаюсь на резервный канал, продолжаю…")
                : QStringLiteral("Сеть нестабильна, повторяю…"));
            retry_->start(switched ? 400 : 4000);
            return;
        }
        finishWithError(QStringLiteral("Сеть: %1").arg(errStr));
        return;
    }
    if (out_) { out_->write(reply_->readAll()); out_->close(); }

    const QString sha = hash_ ? QString::fromLatin1(hash_->result().toHex()) : QString();
    const QString partPath = QDir(downloadDir()).filePath(f.rel) + QStringLiteral(".part");
    const qint64 got = QFileInfo(partPath).size();

    rep->deleteLater();
    reply_ = nullptr;
    delete hash_; hash_ = nullptr;
    delete out_;  out_ = nullptr;

    if (got != f.size || sha != f.sha256) {
        QFile::remove(partPath);
        QFile::remove(partEtagPath(partPath));
        finishWithError(QStringLiteral("Битый файл %1 (size/SHA не сошлись)").arg(f.rel));
        return;
    }
    const QString finalPath = QDir(downloadDir()).filePath(f.rel);
    QFile::remove(finalPath);
    if (!QFile::rename(partPath, finalPath)) {
        finishWithError(QStringLiteral("Не могу переименовать %1").arg(f.rel));
        return;
    }
    QFile::remove(partEtagPath(partPath));
    conn::reportNetworkResult(false);   // файл дошёл → канал жив, обнуляем счётчик авто-резерва
    netRetries_ = 0;                    // ... и локальный бюджет ретраев для следующего файла
    doneBytes_ += f.size;
    ++dlPos_;
    startNext();
}

void ModelManager::finishWithError(const QString &msg) {
    if (idle_) idle_->stop();
    if (reply_) { reply_->deleteLater(); reply_ = nullptr; }
    if (out_) { out_->close(); delete out_; out_ = nullptr; }
    if (hash_) { delete hash_; hash_ = nullptr; }
    // .part НЕ удаляем — следующий ретрай докачает с N (Range). Удаляем .part только при реально
    // битом size/SHA после ПОЛНОГО ответа (см. onFinished) или при 416/смене файла.
    setStatus(Status::Error, msg);
    // В блокирующем режиме гейт сам устроит ретрай (main по statusChanged==Error).
}

void ModelManager::cancel() {
    if (status_ != Status::Downloading && !reply_) return;
    cancelling_ = true;
    if (retry_) retry_->stop();   // погасить отложенный ретрай текущего файла (retry-пауза = status Downloading)
    if (idle_) idle_->stop();
    if (reply_) { reply_->abort(); reply_->deleteLater(); reply_ = nullptr; }
    if (out_) { out_->close(); delete out_; out_ = nullptr; }
    if (hash_) { delete hash_; hash_ = nullptr; }
    setStatus(localComplete() ? Status::Ready : Status::NotReady, QStringLiteral("Отменено."));
}

bool ModelManager::deleteModels() {
    if (retry_) retry_->stop();   // на случай удаления во время retry-паузы (cancel ниже покрывает лишь status==Downloading)
    if (status_ == Status::Downloading) cancel();
    bool ok = true;
    const QString dir = downloadDir();
    for (const ModelFile &f : files_) {
        const QString p = QDir(dir).filePath(f.rel);
        if (QFileInfo::exists(p) && !QFile::remove(p)) ok = false;
        const QString part = p + QStringLiteral(".part");
        QFile::remove(part);
        QFile::remove(partEtagPath(part));
    }
    QFile::remove(localVersionPath());
    setStatus(localComplete() ? Status::Ready : Status::NotReady, QStringLiteral("Удалено."));
    return ok;
}

// ---- ETag-сайдкар для If-Range (резюм переживает рестарт приложения) ----
QString ModelManager::partEtagPath(const QString &partPath) {
    return partPath + QStringLiteral(".etag");
}
QString ModelManager::readPartEtag(const QString &partPath) {
    QFile f(partEtagPath(partPath));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}
void ModelManager::writePartEtag(const QString &partPath, const QString &etag) {
    QFile f(partEtagPath(partPath));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(etag.toUtf8());
}
