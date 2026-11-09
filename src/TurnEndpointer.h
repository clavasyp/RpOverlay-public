#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// Параметры авто-триггера сцены. Числа разворачиваются из пресета чувствительности
// (Тихо/Норма/Активно) в Settings — см. settings::autoAdviceTuning(). VAD-параметры
// (threshold/minSilence/maxDispute) «запекаются» в детектор при создании; их смена через
// applyConfig() пересоздаёт VAD на worker-потоке. Гейт-параметры применяются на лету.
struct TurnEndpointerConfig {
    QString vadModelPath;                  // путь к silero_vad.onnx; пусто/нет файла → класс неактивен
    int   sampleRate          = 16000;     // ожидаемая частота (AudioCapture уже 16к mono)
    int   windowSize          = 512;       // окно silero (32 мс @16к)
    float vadThreshold        = 0.5f;      // порог вероятности речи silero
    float minSilenceSec       = 0.7f;      // VAD: тишина после речи = конец реплики (vadHangover)
    float minUtteranceSec     = 0.9f;      // ГЕЙТ: мин. длительность реплики, чтобы реагировать
    float maxDisputeSec       = 9.0f;      // VAD max_speech_duration + форс-эндпоинт в споре без пауз
    float voiceMarginDb       = 6.0f;      // адаптивный передний план: речь громче фона на N дБ
    float cooldownSec         = 7.0f;      // пауза между авто-советами
    float minNewSpeechSec     = 1.2f;      // дельта: новой other-речи с прошлого fire, чтобы сработать
    bool  requireAdjacency    = false;     // смежность ОБЯЗАТЕЛЬНА (строгий режим «Тихо»)
    bool  suppressWhileMicHot = true;      // не триггерить, пока сам игрок говорит
    // внутренние (не из пресета):
    float noiseHalfLifeSec    = 3.0f;      // период полураспада rolling noise-floor
    float adjacencyWindowSec  = 8.0f;      // окно недавней mic-активности (разговорная смежность)
    float micActivePeak       = 0.08f;     // порог «игрок сейчас говорит» по пику микрофона
};

// TurnEndpointer — детектор «собеседник договорил» поверх silero-VAD + многоступенчатый локальный
// гейт релевантности. Работает по каналу OTHER (звук игры) в реальном времени на СОБСТВЕННОМ
// worker-потоке (инференс VAD на каждый 512-семпловый кадр нельзя на аудио/UI-потоке). Решает только
// вопрос «КОГДА разбирать сцену» — НЕ право; на сервер не ходит. По проходу гейта эмитит
// fireRequested() (queued → UI), где OverlayWindow прогоняет существующую цепочку submitScene→совет.
//
// Гейт (все условия обязательны): VAD-эндпоинт ИЛИ форс-таймаут спора; речь громче фона (адаптивный
// порог в дБ — устойчив к разным микрофонам); разговорная смежность (диалог чередуется с игроком);
// не подавлено речью игрока; кулдаун + накоплено достаточно новой речи. Опаковый VAD-хендл живёт
// только в .cpp и не пересекает поток. Без HAVE_LOCAL_STT — заглушка (isActive()==false).
class TurnEndpointer : public QObject {
    Q_OBJECT
public:
    explicit TurnEndpointer(QObject *parent = nullptr);
    ~TurnEndpointer() override;

    void start(const TurnEndpointerConfig &cfg);   // старт worker + создание VAD; повтор → applyConfig
    void stop();                                   // stop + join + destroy (идемпотентно)
    bool isActive() const { return active_.load(); } // VAD создан и принимает кадры
    void applyConfig(const TurnEndpointerConfig &cfg); // живая смена профиля (пересоздаёт VAD на worker)

    // Вызываются с UI-потока:
    void pushOther(const QByteArray &pcm16);       // кадр other (int16 mono 16к) → очередь worker'а
    void noteMicLevel(float peak);                 // пик микрофона (активность игрока) для смежности/подавления
    void resetState();                             // сброс гейта (вход в режим / ручной разбор)
    void noteFired();                              // отметить, что разбор только что запущен (кулдаун+дельта)

signals:
    void fireRequested();                          // гейт пройден → разобрать сцену (queued → UI)
    void debugState(QString line);                 // диагностика решений гейта (в overlay-debug.log)

private:
    void workerLoop();
    void applyPendingCommands();   // worker: подхватить reset/fired/cfgDirty
    void snapshotConfig();         // worker: cfg_ → wcfg_ под cfgM_
    bool createVad(QString *err);  // worker: создать silero-VAD из wcfg_
    void destroyVad();             // worker: уничтожить VAD-хендл
    void resetGateState();         // worker: обнулить состояние гейта + VAD Reset/Clear
    void processWindow(const float *p, int n);  // worker: VAD-инференс + метрики на одном окне
    bool evaluateGate(double turnDurSec);       // worker: объединить все условия гейта
    void doFire(const char *reason);            // worker: emit + старт кулдауна
    long long nowMs() const;                    // wall-clock мс от epoch_ (steady_clock)

    std::thread worker_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<float> queue_;       // other-семплы (под m_)
    bool stop_ = false;

    std::atomic<bool> started_{false};
    std::atomic<bool> active_{false};

    std::mutex cfgM_;
    TurnEndpointerConfig cfg_;       // под cfgM_ (пишет UI)
    TurnEndpointerConfig wcfg_;      // снимок для worker (трогает только worker)
    std::atomic<bool> cfgDirty_{false};
    std::atomic<float> micActivePeakA_{0.08f};  // зеркало cfg.micActivePeak для lock-free mic-пути

    std::atomic<long long> lastMicActiveMs_{-1000000};  // wall-clock мс последней активности микрофона
    std::atomic<int> resetReq_{0};
    std::atomic<int> firedReq_{0};
    int resetSeen_ = 0;
    int firedSeen_ = 0;

    std::chrono::steady_clock::time_point epoch_;

    // Состояние гейта (трогает ТОЛЬКО worker):
    double noiseFloorRms_ = 1e-4;     // rolling уровень фона (EMA по не-речевым кадрам)
    double curSpeechSumRms_ = 0.0;    // сумма RMS речевых кадров текущей реплики
    int    curSpeechFrames_ = 0;
    bool   inSpeech_ = false;
    double processedMs_ = 0.0;        // «модельное» время (по числу прогнанных семплов)
    double speechStartMs_ = 0.0;      // модельное время начала текущей реплики
    double lastVoiceMs_ = 0.0;        // модельное время последнего речевого кадра
    double accumNewSpeechSec_ = 0.0;  // новой other-речи с прошлого fire (для дельты)
    double lastFireProcMs_ = -1e9;    // модельное время прошлого fire (для кулдауна)
    long long speechStartWallMs_ = 0; // wall-время начала реплики (для смежности с микрофоном)

    struct Vad;                       // опаковый sherpa-хендл — только в .cpp
    Vad *vad_ = nullptr;
};
