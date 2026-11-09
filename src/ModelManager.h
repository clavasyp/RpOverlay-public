#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QFile;
class QCryptographicHash;

// ModelManager — локальные модели оффлайн-движка: STT (голосовой вопрос) + диаризация
// (разделение спикеров в сценовых режимах Адвокат/Прокурор/Общий).
//
// Список файлов и хеши берутся с СЕРВЕРНОГО подписанного модель-манифеста (ed25519):
//   /updates/models-manifest.json (models-qa-manifest.json — замороженный QA-канал, не наш)
// Форма: { "model_version":"N", "files":[{"path","size","sha256"}], "sig":"base64" }.
// → обновил модель = бампнул манифест на сервере, клиент сам перекачает изменившиеся файлы (дельта по
// sha), без пересборки клиента. До/без манифеста используется встроенный дефолт-список (фолбэк оффлайн).
//
// Файлы качаются с АКТИВНОГО канала (relay-aware), атомарно (.part → rename), проверяются по
// размеру+SHA256. model_version хранится локально (детект обновления). Каталог по умолчанию —
// ПАПКА УСТАНОВКИ ({app}\models, рядом с exe; установка per-user → записываема); юзер может сменить
// его в настройках (settings::modelsPath). Существующая модель в старом %LOCALAPPDATA%\...\models
// подхватывается на месте (без повторной ~229 МБ докачки). Детали резолва — в downloadDir().
class ModelManager : public QObject {
    Q_OBJECT
public:
    enum class Status { NotReady, Downloading, Ready, Error };

    explicit ModelManager(QObject *parent = nullptr);
    ~ModelManager() override;

    static QString modelsDir();          // где ИСКАТЬ модели: download-каталог / dev thirdparty
    static QString downloadDir();        // куда КАЧАТЬ: настройка → существующая legacy-модель → папка установки
    qint64 totalBytes() const;           // суммарный размер файлов активного списка
    bool   localComplete() const;        // все файлы активного списка на месте (по размеру) в modelsDir
    bool   isReady() const { return localComplete(); }
    Status status() const { return status_; }

    // Модель-гейт при запуске: тянет манифест, верифицирует, считает дельту и решает:
    //   модели НЕТ → emit blockingDownload(msg) + качаю ОБЯЗАТЕЛЬНО (оверлей ждёт) → по концу modelsReady();
    //   модель есть, но манифест НОВЕЕ → emit modelsReady() сразу + качаю обновление в ФОНЕ (след. запуск);
    //   модель есть и актуальна / оффлайн с готовой моделью → emit modelsReady() сразу.
    void prepareForLaunch();

    void startDownload();                // ручной (кнопка настроек full-сборки): докачать недостающее
    void cancel();
    bool deleteModels();

signals:
    void statusChanged(int status, const QString &message);     // Status как int (для string-connect)
    void progress(qint64 received, qint64 total, int fileIndex, int fileCount);
    void modelsReady();                                          // модель готова — можно открыть оверлей
    void blockingDownload(const QString &message);               // моделей нет → обязательная загрузка пошла

private:
    struct ModelFile { QString rel; qint64 size; QString sha256; };
    static const QVector<ModelFile> &bakedFiles();   // встроенный дефолт (вариант-зависим: QA → только STT)

    QString manifestUrl() const;         // активный канал + models[-qa]-manifest.json
    QString baseUrl() const;             // активный канал + /updates/models/
    void fetchManifest();
    void onManifestFetched();
    bool verifyManifestSig(const QString &version, const QVector<ModelFile> &files,
                           const QString &sigB64) const;
    void planDelta();                    // заполнить toDownload_ (нет/sha не сошёлся) против modelsDir
    static bool fileOk(const QString &dir, const ModelFile &f, bool checkSha);
    static bool dirHasStt(const QString &dir);   // полный STT-набор (по размеру) лежит в каталоге

    void startNext();
    void onMetaData();    // пришли заголовки: резолвим 206(докачка)/200(с нуля)/416(целиком)
    void onReadyRead();
    void onFinished();
    void finishWithError(const QString &msg);
    static QString partEtagPath(const QString &partPath);   // .etag-сайдкар для If-Range
    static QString readPartEtag(const QString &partPath);
    static void    writePartEtag(const QString &partPath, const QString &etag);
    void setStatus(Status s, const QString &msg = QString());
    static QString localVersionPath();
    static QString readLocalVersion();
    static void writeLocalVersion(const QString &v);

    QNetworkAccessManager *nam_ = nullptr;
    QTimer *idle_ = nullptr;             // инактивити-таймаут загрузки файла (по образцу Updater)
    QNetworkReply *reply_ = nullptr;
    QFile *out_ = nullptr;
    QCryptographicHash *hash_ = nullptr;
    qint64 resumeOffset_ = 0;            // сколько байт уже было в .part на старте текущего чанка
    bool metaHandled_ = false;           // заголовки текущего ответа уже разобраны (206/200/416)

    QVector<ModelFile> files_;           // активный список файлов (манифест ИЛИ дефолт)
    QString manifestVersion_;            // версия из манифеста ("" если ещё не получен)
    QVector<int> toDownload_;            // индексы files_ к скачиванию (дельта)
    int dlPos_ = 0;                      // позиция в toDownload_
    qint64 doneBytes_ = 0;
    Status status_ = Status::NotReady;
    bool cancelling_ = false;
    bool blockingMode_ = false;          // текущая загрузка обязательная (гейт ждёт modelsReady)
    bool emittedReady_ = false;          // modelsReady() уже отправлен в этом prepareForLaunch
    QTimer *retry_ = nullptr;            // ретрай текущего файла при транспортном обрыве (авто-резерв B3)
    int netRetries_ = 0;                 // подряд сетевых ретраев текущей сессии загрузки (кап kMaxNetRetries)
};
