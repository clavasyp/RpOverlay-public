#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QFile;
class QCryptographicHash;

// Updater — бесшовное автообновление (Chrome/Discord-стиль), заменяет UpdateClient.
//
// Отличия от старого:
//   • НЕ блокирует старт (фоновая проверка после показа окна + повтор каждые N часов);
//   • per-file delta: качаем только изменившиеся файлы (manifest-v2 со списком {path,size,sha256});
//   • НЕ Inno-установщик: файлы стейджатся, затем отдельный RpUpdater.exe атомарно подменяет их
//     на месте (rename-then-replace + .bak-откат) и перезапускает приложение;
//   • прогресс в UI + баннер «готово к перезапуску».
//
// manifest-v2.json:
//   { "schema":2, "version":"x.y.z", "min_supported":"0.1.0", "mandatory":bool, "notes":"...",
//     "base_path":"/updates/v2/<ver>/", "files":[{"path":"RpOverlay.exe","size":N,"sha256":"hex"},...],
//     "sig":"base64(ed25519 над каноническим дайджестом списка файлов)" }
// Канонический дайджест (совпадает с release-v2.sh): sha256 от UTF-8 строки
//   version+"\n"+min_supported+"\n"+(mandatory?"1":"0")+"\n" + для каждого файла (сортировка по path)
//   path+"\n"+size+"\n"+sha256+"\n". Подпись ed25519 проверяется ключом RPOVERLAY_UPDATE_PUBKEY.
class Updater : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Checking, Downloading, Verifying, ReadyToApply, Error };

    explicit Updater(QObject *parent = nullptr);
    ~Updater() override;

    void checkNow();                 // ручная/таймерная проверка (фоновая, не блокирует UI)
    void startPeriodic(int hours);   // первый чек через ~8с после старта + каждые hours
    bool hasStagedUpdate() const;    // есть готовое к применению обновление
    QString stagedVersion() const;
    // Текущий форс наложен СЕРВЕРОМ (mandatory/min_supported)? false = «обновление при запуске»
    // (любая новая версия применяется блокирующе при старте, но при сбое сети — fail-open).
    bool serverForced() const { return serverForced_; }
    // Применить: записать apply.json, запустить RpUpdater, попросить приложение выйти.
    // (Само завершение приложения делает слот на signal aboutToApply.)
    void applyStaged();
    // На старте: если есть проверенное staged-обновление новее текущего — применить молча.
    void maybeApplyOnLaunch();

signals:
    void stateChanged(State s, const QString &msg);
    void progress(qint64 received, qint64 total, int fileIndex, int fileCount);
    void updateAvailable(const QString &version, const QString &notes, bool mandatory);
    void readyToApply(const QString &version);
    void aboutToApply();             // приложение должно выйти (RpUpdater подменит файлы)
    void errorOccurred(const QString &diag);
    // ФОРС-обновление: версия mandatory ИЛИ текущая < min_supported → старую версию НЕ пускаем,
    // качаем и применяем немедленно (main показывает блокирующий экран, не пускает в приложение).
    void mandatoryUpdate(const QString &version, const QString &notes);
    // Проверка обновлений завершилась и форс НЕ требуется (нет новее / не обязательно / оффлайн) —
    // main может пускать пользователя дальше (вход/активация). Шлётся ровно когда «можно работать».
    void checkConcluded();
    // РАЗМЫКАТЕЛЬ ПЕТЛИ: применение ЭТОЙ версии падало kMaxApplyAttempts раз подряд (своп не встал —
    // напр. AV держит свежий бинарь). Больше НЕ форсим — fail-open в приложение на текущей версии;
    // main показывает уведомление «обновление не установилось: добавьте папку в исключения AV или
    // переустановите». Сбрасывается автоматически при появлении ДРУГОЙ версии или успешном апдейте.
    void updateStuck(const QString &version);

private:
    struct FileEntry { QString path; qint64 size = 0; QString sha256; };

    void fetchManifest();
    void onManifestFinished();
    void planDownloads();            // вычислить дельту (что качать) и стартовать
    void downloadNext();
    void onFileFinished();
    bool verifyManifestSig(const QString &version, const QString &minSupported, bool mandatory,
                           const QVector<FileEntry> &files, const QString &sigB64) const;
    void finish(State s, const QString &msg);
    void writeStateFile(const QString &status);

    static int compareSemver(const QString &a, const QString &b);
    static QString sha256File(const QString &path);
    static QString installDir();
    static QString stageRoot();      // %LOCALAPPDATA%/.../update
    QString stageDir() const;        // stageRoot/staged/<pendingVersion_>

    // --- Счётчик неудачных применений (размыкатель петли автообновления). Файл apply_attempts.txt
    //     в stageRoot: строка версии + строка счётчика. Живёт в Updater (НЕ в RpUpdater): петлю
    //     замыкает форс-проверка манифеста при старте, её и рвём. ---
    static QString applyAttemptsPath();
    static void readApplyAttempts(QString &ver, int &count);
    static void bumpApplyAttempts(const QString &version);   // +1 (или =1 при смене версии)
    static void clearApplyAttempts();
    static constexpr int kMaxApplyAttempts = 3;

    QNetworkAccessManager *nam_ = nullptr;
    QTimer *periodic_ = nullptr;
    QTimer *idle_ = nullptr;         // инактивити-таймаут загрузки

    QNetworkReply *reply_ = nullptr;
    QFile *out_ = nullptr;
    QCryptographicHash *hash_ = nullptr;

    State state_ = State::Idle;
    // данные текущего манифеста
    QString pendingVersion_;
    QString pendingNotes_;
    QString pendingMinSupported_;
    bool    pendingMandatory_ = false;
    QString basePath_;
    QVector<FileEntry> files_;        // полный список файлов релиза
    QVector<int> toDownload_;         // индексы files_, которые надо скачать (дельта)
    int dlPos_ = 0;                   // позиция в toDownload_
    bool stagedReady_ = false;
    bool forced_ = false;             // текущая закачка применяется СРАЗУ (релонч), не на след. запуске
    bool serverForced_ = false;       // форс именно сервером (mandatory/min_supported) — без fail-open
    bool launchCheck_ = true;         // первая (стартовая) проверка: новее → блокирующее обновление
};
