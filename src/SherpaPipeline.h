#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Один сегмент attributed-транскрипта: интервал [start,end] в секундах, ID кластера-спикера
// (нумерация диаризации, нестабильна между прогонами — стабилизируется через SpeakerLabels)
// и распознанный текст сегмента.
struct AttributedSegment {
    float start = 0.0f;
    float end = 0.0f;
    int speaker = 0;
    QString text;
};

// Результат прогона пайплайна по одному окну аудио.
struct PipelineResult {
    bool ok = false;
    QString error;
    int numSpeakers = 0;
    QVector<AttributedSegment> segments;
    double processSeconds = 0.0;  // wall-время process() (для диагностики RTF)
    double audioSeconds = 0.0;    // длительность входного окна
    // Тег-поколение задания (копия параметра tag у submit/submitScene): окно атрибутирует
    // результат своему этапу строго по нему — worker может секунды доделывать ПРЕДЫДУЩЕЕ
    // задание, и его запоздавший результат нельзя принять за результат нового.
    quint64 tag = 0;
};

// SherpaPipeline — обёртка над sherpa-onnx: ОДИН долгоживущий OfflineSpeakerDiarization
// (seg3.0 + CAM++) + ОДИН OfflineRecognizer (GigaAM-v3 nemo_transducer int8). Движки создаются
// единожды на собственном worker-потоке (пересоздание диаризатора роняет 0xC0000005 — §8 п.21
// плана) и там же используются/уничтожаются. По submit(окно) worker делает диаризацию → нарезку
// → посегментный STT → attributed-результат и эмитит resultReady() в поток-владелец (queued).
//
// process() тяжёлый (CPU, ~RTF 0.1 диаризация + ~0.03 STT) → НИКОГДА не на UI-потоке. submit()
// коалесцирует: пока worker занят, хранится только ПОСЛЕДНЕЕ окно (хоткей нажали повторно).
class SherpaPipeline : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString modelsDir;         // корень с моделями (подпапки + campplus_zh_en.onnx)
        QString provider = "cpu";  // "cpu" | "cuda" (Фаза 2)
        int numThreads = 4;
        float clusterThreshold = 1.1f;  // как в валидированном прогоне (Фаза 0)
        int numClusters = -1;           // <=0 → кластеризация по threshold
        float minDurationOn = 0.3f;
        float minDurationOff = 0.5f;
        int sampleRate = 16000;
    };

    explicit SherpaPipeline(QObject *parent = nullptr);
    ~SherpaPipeline() override;

    // Стартует worker и АСИНХРОННО создаёт движки (модель-энкодер 208МБ грузится секунды).
    // По завершении эмитит initialized(ok,err). Повторный вызов — no-op.
    void init(const Config &cfg);

    // Движки созданы и готовы принимать submit().
    bool isReady() const { return ready_.load(); }

    // Поставить окно (mono float [-1,1] на cfg.sampleRate) в обработку. Коалесцируется.
    // diarize=false → STT по всему окну без диаризации (один сегмент, speaker=-1) — для Q&A с микрофона.
    // tag — поколение задания (ставит вызывающий, см. OverlayWindow::pipelineGen_): прокидывается
    // в PipelineResult.tag как есть — единственный надёжный способ атрибуции при коалесинге.
    void submit(const std::vector<float> &mono, bool diarize = true, quint64 tag = 0);

    // Сцена с разделением ПО КАНАЛАМ: mic = «Вы» (speaker=-1), other = собеседники (диаризуются →
    // speaker 0,1,…). Оба окна должны быть ВЫРОВНЕНЫ ПО ДЛИНЕ (left-pad тишиной) — общий таймлайн для
    // хронологического слияния. Надёжнее микса: тебя не слепит с собеседником, даже если голоса похожи.
    // tag — как у submit().
    void submitScene(const std::vector<float> &mic, const std::vector<float> &other, quint64 tag = 0);

    // Живая подстройка порога кластеризации диаризации (плана §0.1: настраиваемый порог). Применяется
    // через SherpaOnnxOfflineSpeakerDiarizationSetConfig БЕЗ пересоздания движка (обход 0xC0000005).
    // Ниже порог → больше спикеров; выше → меньше. Потокобезопасно (atomic), применяется на worker.
    void setClusterThreshold(float t);

signals:
    void initialized(bool ok, QString error);
    void resultReady(PipelineResult result);

private:
    void workerLoop();
    PipelineResult runOnce(const std::vector<float> &mono, bool diarize);
    PipelineResult runScene(const std::vector<float> &mic, const std::vector<float> &other);
    void applyThresholdIfChanged();   // worker: если порог сменили — SetConfig на диаризаторе
    bool createEngines(QString *err);
    void destroyEngines();

    Config cfg_;
    std::thread worker_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool started_ = false;
    bool stop_ = false;
    bool hasJob_ = false;
    std::atomic<bool> ready_{false};
    std::atomic<float> wantThreshold_{1.1f};  // желаемый порог кластеризации (ставит UI, любой поток)
    float appliedThreshold_ = 1.1f;           // применённый на диаризаторе (трогает только worker)
    std::vector<float> pending_;       // последнее поставленное окно (mic, коалесинг)
    std::vector<float> pendingOther_;  // второй канал для сцены (звук игры)
    bool pendingDiarize_ = true;       // одиночное окно: с диаризацией или STT-only (Q&A)
    bool pendingScene_ = false;        // тип задания: сцена по каналам (mic+other) vs одиночное окно
    quint64 pendingTag_ = 0;           // тег-поколение поставленного задания → PipelineResult.tag

    struct Engines;   // непрозрачные хендлы sherpa живут только в .cpp
    Engines *eng_ = nullptr;
};

Q_DECLARE_METATYPE(PipelineResult)
