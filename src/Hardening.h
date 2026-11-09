#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Hardening — клиентский харденинг оверлея. ВСЁ report-only: НИКОГДА не крашит и не блокирует
// приложение (ложные срабатывания в AV-песочницах/под отладкой не должны ломать рабочий клиент).
// Главная защита у нас СЕРВЕРНАЯ (вся ценность за JWT-гейтом: совет/RAG/STT; refresh на каждом
// продлении перепроверяет статус ключа + hwid-бан). Этот модуль лишь поднимает планку для
// спуфинга/патчинга и даёт АУДИТ-сигнал, который уезжает на сервер при активации.
//
// Две проверки:
//   1) Анти-дебаг (мгновенно, без сети): IsDebuggerPresent / CheckRemoteDebuggerPresent /
//      PEB.BeingDebugged / PEB.NtGlobalFlag (heap-debug-флаги). Снимок встраивается в тело
//      /v1/license/activate (поле client_integrity) → сервер видит «ключ активировали под отладчиком».
//      Грубый timing-чек НАМЕРЕННО не делаем — он шумный (планировщик/GC дают ложные срабатывания).
//   2) Integrity self-check (по сети): тянет ПОДПИСАННЫЙ manifest-v2 (тот же, что у Updater),
//      и ЕСЛИ его version == APP_VERSION (т.е. мы на актуальной версии — иначе локальные файлы
//      легитимно отличаются от более нового манифеста), хеширует критичный набор своих файлов
//      (exe + RpUpdater + OpenSSL + Qt-сеть/ядро) и сверяет sha256 со списком манифеста →
//      список расхождений (подменённый exe/DLL). Report-only: пишем в debug-лог + сигнал.
class Hardening : public QObject {
    Q_OBJECT
public:
    explicit Hardening(QObject *parent = nullptr);
    ~Hardening() override;

    // Мгновенный снимок анти-дебаг флагов (без сети) — для вставки в тело активации и логов.
    // Статический: вызывается и из LicenseClient без живого экземпляра Hardening.
    static QJsonObject antiDebugSnapshot();

    // Запустить периодический фоновый самоконтроль: первый прогон через ~12с после старта,
    // затем каждые `minutes` минут. Анти-дебаг — сразу; integrity — по сети (активный канал).
    void startPeriodic(int minutes);
    void checkNow();   // разовый прогон (ручной/таймерный)

    bool integrityChecked() const { return integrityChecked_; }
    bool integrityOk() const { return integrityOk_; }
    QStringList tamperedFiles() const { return tampered_; }
    // Полный компактный снимок (анти-дебаг + последний integrity) — для отчёта/лога.
    QJsonObject snapshot() const;

signals:
    // Report-only: kind = "debugger" | "tamper"; detail — человекочитаемая подробность.
    void anomaly(const QString &kind, const QString &detail);

private:
    struct FileEntry { QString path; qint64 size = 0; QString sha256; };

    void fetchManifest();
    void onManifestFinished();
    bool verifyManifestSig(const QString &version, const QString &minSupported, bool mandatory,
                           const QVector<FileEntry> &files, const QString &sigB64) const;
    void runIntegrity(const QString &version, const QVector<FileEntry> &files);
    void reportAntiDebug();   // прогнать анти-дебаг и (если что) эмитить anomaly + лог
    void checkThemidaProtection();  // Слой A: сторожа Themida → anomaly (актив. только в защ. билде)

    static QString sha256File(const QString &path);
    static int compareSemver(const QString &a, const QString &b);

    QNetworkAccessManager *nam_ = nullptr;
    QTimer *periodic_ = nullptr;
    QNetworkReply *reply_ = nullptr;

    bool integrityChecked_ = false;   // был ли валидный прогон integrity (version совпала)
    bool integrityOk_ = true;
    QStringList tampered_;            // относительные пути файлов с несовпавшим sha256
};
