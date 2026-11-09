#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QList>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

// resolveProcessId возвращает Win32 PID. Чтобы не тянуть windows.h в заголовок,
// объявляем тип-алиас, совместимый с DWORD (unsigned long). Если windows.h уже
// подключён (этот же тип), идентичный typedef не конфликтует.
#ifndef _WINDEF_
using DWORD = unsigned long;
#endif

// AudioCapture — одновременный WASAPI-захват двух источников:
//   (а) "mic"   — микрофон по умолчанию  (eCapture / eConsole),
//   (б) "other" — звук игры через loopback дефолтного render-устройства
//                 (eRender / eConsole, AUDCLNT_STREAMFLAGS_LOOPBACK).
//
// Каждый источник крутится в отдельном std::thread с циклом GetBuffer/ReleaseBuffer.
// Формат устройства (обычно float32 stereo 44.1/48 кГц) конвертируется в
// МОНО PCM int16 16000 Гц (микс каналов + линейный ресэмпл) и эмитится
// кадрами ~100 мс через сигнал frame().
//
// Использование:
//   auto *cap = new AudioCapture;
//   connect(cap, &AudioCapture::frame, this, &Sink::onFrame);
//   cap->start();  ... cap->stop();
class AudioCapture : public QObject {
    Q_OBJECT
public:
    explicit AudioCapture(QObject *parent = nullptr);
    ~AudioCapture() override;

    AudioCapture(const AudioCapture &) = delete;
    AudioCapture &operator=(const AudioCapture &) = delete;

    // --- Перечисление устройств / источников (вызывать из UI-потока) ---
    struct MicDeviceInfo  { QString id;   QString name; };
    struct AudioProcessInfo { quint32 pid; QString exe; QString title; };

    // Активные устройства захвата (eCapture, DEVICE_STATE_ACTIVE).
    QList<MicDeviceInfo> enumerateMicDevices();
    // Процессы, имеющие аудио-сессию на устройстве воспроизведения по умолчанию.
    QList<AudioProcessInfo> enumerateAudioProcesses();

    // Выбор источников. Применяются при СЛЕДУЮЩЕМ start() (текущий захват не трогают).
    // id  == "" → микрофон по умолчанию; exe == "" → весь системный звук (device-loopback).
    void setMicDeviceId(const QString &id);
    void setGameProcessExe(const QString &exe);

    // Идёт ли сейчас захват.
    bool isRunning() const;

    // Запускает захват (idempotent: повторный start() без stop() — no-op).
    // micOnly=true → поднимаем ТОЛЬКО микрофон (без loopback звука игры) — для Q&A/PTT.
    void start(bool micOnly = false);
    // Останавливает потоки и дожидается их завершения.
    void stop();

signals:
    // pcm16 — сырые сэмплы int16 LE, mono, 16000 Гц (длительностью ~100 мс).
    // source — "mic" или "other".
    void frame(QByteArray pcm16, QString source);
    // Уровень сигнала источника за кадр (~100 мс): пик амплитуды 0.0..1.0 — для VU-индикатора.
    void level(QString source, float peak);
    // Нефатальная диагностика (источник не удалось открыть, поток завершился и т.п.).
    void error(QString message);

private:
    // Целевой формат для сервера /v1/voice/stream.
    static constexpr int kTargetRate = 16000;
    static constexpr int kFrameMs    = 100;

    enum class Mode { Mic, Loopback };

    // Внешний поток источника: крутит captureLoop с ПЕРЕАКВИЗИЦИЕЙ (само-восстановление
    // в духе reconnect-потока OBS — приложение запустилось/зазвучало/перезапустилось).
    void captureThread(Mode mode, QString source);
    // Одна сессия захвата (acquire→capture→teardown). Возвращается при stop / транзиентной
    // ошибке / смене целевого процесса; captureThread её переподнимает.
    void captureLoop(Mode mode, QString source);

    // PID процесса по имени исполняемого файла (basename, без учёта регистра).
    // 0 — процесс не найден среди работающих.
    static DWORD resolveProcessId(const QString &exe);

    std::atomic<bool> running_{false};
    std::thread micThread_;
    std::thread loopThread_;

    // Выбранные источники. Читаются потоками захвата при старте под mutex'ом,
    // но фактически меняются только когда захват остановлен (см. контракт applyAudioSettings).
    mutable std::mutex selMutex_;
    QString micDeviceId_;     // "" → устройство по умолчанию
    QString gameProcessExe_;  // "" → device-loopback (весь системный звук)
};
