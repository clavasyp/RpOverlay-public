#include "SherpaPipeline.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>

#ifdef HAVE_LOCAL_STT
#include "sherpa-onnx/c-api/c-api.h"
#ifdef _WIN32
// NOMINMAX — иначе windows.h определит макросы min/max и сломает std::max в runScene.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // VEH: AddVectoredExceptionHandler, EXCEPTION_ACCESS_VIOLATION
#include <csetjmp>     // setjmp/longjmp — отлов access-violation из sherpa
#endif
#endif

// Относительные пути моделей внутри Config::modelsDir (имена сверены в Фазе 0).
namespace {
constexpr const char *kRecSubdir =
    "sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16";
constexpr const char *kSegModelRel = "sherpa-onnx-pyannote-segmentation-3-0/model.onnx";
constexpr const char *kEmbModelRel = "campplus_zh_en.onnx";
}  // namespace

#ifdef HAVE_LOCAL_STT
struct SherpaPipeline::Engines {
    const SherpaOnnxOfflineRecognizer *rec = nullptr;
    const SherpaOnnxOfflineSpeakerDiarization *sd = nullptr;
    // Сохранённые параметры диаризации — для live-SetConfig при смене порога (без пересоздания).
    std::string segModel, embModel, provider;
    int numThreads = 4, numClusters = -1;
    float minOn = 0.3f, minOff = 0.5f;
};
#else
struct SherpaPipeline::Engines {};
#endif

SherpaPipeline::SherpaPipeline(QObject *parent) : QObject(parent) {}

SherpaPipeline::~SherpaPipeline() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    delete eng_;
}

void SherpaPipeline::init(const Config &cfg) {
    qRegisterMetaType<PipelineResult>("PipelineResult");
    std::lock_guard<std::mutex> lk(m_);
    if (started_) return;
    cfg_ = cfg;
    started_ = true;
#ifdef HAVE_LOCAL_STT
    worker_ = std::thread([this] { workerLoop(); });
#else
    // Сборка без локального STT — сразу сигнализируем недоступность (на главном потоке через очередь).
    QMetaObject::invokeMethod(
        this,
        [this] { emit initialized(false, QStringLiteral("Локальный STT не собран (нет sherpa-onnx)")); },
        Qt::QueuedConnection);
#endif
}

void SherpaPipeline::submit(const std::vector<float> &mono, bool diarize, quint64 tag) {
    {
        std::lock_guard<std::mutex> lk(m_);
        pending_ = mono;  // коалесинг: только последнее окно
        pendingDiarize_ = diarize;
        pendingScene_ = false;
        pendingOther_.clear();
        pendingTag_ = tag;
        hasJob_ = true;
    }
    cv_.notify_one();
}

void SherpaPipeline::submitScene(const std::vector<float> &mic, const std::vector<float> &other, quint64 tag) {
    {
        std::lock_guard<std::mutex> lk(m_);
        pending_ = mic;
        pendingOther_ = other;
        pendingScene_ = true;
        pendingTag_ = tag;
        hasJob_ = true;
    }
    cv_.notify_one();
}

#ifdef HAVE_LOCAL_STT

void SherpaPipeline::workerLoop() {
    eng_ = new Engines();
    QString err;
    const bool ok = createEngines(&err);
    ready_.store(ok);
    emit initialized(ok, err);  // queued в поток-владелец
    if (ok) {
        for (;;) {
            std::vector<float> job, other;
            bool diar = true, scene = false;
            quint64 tag = 0;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || hasJob_; });
                if (stop_) break;
                job.swap(pending_);
                other.swap(pendingOther_);
                diar = pendingDiarize_;
                scene = pendingScene_;
                tag = pendingTag_;
                hasJob_ = false;
            }
            PipelineResult res = scene ? runScene(job, other) : runOnce(job, diar);
            // Тег задания — сквозь worker в результат: только по нему окно может отличить
            // «свой» результат от пережитого (коалесинг pending_ съедает промежуточные задания
            // недетерминированно — подсчёт «сколько результатов пропустить» не работает).
            res.tag = tag;
            emit resultReady(res);
        }
    }
    destroyEngines();  // создание/использование/уничтожение — на одном worker-потоке
}

bool SherpaPipeline::createEngines(QString *err) {
    const QString recDir = cfg_.modelsDir + "/" + kRecSubdir;
    // Держим std::string живыми, пока зовём Create (C-API копирует строки внутрь себя на Create,
    // но не полагаемся — храним локально до конца вызовов).
    const std::string encoder = (recDir + "/encoder.int8.onnx").toStdString();
    const std::string decoder = (recDir + "/decoder.onnx").toStdString();
    const std::string joiner = (recDir + "/joiner.onnx").toStdString();
    const std::string tokens = (recDir + "/tokens.txt").toStdString();
    const std::string segModel = (cfg_.modelsDir + "/" + kSegModelRel).toStdString();
    const std::string embModel = (cfg_.modelsDir + "/" + kEmbModelRel).toStdString();
    const std::string provider = cfg_.provider.toStdString();

    // Ранняя проверка наличия файлов — внятная ошибка вместо NULL из Create.
    // laws-билд: только STT (диаризация не качается и не создаётся — сценовых режимов нет,
    // голосовой поиск ходит runOnce(diarize=false), которому достаточно распознавателя).
    for (const QString &p : {recDir + "/encoder.int8.onnx", recDir + "/decoder.onnx",
                             recDir + "/joiner.onnx", recDir + "/tokens.txt",
#ifndef RPOVERLAY_LAWS
                             cfg_.modelsDir + "/" + kSegModelRel,
                             cfg_.modelsDir + "/" + kEmbModelRel
#endif
                            }) {
        if (!QFileInfo::exists(p)) {
            if (err) *err = QStringLiteral("Нет файла модели: %1").arg(p);
            return false;
        }
    }

    // --- OfflineRecognizer (GigaAM-v3 nemo_transducer int8) ---
    SherpaOnnxOfflineRecognizerConfig rc;
    memset(&rc, 0, sizeof(rc));
    rc.feat_config.sample_rate = cfg_.sampleRate;
    rc.feat_config.feature_dim = 80;
    rc.model_config.transducer.encoder = encoder.c_str();
    rc.model_config.transducer.decoder = decoder.c_str();
    rc.model_config.transducer.joiner = joiner.c_str();
    rc.model_config.tokens = tokens.c_str();
    rc.model_config.model_type = "nemo_transducer";
    rc.model_config.num_threads = cfg_.numThreads;
    rc.model_config.provider = provider.c_str();
    rc.model_config.debug = 0;
    rc.decoding_method = "greedy_search";
    eng_->rec = SherpaOnnxCreateOfflineRecognizer(&rc);
    if (!eng_->rec) {
        if (err) *err = QStringLiteral("CreateOfflineRecognizer вернул NULL");
        return false;
    }

#ifndef RPOVERLAY_LAWS
    // --- OfflineSpeakerDiarization (seg3.0 + CAM++) ---
    SherpaOnnxOfflineSpeakerDiarizationConfig dc;
    memset(&dc, 0, sizeof(dc));
    dc.segmentation.pyannote.model = segModel.c_str();
    dc.segmentation.num_threads = cfg_.numThreads;
    dc.segmentation.provider = provider.c_str();
    dc.embedding.model = embModel.c_str();
    dc.embedding.num_threads = cfg_.numThreads;
    dc.embedding.provider = provider.c_str();
    dc.clustering.num_clusters = cfg_.numClusters;
    dc.clustering.threshold = cfg_.clusterThreshold;
    dc.min_duration_on = cfg_.minDurationOn;
    dc.min_duration_off = cfg_.minDurationOff;
    eng_->sd = SherpaOnnxCreateOfflineSpeakerDiarization(&dc);
    if (!eng_->sd) {
        if (err) *err = QStringLiteral("CreateOfflineSpeakerDiarization вернул NULL");
        return false;
    }
#else
    // laws-билд: диаризатор не создаём (eng_->sd остаётся nullptr — STT-путь его не требует).
    (void)segModel; (void)embModel;
#endif
    // Запоминаем параметры для live-смены порога (SetConfig) + фиксируем стартовый порог.
    eng_->segModel = segModel; eng_->embModel = embModel; eng_->provider = provider;
    eng_->numThreads = cfg_.numThreads; eng_->numClusters = cfg_.numClusters;
    eng_->minOn = cfg_.minDurationOn; eng_->minOff = cfg_.minDurationOff;
    appliedThreshold_ = cfg_.clusterThreshold;
    return true;
}

void SherpaPipeline::setClusterThreshold(float t) { wantThreshold_.store(t); }

// worker: если UI сменил порог — пересобираем конфиг диаризации и применяем SetConfig (без пересоздания).
void SherpaPipeline::applyThresholdIfChanged() {
    if (!eng_ || !eng_->sd) return;
    const float want = wantThreshold_.load();
    if (want == appliedThreshold_) return;
    SherpaOnnxOfflineSpeakerDiarizationConfig dc;
    memset(&dc, 0, sizeof(dc));
    dc.segmentation.pyannote.model = eng_->segModel.c_str();
    dc.segmentation.num_threads = eng_->numThreads;
    dc.segmentation.provider = eng_->provider.c_str();
    dc.embedding.model = eng_->embModel.c_str();
    dc.embedding.num_threads = eng_->numThreads;
    dc.embedding.provider = eng_->provider.c_str();
    dc.clustering.num_clusters = eng_->numClusters;
    dc.clustering.threshold = want;
    dc.min_duration_on = eng_->minOn;
    dc.min_duration_off = eng_->minOff;
    SherpaOnnxOfflineSpeakerDiarizationSetConfig(eng_->sd, &dc);
    appliedThreshold_ = want;
}

void SherpaPipeline::destroyEngines() {
    if (!eng_) return;
    if (eng_->sd) {
        SherpaOnnxDestroyOfflineSpeakerDiarization(eng_->sd);
        eng_->sd = nullptr;
    }
    if (eng_->rec) {
        SherpaOnnxDestroyOfflineRecognizer(eng_->rec);
        eng_->rec = nullptr;
    }
}

// --- Guard вокруг диаризации: Process падает access-violation ВНУТРИ sherpa при высоком пороге
// кластеризации на реальной многоголосой записи (баг библиотеки v1.13.2, локализован офлайн-стендом
// thirdparty/probe_scene.c; gdb-бэктрейс — краш в самой DLL). В оверлее воркер — std::thread, и такой
// SIGSEGV уронил бы ВЕСЬ процесс. GCC/MinGW не знает __try/__except (MSVC-расширение, в x64 нет),
// поэтому ловим AV через Vectored Exception Handler + setjmp/longjmp: точка возврата ставится ДО вызова,
// VEH на access-violation прыгает обратно → возвращаем nullptr (обе ветки-вызыватели уже трактуют его как
// «диаризация не удалась»). Проверено: движок переживает пойманный краш и работает на следующей сцене.
// thread_local-состояние → корректно на worker-потоке; функция БЕЗ C++-объектов с деструкторами (longjmp
// не разматывает стек). Это БЭКСТОП к клампу слайдера (≤1.15): кламп уводит из зоны краша, guard ловит
// остаток риска, если граница «уедет» на другом аудио.
namespace {
#ifdef _WIN32
thread_local std::jmp_buf g_diarJmp;
thread_local volatile int g_diarActive = 0;
LONG CALLBACK diarVeh(EXCEPTION_POINTERS *info) {
    if (g_diarActive &&
        info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        g_diarActive = 0;
        std::longjmp(g_diarJmp, 1);  // не возвращается
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
const SherpaOnnxOfflineSpeakerDiarizationResult *
diarizeGuarded(const SherpaOnnxOfflineSpeakerDiarization *sd, const float *data, int n) {
#ifdef _WIN32
    const SherpaOnnxOfflineSpeakerDiarizationResult *dr = nullptr;
    PVOID h = AddVectoredExceptionHandler(1, diarVeh);  // 1 = первым в цепочке
    g_diarActive = 1;
    if (setjmp(g_diarJmp) == 0) {
        dr = SherpaOnnxOfflineSpeakerDiarizationProcess(sd, data, n);
    } else {
        dr = nullptr;  // поймали access-violation
    }
    g_diarActive = 0;
    if (h) RemoveVectoredExceptionHandler(h);
    return dr;
#else
    return SherpaOnnxOfflineSpeakerDiarizationProcess(sd, data, n);
#endif
}
}  // namespace

PipelineResult SherpaPipeline::runOnce(const std::vector<float> &mono, bool diarize) {
    PipelineResult out;
    out.audioSeconds = mono.empty() ? 0.0 : double(mono.size()) / cfg_.sampleRate;
    // STT-путь (diarize=false, Q&A) нужен ТОЛЬКО распознаватель — диаризатор (sd) может отсутствовать
    // (STT-only сборка). Диаризацию проверяем отдельно в её ветке ниже.
    if (mono.empty() || !eng_ || !eng_->rec) {
        out.error = QStringLiteral("Пустое окно или движки не готовы");
        return out;
    }
    QElapsedTimer t;
    t.start();
    const int n = static_cast<int>(mono.size());

    // STT-only (без диаризации) — для Q&A с микрофона: один говорящий, нужен только текст.
    if (!diarize) {
        const SherpaOnnxOfflineStream *st = SherpaOnnxCreateOfflineStream(eng_->rec);
        SherpaOnnxAcceptWaveformOffline(st, cfg_.sampleRate, mono.data(), n);
        SherpaOnnxDecodeOfflineStream(eng_->rec, st);
        const SherpaOnnxOfflineRecognizerResult *r = SherpaOnnxGetOfflineStreamResult(st);
        AttributedSegment seg;
        seg.start = 0.0f;
        seg.end = static_cast<float>(out.audioSeconds);
        seg.speaker = -1;
        seg.text = (r && r->text) ? QString::fromUtf8(r->text).trimmed() : QString();
        if (!seg.text.isEmpty()) out.segments.push_back(seg);
        SherpaOnnxDestroyOfflineRecognizerResult(r);
        SherpaOnnxDestroyOfflineStream(st);
        out.numSpeakers = out.segments.isEmpty() ? 0 : 1;
        out.processSeconds = t.elapsed() / 1000.0;
        out.ok = true;
        return out;
    }

    if (!eng_->sd) {   // диаризация запрошена, но движка нет (STT-only сборка)
        out.error = QStringLiteral("Диаризация недоступна в этой сборке");
        return out;
    }
    applyThresholdIfChanged();   // подхватить порог из настроек, если сменили
    const SherpaOnnxOfflineSpeakerDiarizationResult *dr =
        diarizeGuarded(eng_->sd, mono.data(), n);   // SEH-guard: краш sherpa → nullptr, не вылет
    if (!dr) {
        out.error = QStringLiteral("Диаризация вернула NULL");
        return out;
    }
    out.numSpeakers = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSpeakers(dr);
    const int numSeg = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(dr);
    const SherpaOnnxOfflineSpeakerDiarizationSegment *segs =
        SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(dr);

    const int sr = cfg_.sampleRate;
    out.segments.reserve(numSeg);
    for (int i = 0; i < numSeg; ++i) {
        const float t0 = segs[i].start, t1 = segs[i].end;
        int begin = static_cast<int>(t0 * sr);
        int count = static_cast<int>((t1 - t0) * sr);
        if (begin < 0) begin = 0;
        if (begin + count > n) count = n - begin;
        if (count <= 0) continue;

        const SherpaOnnxOfflineStream *st = SherpaOnnxCreateOfflineStream(eng_->rec);
        SherpaOnnxAcceptWaveformOffline(st, sr, mono.data() + begin, count);
        SherpaOnnxDecodeOfflineStream(eng_->rec, st);
        const SherpaOnnxOfflineRecognizerResult *r = SherpaOnnxGetOfflineStreamResult(st);

        AttributedSegment seg;
        seg.start = t0;
        seg.end = t1;
        seg.speaker = segs[i].speaker;
        seg.text = (r && r->text) ? QString::fromUtf8(r->text).trimmed() : QString();
        if (!seg.text.isEmpty()) out.segments.push_back(seg);

        SherpaOnnxDestroyOfflineRecognizerResult(r);
        SherpaOnnxDestroyOfflineStream(st);
    }

    SherpaOnnxOfflineSpeakerDiarizationDestroySegment(segs);
    SherpaOnnxOfflineSpeakerDiarizationDestroyResult(dr);

    out.processSeconds = t.elapsed() / 1000.0;
    out.ok = true;
    return out;
}

// Сцена по каналам: mic → «Вы» (speaker=-1), other → собеседники (диаризуются). Окна выровнены по
// длине (общий таймлайн), поэтому сегменты обоих каналов сливаются хронологически по start.
PipelineResult SherpaPipeline::runScene(const std::vector<float> &mic, const std::vector<float> &other) {
    PipelineResult out;
    const size_t maxLen = std::max(mic.size(), other.size());
    out.audioSeconds = maxLen ? double(maxLen) / cfg_.sampleRate : 0.0;
    if ((mic.empty() && other.empty()) || !eng_ || !eng_->sd || !eng_->rec) {
        out.error = QStringLiteral("Пустое окно или движки не готовы");
        return out;
    }
    QElapsedTimer t;
    t.start();
    const int sr = cfg_.sampleRate;
    applyThresholdIfChanged();   // подхватить порог из настроек, если сменили

    // Диаризация+STT одного канала; self → все сегменты speaker=-1 («Вы»), иначе speaker=cluster id.
    auto processChannel = [&](const std::vector<float> &a, bool self) -> int {
        if (a.empty()) return 0;
        const int n = static_cast<int>(a.size());
        const SherpaOnnxOfflineSpeakerDiarizationResult *dr =
            diarizeGuarded(eng_->sd, a.data(), n);   // SEH-guard: краш канала → 0, без вылета оверлея
        if (!dr) return 0;
        const int num = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSpeakers(dr);
        const int numSeg = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(dr);
        const SherpaOnnxOfflineSpeakerDiarizationSegment *segs =
            SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(dr);
        for (int i = 0; i < numSeg; ++i) {
            const float t0 = segs[i].start, t1 = segs[i].end;
            int begin = static_cast<int>(t0 * sr);
            int count = static_cast<int>((t1 - t0) * sr);
            if (begin < 0) begin = 0;
            if (begin + count > n) count = n - begin;
            if (count <= 0) continue;
            const SherpaOnnxOfflineStream *st = SherpaOnnxCreateOfflineStream(eng_->rec);
            SherpaOnnxAcceptWaveformOffline(st, sr, a.data() + begin, count);
            SherpaOnnxDecodeOfflineStream(eng_->rec, st);
            const SherpaOnnxOfflineRecognizerResult *r = SherpaOnnxGetOfflineStreamResult(st);
            AttributedSegment seg;
            seg.start = t0;
            seg.end = t1;
            seg.speaker = self ? -1 : segs[i].speaker;
            seg.text = (r && r->text) ? QString::fromUtf8(r->text).trimmed() : QString();
            if (!seg.text.isEmpty()) out.segments.push_back(seg);
            SherpaOnnxDestroyOfflineRecognizerResult(r);
            SherpaOnnxDestroyOfflineStream(st);
        }
        SherpaOnnxOfflineSpeakerDiarizationDestroySegment(segs);
        SherpaOnnxOfflineSpeakerDiarizationDestroyResult(dr);
        return self ? (numSeg > 0 ? 1 : 0) : num;
    };

    const int youCount = processChannel(mic, /*self*/true);
    const int otherCount = processChannel(other, /*self*/false);

    std::sort(out.segments.begin(), out.segments.end(),
              [](const AttributedSegment &a, const AttributedSegment &b) { return a.start < b.start; });
    out.numSpeakers = youCount + otherCount;
    out.processSeconds = t.elapsed() / 1000.0;
    out.ok = true;
    return out;
}

#else  // !HAVE_LOCAL_STT — заглушки

void SherpaPipeline::workerLoop() {}
bool SherpaPipeline::createEngines(QString *) { return false; }
void SherpaPipeline::destroyEngines() {}
void SherpaPipeline::setClusterThreshold(float) {}
void SherpaPipeline::applyThresholdIfChanged() {}
PipelineResult SherpaPipeline::runOnce(const std::vector<float> &, bool) {
    PipelineResult out;
    out.error = QStringLiteral("Локальный STT не собран");
    return out;
}
PipelineResult SherpaPipeline::runScene(const std::vector<float> &, const std::vector<float> &) {
    PipelineResult out;
    out.error = QStringLiteral("Локальный STT не собран");
    return out;
}

#endif  // HAVE_LOCAL_STT
