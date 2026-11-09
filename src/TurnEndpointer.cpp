#include "TurnEndpointer.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef HAVE_LOCAL_STT
#include "sherpa-onnx/c-api/c-api.h"
#endif

#ifdef HAVE_LOCAL_STT
struct TurnEndpointer::Vad {
    const SherpaOnnxVoiceActivityDetector *h = nullptr;
};
#else
struct TurnEndpointer::Vad {};
#endif

// ----- Часть без sherpa: жизненный цикл, ввод данных от UI (компилируется всегда) -----

TurnEndpointer::TurnEndpointer(QObject *parent) : QObject(parent) {}

TurnEndpointer::~TurnEndpointer() { stop(); }

void TurnEndpointer::start(const TurnEndpointerConfig &cfg) {
    if (started_.load()) { applyConfig(cfg); return; }
    {
        std::lock_guard<std::mutex> lk(cfgM_);
        cfg_ = cfg;
    }
    micActivePeakA_.store(cfg.micActivePeak);
    epoch_ = std::chrono::steady_clock::now();
    started_.store(true);
#ifdef HAVE_LOCAL_STT
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = false;
    }
    worker_ = std::thread([this] { workerLoop(); });
#else
    active_.store(false);
#endif
}

void TurnEndpointer::stop() {
    if (!started_.load()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    started_.store(false);
    active_.store(false);
    {
        std::lock_guard<std::mutex> lk(m_);
        queue_.clear();
    }
}

void TurnEndpointer::applyConfig(const TurnEndpointerConfig &cfg) {
    {
        std::lock_guard<std::mutex> lk(cfgM_);
        cfg_ = cfg;
    }
    micActivePeakA_.store(cfg.micActivePeak);
    cfgDirty_.store(true);
    cv_.notify_one();
}

void TurnEndpointer::pushOther(const QByteArray &pcm16) {
    if (!active_.load()) return;               // VAD не готов / не собран → не копим
    const int n = static_cast<int>(pcm16.size() / 2);
    if (n <= 0) return;
    const int16_t *s = reinterpret_cast<const int16_t *>(pcm16.constData());
    std::vector<float> f;
    f.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) f.push_back(s[i] / 32768.0f);
    {
        std::lock_guard<std::mutex> lk(m_);
        queue_.insert(queue_.end(), f.begin(), f.end());
    }
    cv_.notify_one();
}

void TurnEndpointer::noteMicLevel(float peak) {
    if (!started_.load()) return;
    if (peak >= micActivePeakA_.load()) lastMicActiveMs_.store(nowMs());
}

void TurnEndpointer::resetState() { resetReq_.fetch_add(1); cv_.notify_one(); }
void TurnEndpointer::noteFired()  { firedReq_.fetch_add(1); cv_.notify_one(); }

long long TurnEndpointer::nowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - epoch_)
        .count();
}

#ifdef HAVE_LOCAL_STT

// ----- Worker-поток: VAD + многоступенчатый гейт -----

void TurnEndpointer::snapshotConfig() {
    std::lock_guard<std::mutex> lk(cfgM_);
    wcfg_ = cfg_;
}

bool TurnEndpointer::createVad(QString *err) {
    if (wcfg_.vadModelPath.isEmpty()) {
        if (err) *err = QStringLiteral("Не задан путь к silero_vad.onnx");
        return false;
    }
    if (!QFileInfo::exists(wcfg_.vadModelPath)) {
        if (err) *err = QStringLiteral("Нет файла VAD: %1").arg(wcfg_.vadModelPath);
        return false;
    }
    const std::string model = wcfg_.vadModelPath.toStdString();
    SherpaOnnxVadModelConfig vc;
    memset(&vc, 0, sizeof(vc));
    vc.silero_vad.model = model.c_str();
    vc.silero_vad.threshold = wcfg_.vadThreshold;
    vc.silero_vad.min_silence_duration = wcfg_.minSilenceSec;
    vc.silero_vad.min_speech_duration = 0.20f;  // детект речи; «достоин ли совета» решает гейт minUtteranceSec
    vc.silero_vad.window_size = wcfg_.windowSize;
    vc.silero_vad.max_speech_duration = wcfg_.maxDisputeSec;
    vc.sample_rate = wcfg_.sampleRate;
    vc.num_threads = 1;
    vc.provider = "cpu";
    vc.debug = 0;
    vad_->h = SherpaOnnxCreateVoiceActivityDetector(&vc, 30.0f);
    if (!vad_->h) {
        if (err) *err = QStringLiteral("CreateVoiceActivityDetector вернул NULL");
        return false;
    }
    return true;
}

void TurnEndpointer::destroyVad() {
    if (!vad_) return;
    if (vad_->h) {
        SherpaOnnxDestroyVoiceActivityDetector(vad_->h);
        vad_->h = nullptr;
    }
    delete vad_;
    vad_ = nullptr;
}

void TurnEndpointer::resetGateState() {
    noiseFloorRms_ = 1e-4;
    curSpeechSumRms_ = 0.0;
    curSpeechFrames_ = 0;
    inSpeech_ = false;
    speechStartMs_ = processedMs_;
    lastVoiceMs_ = processedMs_;
    accumNewSpeechSec_ = 0.0;
    lastFireProcMs_ = -1e9;
    speechStartWallMs_ = nowMs();
    if (vad_ && vad_->h) {
        SherpaOnnxVoiceActivityDetectorReset(vad_->h);
        SherpaOnnxVoiceActivityDetectorClear(vad_->h);
    }
}

void TurnEndpointer::applyPendingCommands() {
    if (cfgDirty_.exchange(false)) {
        snapshotConfig();
        destroyVad();
        vad_ = new Vad();
        QString err;
        const bool ok = createVad(&err);
        active_.store(ok);
        if (!ok) emit debugState(QStringLiteral("[endpointer] пересоздание VAD: %1").arg(err));
        resetGateState();
    }
    const int r = resetReq_.load();
    if (r != resetSeen_) { resetSeen_ = r; resetGateState(); }
    const int f = firedReq_.load();
    if (f != firedSeen_) {
        firedSeen_ = f;
        lastFireProcMs_ = processedMs_;   // ручной разбор: стартуем кулдаун, гасим дельту
        accumNewSpeechSec_ = 0.0;
    }
}

void TurnEndpointer::doFire(const char *reason) {
    lastFireProcMs_ = processedMs_;
    accumNewSpeechSec_ = 0.0;
    emit fireRequested();
    emit debugState(QStringLiteral("[endpointer] fire (%1)").arg(QString::fromLatin1(reason)));
}

bool TurnEndpointer::evaluateGate(double turnDurSec) {
    if (curSpeechFrames_ <= 0) return false;
    if (turnDurSec < wcfg_.minUtteranceSec) return false;  // слишком короткая реплика (барк/филлер)

    // Адаптивный передний план: речь должна быть громче скользящего фона на voiceMarginDb (ОТНОСИТЕЛЬНО).
    const double avgSpeech = curSpeechSumRms_ / std::max(1, curSpeechFrames_);
    const double floorR = std::max(1e-6, noiseFloorRms_);
    const double dbOver = 20.0 * std::log10(std::max(1e-6, avgSpeech) / floorR);
    const bool foreground = dbOver >= wcfg_.voiceMarginDb;

    // Разговорная смежность: была ли активность микрофона незадолго до начала этой реплики (диалог чередуется).
    const long long micMs = lastMicActiveMs_.load();
    const bool adjacent =
        micMs >= (speechStartWallMs_ - static_cast<long long>(wcfg_.adjacencyWindowSec * 1000.0));

    const bool relevant =
        wcfg_.requireAdjacency ? (foreground && adjacent) : (foreground || adjacent);
    if (!relevant) return false;

    // Подавление, пока говорит сам игрок (микрофон громкий в последние ~350 мс).
    if (wcfg_.suppressWhileMicHot && (nowMs() - micMs) < 350) return false;

    // Кулдаун + дельта (накоплено достаточно новой речи с прошлого совета).
    if ((processedMs_ - lastFireProcMs_) < wcfg_.cooldownSec * 1000.0) return false;
    if (accumNewSpeechSec_ < wcfg_.minNewSpeechSec) return false;
    return true;
}

void TurnEndpointer::processWindow(const float *p, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += static_cast<double>(p[i]) * p[i];
    const double rms = std::sqrt(sum / std::max(1, n));
    const double winSec = static_cast<double>(n) / wcfg_.sampleRate;

    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_->h, p, n);
    const bool nowSpeech = SherpaOnnxVoiceActivityDetectorDetected(vad_->h) != 0;

    processedMs_ += winSec * 1000.0;

    if (nowSpeech && !inSpeech_) {  // фронт начала реплики
        speechStartMs_ = processedMs_;
        speechStartWallMs_ = nowMs();
        curSpeechSumRms_ = 0.0;
        curSpeechFrames_ = 0;
    }
    if (nowSpeech) {
        lastVoiceMs_ = processedMs_;
        curSpeechSumRms_ += rms;
        curSpeechFrames_ += 1;
        accumNewSpeechSec_ += winSec;
    } else {
        // noise-floor: EMA по не-речевым кадрам (период полураспада noiseHalfLifeSec)
        const double a = std::exp(-winSec * 0.69314718 / std::max(0.05f, wcfg_.noiseHalfLifeSec));
        noiseFloorRms_ = noiseFloorRms_ * a + rms * (1.0 - a);
    }
    inSpeech_ = nowSpeech;

    // 1) Чистый endpoint — VAD закрыл сегмент (пауза >= min_silence). В споре с микропаузами VAD
    //    выдаёт сегменты на каждой паузе — каждый из них кандидат.
    bool cleanEndpoint = false;
    while (SherpaOnnxVoiceActivityDetectorEmpty(vad_->h) == 0) {
        const SherpaOnnxSpeechSegment *seg = SherpaOnnxVoiceActivityDetectorFront(vad_->h);
        if (seg) SherpaOnnxDestroySpeechSegment(seg);
        SherpaOnnxVoiceActivityDetectorPop(vad_->h);
        cleanEndpoint = true;
    }
    if (cleanEndpoint) {
        const double turnDur = std::max(0.0, (lastVoiceMs_ - speechStartMs_) / 1000.0);
        if (evaluateGate(turnDur)) doFire("endpoint");
    }

    // 2) Форс при затяжном споре без пауз: речь льётся непрерывно дольше maxDisputeSec — не ждём тишину.
    if (inSpeech_ && (processedMs_ - speechStartMs_) >= wcfg_.maxDisputeSec * 1000.0) {
        const double turnDur = (processedMs_ - speechStartMs_) / 1000.0;
        if (evaluateGate(turnDur)) doFire("dispute");
        // перезапуск отсчёта реплики, чтобы не палить каждый следующий кадр
        speechStartMs_ = processedMs_;
        curSpeechSumRms_ = 0.0;
        curSpeechFrames_ = 0;
    }
}

void TurnEndpointer::workerLoop() {
    vad_ = new Vad();
    snapshotConfig();
    QString err;
    const bool ok = createVad(&err);
    active_.store(ok);
    if (!ok) emit debugState(QStringLiteral("[endpointer] VAD не создан: %1").arg(err));
    resetGateState();

    std::vector<float> tail;
    std::vector<float> chunk;
    for (;;) {
        chunk.clear();
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            chunk.assign(queue_.begin(), queue_.end());
            queue_.clear();
        }
        applyPendingCommands();
        if (!active_.load() || !vad_ || !vad_->h) continue;  // нет VAD — просто выкидываем аудио

        tail.insert(tail.end(), chunk.begin(), chunk.end());
        const int win = std::max(1, wcfg_.windowSize);
        size_t off = 0;
        while (off + static_cast<size_t>(win) <= tail.size()) {
            processWindow(&tail[off], win);
            off += static_cast<size_t>(win);
        }
        if (off) tail.erase(tail.begin(), tail.begin() + off);
        if (tail.size() > static_cast<size_t>(win) * 4) tail.clear();  // страховка от роста хвоста
    }
    destroyVad();
}

#else  // !HAVE_LOCAL_STT — заглушки (worker не стартует, isActive()==false)

void TurnEndpointer::workerLoop() {}
void TurnEndpointer::applyPendingCommands() {}
void TurnEndpointer::snapshotConfig() {}
bool TurnEndpointer::createVad(QString *) { return false; }
void TurnEndpointer::destroyVad() {}
void TurnEndpointer::resetGateState() {}
void TurnEndpointer::processWindow(const float *, int) {}
bool TurnEndpointer::evaluateGate(double) { return false; }
void TurnEndpointer::doFire(const char *) {}

#endif  // HAVE_LOCAL_STT
