#include "AudioCapture.h"

#include <QDebug>

// --- WASAPI / COM ---
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>            // WAVEFORMATEX / WAVEFORMATEXTENSIBLE
#include <ksmedia.h>          // KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT
#include <mmdeviceapi.h>      // IMMDeviceEnumerator / IMMDevice / ActivateAudioInterfaceAsync
#include <audioclient.h>      // IAudioClient / IAudioCaptureClient
#include <audiopolicy.h>      // IAudioSessionManager2 / IAudioSessionControl2
#include <Functiondiscoverykeys_devpkey.h> // PKEY_Device_FriendlyName
#include <propvarutil.h>      // PROPVARIANT helpers
#include <propidl.h>          // PROPVARIANT
#include <psapi.h>            // QueryFullProcessImageNameW (через windows.h тоже доступно)
#include <avrt.h>             // AvSetMmThreadCharacteristics (MMCSS)
#include <tlhelp32.h>         // CreateToolhelp32Snapshot — резолв PID по имени/дереву

#include "WasapiProcessLoopback.h" // активационные структуры process-loopback

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
//  Мелкие RAII-помощники для COM (без ATL — только Qt+WinAPI разрешены).
// ---------------------------------------------------------------------------
namespace {

// Безопасный Release для COM-интерфейсов.
template <class T>
inline void safeRelease(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// RAII для CoInitializeEx: вызывает CoUninitialize только если init удался в этом потоке.
class ComScope {
public:
    ComScope() {
        // Multithreaded apartment — корректно для фонового потока захвата.
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // RPC_E_CHANGED_MODE: COM уже инициализирован в другом режиме — мы не «владеем» им.
        owns_ = SUCCEEDED(hr_) && hr_ != S_FALSE;
    }
    ~ComScope() {
        if (owns_)
            CoUninitialize();
    }
    // RPC_E_CHANGED_MODE = COM уже инициализирован в другом апартаменте (так бывает в
    // UI-потоке Qt: он STA). COM при этом ПОЛНОСТЬЮ рабочий — объекты создаём как обычно,
    // только не вызываем CoUninitialize (owns_=false). Поэтому это НЕ ошибка для нас.
    bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_ = E_FAIL;
    bool owns_ = false;
};

// Освобождение WAVEFORMATEX*, полученного из GetMixFormat (CoTaskMemAlloc).
struct CoTaskMemDeleter {
    void operator()(void *p) const { if (p) CoTaskMemFree(p); }
};
using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

// RAII для HANDLE событий.
class EventHandle {
public:
    EventHandle() : h_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~EventHandle() { if (h_) CloseHandle(h_); }
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr; }

private:
    HANDLE h_;
};

// Распознаём, какой формат отдаёт устройство, чтобы корректно прочитать сэмплы.
enum class SampleKind { Float32, Int16, Int32, Unsupported };

SampleKind classifyFormat(const WAVEFORMATEX *wf) {
    if (!wf)
        return SampleKind::Unsupported;

    WORD tag = wf->wFormatTag;
    WORD bits = wf->wBitsPerSample;

    // Для EXTENSIBLE смотрим в SubFormat.
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wf);
        const GUID &sub = ext->SubFormat;
        if (sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            return (bits == 32) ? SampleKind::Float32 : SampleKind::Unsupported;
        if (sub == KSDATAFORMAT_SUBTYPE_PCM) {
            if (bits == 16) return SampleKind::Int16;
            if (bits == 32) return SampleKind::Int32;
            return SampleKind::Unsupported;
        }
        return SampleKind::Unsupported;
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT)
        return (bits == 32) ? SampleKind::Float32 : SampleKind::Unsupported;
    if (tag == WAVE_FORMAT_PCM) {
        if (bits == 16) return SampleKind::Int16;
        if (bits == 32) return SampleKind::Int32;
        return SampleKind::Unsupported;
    }
    return SampleKind::Unsupported;
}

// Прочитать один сэмпл одного канала как float в диапазоне [-1, 1].
inline float readSampleFloat(const BYTE *frame, int channel, SampleKind kind, int bytesPerSample) {
    const BYTE *p = frame + static_cast<size_t>(channel) * bytesPerSample;
    switch (kind) {
    case SampleKind::Float32: {
        float v;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }
    case SampleKind::Int16: {
        int16_t v;
        std::memcpy(&v, p, sizeof(int16_t));
        return static_cast<float>(v) / 32768.0f;
    }
    case SampleKind::Int32: {
        int32_t v;
        std::memcpy(&v, p, sizeof(int32_t));
        return static_cast<float>(static_cast<double>(v) / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

inline int16_t floatToI16(float v) {
    if (v > 1.0f)  v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    int s = static_cast<int>(std::lround(v * 32767.0f));
    if (s > 32767)  s = 32767;
    if (s < -32768) s = -32768;
    return static_cast<int16_t>(s);
}

// Полный путь исполняемого файла процесса по PID (или пусто).
QString processImagePath(DWORD pid) {
    if (pid == 0)
        return QString();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return QString();
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = static_cast<DWORD>(std::size(buf));
    QString result;
    if (QueryFullProcessImageNameW(h, 0, buf, &sz))
        result = QString::fromWCharArray(buf, static_cast<int>(sz));
    CloseHandle(h);
    return result;
}

// basename исполняемого файла процесса по PID ("game.exe"), либо пусто.
QString processExeName(DWORD pid) {
    const QString full = processImagePath(pid);
    if (full.isEmpty())
        return QString();
    const int slash = full.lastIndexOf(QLatin1Char('\\'));
    return (slash >= 0) ? full.mid(slash + 1) : full;
}

// Жив ли процесс с данным PID. Используется watchdog'ом само-переакквизиции:
// если процесс, на который активирован process-loopback, ЗАВЕРШИЛСЯ — пора
// переподняться (приложение закрыли/перезапустили). OpenProcess не открылся →
// процесс уже не существует; STILL_ACTIVE (259) → ещё работает. Edge-case с
// «легальным» кодом выхода 259 для нас несущественен.
bool isProcessAlive(DWORD pid) {
    if (pid == 0)
        return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return false;
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE;
}

// PID процесса по basename exe среди ВСЕХ процессов (Toolhelp). Нужен для process-loopback,
// когда выбранное приложение СЕЙЧАС молчит (аудио-сессии нет) — но процесс запущен.
// Предпочитаем КОРЕНЬ дерева (родитель не с тем же именем): INCLUDE_TARGET_PROCESS_TREE
// тогда захватит и все дочерние процессы (Discord и пр. — многопроцессные).
DWORD findPidByExeTree(const QString &exe) {
    const QString want = exe.trimmed();
    if (want.isEmpty())
        return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    std::vector<std::pair<DWORD, DWORD>> matches; // {pid, ppid}
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile);
            if (name.compare(want, Qt::CaseInsensitive) == 0)
                matches.emplace_back(pe.th32ProcessID, pe.th32ParentProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (matches.empty())
        return 0;
    auto inSet = [&](DWORD p) {
        for (const auto &m : matches) if (m.first == p) return true;
        return false;
    };
    for (const auto &m : matches)
        if (!inSet(m.second)) return m.first; // корень дерева одноимённых процессов
    return matches.front().first;
}

// Сбор процессов с ВИДИМЫМ верхнеуровневым окном (для списка источников: приложение
// можно выбрать, даже если оно сейчас молчит и не имеет аудио-сессии).
BOOL CALLBACK collectWindowProc(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE; // только верхнеуровневые видимые окна
    if (GetWindowTextLengthW(hwnd) <= 0)
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return TRUE;
    wchar_t buf[256];
    const int n = GetWindowTextW(hwnd, buf, 256);
    auto *v = reinterpret_cast<std::vector<std::pair<DWORD, QString>> *>(lp);
    v->emplace_back(pid, QString::fromWCharArray(buf, n));
    return TRUE;
}

// ---------------------------------------------------------------------------
//  Completion-handler для ActivateAudioInterfaceAsync (process-loopback).
//  Минимальная реализация IActivateAudioInterfaceCompletionHandler без ATL.
// ---------------------------------------------------------------------------
// IAgileObject — marker-IID {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}. Объявляем GUID
// вручную (в MinGW символ IID_IAgileObject / __uuidof может отсутствовать). Именно
// по QI(IAgileObject) ActivateAudioInterfaceAsync проверяет «агильность» handler'а;
// без S_OK тут API СИНХРОННО возвращает E_ILLEGAL_METHOD_CALL (0x8000000E).
static const GUID kIID_IAgileObject =
    {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivateHandler(HANDLE done) : done_(done) {
        // КЛЮЧЕВОЕ: ActivateAudioInterfaceAsync требует, чтобы handler был AGILE
        // (free-threaded marshaling). Без этого активация падает синхронно с
        // E_ILLEGAL_METHOD_CALL (0x8000000E). Агрегируем штатный FTM и отдаём его
        // на QI(IMarshal) — это и делает объект agile без WRL/ATL.
        CoCreateFreeThreadedMarshaler(
            static_cast<IActivateAudioInterfaceCompletionHandler *>(this), &ftm_);
    }
    ~ActivateHandler() { if (ftm_) ftm_->Release(); }

    HRESULT       activateResult() const { return activateHr_; }
    IAudioClient *takeClient() { IAudioClient *c = client_; client_ = nullptr; return c; }

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == kIID_IAgileObject) {   // КЛЮЧЕВОЕ: agile-маркер — иначе E_ILLEGAL_METHOD_CALL
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
            AddRef();
            return S_OK;
        }
        // Legacy-агильность для кода, не знающего про IAgileObject: IMarshal → FTM.
        if (riid == IID_IMarshal && ftm_)
            return ftm_->QueryInterface(riid, ppv);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0)
            delete this;
        return static_cast<ULONG>(r);
    }

    // --- IActivateAudioInterfaceCompletionHandler ---
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override {
        HRESULT activateHr = E_FAIL;
        IUnknown *iface = nullptr;
        HRESULT hr = op->GetActivateResult(&activateHr, &iface);
        if (SUCCEEDED(hr) && SUCCEEDED(activateHr) && iface) {
            iface->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&client_));
            activateHr_ = client_ ? S_OK : E_NOINTERFACE;
        } else {
            activateHr_ = FAILED(hr) ? hr : activateHr;
        }
        if (iface)
            iface->Release();
        SetEvent(done_);
        return S_OK;
    }

private:
    IUnknown     *ftm_       = nullptr;   // free-threaded marshaler (agility)
    LONG          ref_       = 1;
    HANDLE        done_      = nullptr;   // не владеем — закрывает вызывающий
    IAudioClient *client_    = nullptr;
    HRESULT       activateHr_ = E_FAIL;
};

} // namespace

// ---------------------------------------------------------------------------
//  AudioCapture
// ---------------------------------------------------------------------------
AudioCapture::AudioCapture(QObject *parent) : QObject(parent) {}

AudioCapture::~AudioCapture() {
    stop();
}

void AudioCapture::start(bool micOnly) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return; // уже запущено

    // Само-переакквизиция в духе reconnect-потока OBS (см. captureThread/captureLoop):
    // captureLoop делает ОДНУ сессию захвата; captureThread переподнимает её, когда
    // целевой процесс умер/перезапустился ИЛИ (на fallback) наконец появился. ВАЖНО:
    // переакквизиция НЕ происходит на каждое отличие resolveProcessId — только по факту
    // смерти активного PID или появления процесса (иначе осцилляция session↔корень).
    micThread_  = std::thread(&AudioCapture::captureThread, this, Mode::Mic,      QStringLiteral("mic"));
    // В micOnly (Q&A/PTT) звук игры не захватываем — поднимаем только микрофон.
    if (!micOnly)
        loopThread_ = std::thread(&AudioCapture::captureThread, this, Mode::Loopback, QStringLiteral("other"));
}

void AudioCapture::stop() {
    if (!running_.exchange(false))
        return; // уже остановлено

    if (micThread_.joinable())
        micThread_.join();
    if (loopThread_.joinable())
        loopThread_.join();
}

bool AudioCapture::isRunning() const {
    return running_.load(std::memory_order_relaxed);
}

void AudioCapture::setMicDeviceId(const QString &id) {
    std::lock_guard<std::mutex> lk(selMutex_);
    micDeviceId_ = id;
}

void AudioCapture::setGameProcessExe(const QString &exe) {
    std::lock_guard<std::mutex> lk(selMutex_);
    gameProcessExe_ = exe;
}

// ---------------------------------------------------------------------------
//  Внешний поток: само-переакквизиция (reconnect-поток в духе OBS). captureLoop —
//  ОДНА сессия захвата; она выходит сама, когда watchdog внутри неё решил, что пора
//  переподняться: целевой процесс УМЕР (приложение закрыли/перезапустили) ИЛИ мы были
//  на системном fallback и выбранный процесс ПОЯВИЛСЯ. Здесь мы просто перезапускаем
//  её с коротким бэкоффом. На каждой новой итерации цель резолвится заново.
//  КЛЮЧЕВОЕ отличие от прежнего (осциллировавшего) варианта: captureLoop НЕ рвёт
//  захват из-за того, что resolveProcessId стал бы возвращать другой PID для ЖИВОЙ
//  цели — он смотрит только на смерть активного PID. Без этого был флип session↔корень.
// ---------------------------------------------------------------------------
void AudioCapture::captureThread(Mode mode, QString source) {
    while (running_.load(std::memory_order_relaxed)) {
        captureLoop(mode, source);
        if (!running_.load(std::memory_order_relaxed))
            break;
        // Бэкофф ~500мс между сессиями (не спамим активацией), дробим на короткие сны,
        // чтобы stop() отрабатывал быстро.
        for (int i = 0; i < 5 && running_.load(std::memory_order_relaxed); ++i)
            Sleep(100);
    }
}

// ---------------------------------------------------------------------------
//  Одна сессия захвата одного источника. Выполняется вне Qt event loop;
//  результат отдаётся через сигнал frame() (QueuedConnection в чужой поток).
// ---------------------------------------------------------------------------
void AudioCapture::captureLoop(Mode mode, QString source) {
    ComScope com;
    if (!com.ok()) {
        emit error(source + QStringLiteral(": CoInitializeEx failed"));
        return;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice           *device     = nullptr;
    IAudioClient        *client      = nullptr;
    IAudioCaptureClient *capture     = nullptr;
    HRESULT hr = S_OK;

    // Гарантированный teardown при любом выходе из функции.
    struct Cleanup {
        IMMDeviceEnumerator **e; IMMDevice **d; IAudioClient **c; IAudioCaptureClient **cap;
        ~Cleanup() { safeRelease(*cap); safeRelease(*c); safeRelease(*d); safeRelease(*e); }
    } cleanup{ &enumerator, &device, &client, &capture };

    // Снимок выбранных источников (см. setMicDeviceId/setGameProcessExe).
    QString selMic, selGameExe;
    {
        std::lock_guard<std::mutex> lk(selMutex_);
        selMic     = micDeviceId_;
        selGameExe = gameProcessExe_;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        emit error(source + QStringLiteral(": MMDeviceEnumerator failed"));
        return;
    }

    // --- Параметры формата: задаются ниже либо из GetMixFormat (device-путь),
    //     либо жёстко PCM16/mono/16000 (process-loopback-путь). ---
    SampleKind kind        = SampleKind::Unsupported;
    int srcChannels        = 0;
    int srcRate            = 0;
    int bytesPerSample     = 0;
    int frameBytes         = 0;

    // Event-driven режим для микрофона; loopback на render-устройстве надёжнее опрашивать
    // таймером (event-driven loopback не всегда сигналит при тишине) — используем polling.
    // Process-loopback также opt-in event-driven (см. ветку ниже).
    bool eventDriven       = (mode == Mode::Mic);
    DWORD streamFlags      = 0;

    EventHandle evt;
    const REFERENCE_TIME bufDuration = 2000000; // буфер ~200 мс (100-нс единицы)

    // Признак, что клиент уже полностью настроен (Initialize+SetEventHandle сделаны
    // в process-loopback-ветке) — тогда device-путь пропускается.
    bool clientReady = false;
    // PID, на который реально активирован process-loopback (0 = device-loopback/fallback
    // или микрофон). Сравнивается с периодическим ре-резолвом для само-переакквизиции.
    DWORD plActivePid = 0;

    // === Process-loopback ветка (только Loopback + задан конкретный процесс) ===
    if (mode == Mode::Loopback && !selGameExe.isEmpty()) {
        const DWORD pid = resolveProcessId(selGameExe);
        qInfo().noquote() << "[audio] process-loopback target exe=" << selGameExe << " pid=" << pid;
        // Диагностика вылетов ИГРЫ при захвате: OBS тоже умеет process-loopback/хук игры. Два
        // захватчика одного процесса (мы + OBS) — вероятный источник краша GTA в режиме «слушать».
        // Логируем присутствие OBS, чтобы коррелировать с репортами тестеров.
        {
            const bool obs = resolveProcessId(QStringLiteral("obs64.exe"))
                          || resolveProcessId(QStringLiteral("obs32.exe"));
            qInfo().noquote() << "[audio] PL: OBS запущен =" << obs
                              << "(при крашах игры в режиме слушания подозреваем конфликт PL↔OBS)";
        }
        if (pid == 0) {
            qWarning().noquote() << "[audio] PL: процесс не найден (pid=0) → fallback системный звук";
            emit error(source + QStringLiteral(": процесс «") + selGameExe +
                       QStringLiteral("» не найден — захватываю весь системный звук"));
            // fallthrough → device-loopback ниже
        } else if (!evt.valid()) {
            emit error(source + QStringLiteral(": CreateEvent failed — fallback на системный звук"));
        } else {
            // Активация виртуального устройства process-loopback.
            AUDIOCLIENT_ACTIVATION_PARAMS actParams = {};
            actParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            actParams.ProcessLoopbackParams.TargetProcessId     = pid;
            actParams.ProcessLoopbackParams.ProcessLoopbackMode =
                PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt          = VT_BLOB;
            pv.blob.cbSize = sizeof(actParams);
            pv.blob.pBlobData = reinterpret_cast<BYTE *>(&actParams);

            HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            bool plOk = false;
            if (done) {
                ActivateHandler *handler = new ActivateHandler(done); // ref=1, освобождаем сами
                {
                    APTTYPE aptType = APTTYPE_CURRENT; APTTYPEQUALIFIER aptQual;
                    HRESULT aptHr = CoGetApartmentType(&aptType, &aptQual);
                    qInfo().noquote() << "[audio] PL thread apt=" << static_cast<int>(aptType)
                                      << "(0=STA,1=MTA,2=NA,3=MAINSTA) aptHr=0x" + QString::number(static_cast<quint32>(aptHr), 16);
                }
                IActivateAudioInterfaceAsyncOperation *op = nullptr;
                HRESULT ah = ActivateAudioInterfaceAsync(
                    VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                    __uuidof(IAudioClient), &pv, handler, &op);
                HRESULT ar = E_FAIL;
                if (SUCCEEDED(ah)) {
                    WaitForSingleObject(done, 5000);
                    ar = handler->activateResult();
                    if (SUCCEEDED(ar)) {
                        client = handler->takeClient(); // забираем владение IAudioClient
                        plOk   = (client != nullptr);
                    }
                }
                qInfo().noquote() << "[audio] PL activate ah=0x" + QString::number(static_cast<quint32>(ah), 16)
                                  << " result=0x" + QString::number(static_cast<quint32>(ar), 16)
                                  << " plOk=" << plOk;
                if (op) op->Release();
                handler->Release();
                CloseHandle(done);
            }
            // pv.blob.pBlobData указывает на стек — НЕ освобождать через PropVariantClear.

            if (plOk) {
                // Формат захвата: НАТИВНЫЙ float32/stereo на частоте микса render-устройства —
                // ровно конфигурация OBS Application Audio Capture (win-wasapi), единственная
                // массово обкатанная на этом API. Раньше мы просили PCM16/mono/16k через
                // AUTOCONVERTPCM|SRC_DEFAULT_QUALITY → движковый SRC на каждый пакет В ПРОЦЕССЕ
                // audiodg, чей process-loopback-путь до Win11 24H2 БАГОВЫЙ (obs-studio#8064).
                // При двух одновременных тапах одного PID (мы + OBS) это лучший кандидат краша
                // ИГРЫ (инвалидация её render-сессии). Даунмикс/ресемплинг 48k→16k делаем у себя
                // (общий хвост цикла уже умеет — device-путь работает так всегда).
                DWORD mixRate = 48000;  // дефолт, если опрос микса не удался
                {
                    IMMDevice *rdev = nullptr;
                    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &rdev)) && rdev) {
                        IAudioClient *probe = nullptr;
                        if (SUCCEEDED(rdev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                     reinterpret_cast<void **>(&probe))) && probe) {
                            WAVEFORMATEX *mix = nullptr;
                            if (SUCCEEDED(probe->GetMixFormat(&mix)) && mix) {
                                mixRate = mix->nSamplesPerSec;
                                CoTaskMemFree(mix);
                            }
                            probe->Release();
                        }
                        rdev->Release();
                    }
                }
                WAVEFORMATEX wf = {};
                wf.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
                wf.nChannels       = 2;
                wf.nSamplesPerSec  = mixRate;
                wf.wBitsPerSample  = 32;
                wf.nBlockAlign     = static_cast<WORD>(wf.nChannels * wf.wBitsPerSample / 8);
                wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
                wf.cbSize          = 0;

                streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK
                            | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                        bufDuration, 0, &wf, nullptr);
                if (SUCCEEDED(hr)) {
                    kind          = SampleKind::Float32;
                    srcChannels   = 2;
                    srcRate       = static_cast<int>(mixRate);
                    bytesPerSample = 4;
                    frameBytes    = 8;
                    qInfo().noquote() << "[audio] PL формат: float32/stereo/" << mixRate << "Гц (native, без движкового SRC)";
                } else {
                    // Фолбэк на старый формат (PCM16/mono/16k + движковая конверсия) — вдруг
                    // система не приняла float32@mixRate. Лучше дорогой тап, чем никакого.
                    qWarning().noquote() << "[audio] PL native Initialize hr=0x" + QString::number(static_cast<quint32>(hr), 16)
                                         << "→ пробую legacy PCM16/16k (AUTOCONVERTPCM)";
                    WAVEFORMATEX wfl = {};
                    wfl.wFormatTag      = WAVE_FORMAT_PCM;
                    wfl.nChannels       = 1;
                    wfl.nSamplesPerSec  = static_cast<DWORD>(kTargetRate);
                    wfl.wBitsPerSample  = 16;
                    wfl.nBlockAlign     = static_cast<WORD>(wfl.nChannels * wfl.wBitsPerSample / 8);
                    wfl.nAvgBytesPerSec = wfl.nSamplesPerSec * wfl.nBlockAlign;
                    wfl.cbSize          = 0;
                    streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK
                                | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                            bufDuration, 0, &wfl, nullptr);
                    if (SUCCEEDED(hr)) {
                        kind          = SampleKind::Int16;
                        srcChannels   = 1;
                        srcRate       = kTargetRate;
                        bytesPerSample = 2;
                        frameBytes    = 2;
                    }
                }
                if (SUCCEEDED(hr))
                    hr = client->SetEventHandle(evt.get());

                if (SUCCEEDED(hr)) {
                    eventDriven   = true;
                    clientReady   = true;
                    plActivePid   = pid;
                    qInfo().noquote() << "[audio] PL ACTIVE — слушаю только" << selGameExe << "(pid" << pid << ")";
                } else {
                    qWarning().noquote() << "[audio] PL Initialize FAILED hr=0x" + QString::number(static_cast<quint32>(hr), 16)
                                         << "→ fallback системный звук";
                    emit error(source + QStringLiteral(": process-loopback Initialize failed — "
                                                       "fallback на системный звук"));
                    safeRelease(client); // освобождаем неудавшийся клиент перед fallback
                }
            } else {
                emit error(source + QStringLiteral(": process-loopback активация не удалась — "
                                                   "fallback на системный звук"));
            }
        }
    }

    // === Device-путь (микрофон или device-loopback / fallback) ===
    if (!clientReady) {
        // Mic с выбранным устройством → GetDevice по id; иначе/Loopback → endpoint по умолчанию.
        if (mode == Mode::Mic && !selMic.isEmpty()) {
            hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(selMic.utf16()), &device);
            if (FAILED(hr) || !device) {
                emit error(source + QStringLiteral(": выбранный микрофон недоступен — "
                                                   "беру устройство по умолчанию"));
                safeRelease(device);
                hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            }
        } else {
            const EDataFlow flow = (mode == Mode::Loopback) ? eRender : eCapture;
            hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
        }
        if (FAILED(hr) || !device) {
            emit error(source + QStringLiteral(": нет устройства по умолчанию"));
            return;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&client));
        if (FAILED(hr) || !client) {
            emit error(source + QStringLiteral(": IAudioClient::Activate failed"));
            return;
        }

        // Микрофон: открываем в RAW-режиме (если поддерживается) — обходим APO-цепочку Windows
        // (AGC/AEC/шумоподавление). Причина: при громком системном звуке (игра/Discord на колонках)
        // и частых пере-открытиях потока AEC/AGC «зажимали» микрофон почти в тишину → STT не слышал
        // вопрос («не расслышал», особенно на 2-3-м вопросе подряд). RAW отдаёт сырой сигнал.
        // Best-effort: нет IAudioClient2/RAW → молча продолжаем как раньше.
        if (mode == Mode::Mic) {
            IAudioClient2 *ac2 = nullptr;
            if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient2),
                                                 reinterpret_cast<void **>(&ac2))) && ac2) {
                AudioClientProperties props = {};
                props.cbSize     = sizeof(props);
                props.bIsOffload = FALSE;
                props.eCategory  = AudioCategory_Other;
                props.Options    = AUDCLNT_STREAMOPTIONS_RAW;
                const HRESULT pr = ac2->SetClientProperties(&props);
                qInfo().noquote() << "[audio] mic RAW SetClientProperties hr=0x"
                                  + QString::number(static_cast<quint32>(pr), 16);
                ac2->Release();
            }
        }

        WAVEFORMATEX *rawFmt = nullptr;
        hr = client->GetMixFormat(&rawFmt);
        if (FAILED(hr) || !rawFmt) {
            emit error(source + QStringLiteral(": GetMixFormat failed"));
            return;
        }
        WaveFormatPtr fmt(rawFmt);

        kind = classifyFormat(fmt.get());
        if (kind == SampleKind::Unsupported) {
            emit error(source + QStringLiteral(": неподдерживаемый формат устройства"));
            return;
        }

        srcChannels    = fmt->nChannels;
        srcRate        = static_cast<int>(fmt->nSamplesPerSec);
        bytesPerSample = fmt->wBitsPerSample / 8;
        frameBytes     = fmt->nBlockAlign; // байт на один аудиокадр (все каналы)
        if (srcChannels <= 0 || srcRate <= 0 || bytesPerSample <= 0 || frameBytes <= 0) {
            emit error(source + QStringLiteral(": некорректный формат устройства"));
            return;
        }

        // Event-driven для микрофона; device-loopback на render надёжнее опрашивать polling'ом.
        eventDriven = (mode == Mode::Mic);
        streamFlags = 0;
        if (mode == Mode::Loopback)
            streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        if (eventDriven)
            streamFlags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                bufDuration, 0, fmt.get(), nullptr);
        if (FAILED(hr)) {
            emit error(source + QStringLiteral(": IAudioClient::Initialize failed"));
            return;
        }

        if (eventDriven) {
            if (!evt.valid()) {
                emit error(source + QStringLiteral(": CreateEvent failed"));
                return;
            }
            hr = client->SetEventHandle(evt.get());
            if (FAILED(hr)) {
                emit error(source + QStringLiteral(": SetEventHandle failed"));
                return;
            }
        }
    }

    hr = client->GetService(__uuidof(IAudioCaptureClient),
                            reinterpret_cast<void **>(&capture));
    if (FAILED(hr) || !capture) {
        emit error(source + QStringLiteral(": GetService(IAudioCaptureClient) failed"));
        return;
    }

    // Поднять приоритет потока для аудио (Avrt). Необязательно — не фатально.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    hr = client->Start();
    if (FAILED(hr)) {
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        emit error(source + QStringLiteral(": IAudioClient::Start failed"));
        return;
    }

    // --- Состояние конвертации (resample float-моно srcRate -> 16000) ---
    const double ratio = static_cast<double>(srcRate) / static_cast<double>(kTargetRate);
    const int    samplesPerOut = kTargetRate * kFrameMs / 1000; // 1600 сэмплов на кадр 100 мс

    std::vector<float>   monoBuf;     // накопитель моно-сэмплов в исходном srcRate
    std::vector<int16_t> outBuf;      // накопитель выходных 16k int16
    outBuf.reserve(samplesPerOut);

    // Диагностика (heartbeat раз в ~1с): пик и число кадров за окно — чтобы по логам
    // видеть, идёт ли реально звук и насколько громко (поймать «тишину» при живом PID).
    float winPeak   = 0.0f;
    int   winFrames = 0;

    // resamplePos — дробная позиция чтения по monoBuf. Между вызовами resampleMono()
    // мы сохраняем в начале буфера один «исторический» сэмпл (индекс 0), относительно
    // которого resamplePos лежит в [0, 1) на стыке блоков — так интерполяция непрерывна.
    double  resamplePos = 0.0;

    auto flushOutFrame = [&]() {
        // Эмитим ровно по samplesPerOut сэмплов.
        while (static_cast<int>(outBuf.size()) >= samplesPerOut) {
            QByteArray pcm(reinterpret_cast<const char *>(outBuf.data()),
                           samplesPerOut * static_cast<int>(sizeof(int16_t)));
            // Пиковый уровень кадра (0..1) для VU-индикатора в UI.
            int peak = 0;
            for (int i = 0; i < samplesPerOut; ++i) {
                int a = outBuf[i];
                if (a < 0) a = -a;
                if (a > peak) peak = a;
            }
            const float pf = static_cast<float>(peak) / 32768.0f;
            if (pf > winPeak) winPeak = pf;
            ++winFrames;
            emit level(source, pf);
            emit frame(pcm, source);
            outBuf.erase(outBuf.begin(), outBuf.begin() + samplesPerOut);
        }
    };

    // Линейный ресэмпл накопленного monoBuf (srcRate -> kTargetRate).
    auto resampleMono = [&]() {
        const int n = static_cast<int>(monoBuf.size());
        if (n < 2)
            return; // нужно минимум 2 сэмпла для интерполяции
        while (true) {
            const int i0 = static_cast<int>(std::floor(resamplePos));
            const int i1 = i0 + 1;
            if (i1 > n - 1) // не хватает правого соседа — ждём следующий блок
                break;
            const float s0 = monoBuf[i0];
            const float s1 = monoBuf[i1];
            const float frac = static_cast<float>(resamplePos - i0);
            outBuf.push_back(floatToI16(s0 + (s1 - s0) * frac));
            resamplePos += ratio;
        }
        // Сдвигаем буфер, сохраняя один сэмпл слева от текущей позиции (как историю),
        // чтобы следующая интерполяция началась корректно (resamplePos -> [0,1)).
        const int i0now = static_cast<int>(std::floor(resamplePos));
        int dropTo = i0now;   // оставляем [dropTo .. n): dropTo станет новым индексом 0
        // ЗАЩИТА ОТ КРАША (heap corruption): при downsample (mic 48k → target, ratio>1) resamplePos
        // за один шаг может перепрыгнуть за конец буфера → i0now > n → erase(begin, begin+dropTo)
        // уходит ЗА end(). Клампим dropTo размером буфера (лишний «хвост» съест следующий блок).
        if (dropTo > n)
            dropTo = n;
        if (dropTo > 0) {
            monoBuf.erase(monoBuf.begin(), monoBuf.begin() + dropTo);
            resamplePos -= dropTo;
        }
        flushOutFrame();
    };

    // --- Основной цикл захвата ---
    DWORD lastWatchMs = GetTickCount();  // троттл heartbeat'а и watchdog'а переакквизиции
    while (running_.load(std::memory_order_relaxed)) {
        if (eventDriven) {
            DWORD wr = WaitForSingleObject(evt.get(), 100); // таймаут, чтобы проверять running_
            if (wr == WAIT_FAILED)
                break;
            if (wr == WAIT_TIMEOUT)
                continue;
        } else {
            // Polling loopback: спим короткий квант, затем дренируем пакеты.
            Sleep(10);
        }

        // Дренируем все доступные пакеты. ФАТАЛЬНЫЕ ошибки сессии (инвалидация устройства/
        // ресурсов аудио-движка — бывает при глитче audiodg, Alt-Tab из эксклюзивного
        // фуллскрина, перезапуске аудиослужбы) раньше просто прерывали дренаж и оставляли
        // МЁРТВУЮ сессию навсегда (watchdog смотрит только на жизнь PID). Теперь → teardown
        // и переакквизиция тем же путём, что при смерти процесса.
        bool sessionDead = false;
        auto fatalHr = [](HRESULT h) {
            return h == AUDCLNT_E_DEVICE_INVALIDATED
                || h == AUDCLNT_E_SERVICE_NOT_RUNNING
                || h == AUDCLNT_E_OUT_OF_ORDER
                || h == AUDCLNT_E_BUFFER_ERROR
                || h == static_cast<HRESULT>(0x88890026) /*AUDCLNT_E_RESOURCES_INVALIDATED*/;
        };
        for (;;) {
            UINT32 packetFrames = 0;
            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                if (fatalHr(hr)) sessionDead = true;
                break;
            }
            if (packetFrames == 0)
                break;

            BYTE   *data  = nullptr;
            UINT32  numFrames = 0;
            DWORD   flags = 0;
            hr = capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                if (fatalHr(hr)) sessionDead = true;
                break;
            }

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            // Микшируем каналы в моно float и докладываем в monoBuf.
            const size_t base = monoBuf.size();
            monoBuf.resize(base + numFrames);
            if (silent || data == nullptr) {
                std::fill(monoBuf.begin() + base, monoBuf.end(), 0.0f);
            } else {
                for (UINT32 f = 0; f < numFrames; ++f) {
                    const BYTE *fp = data + static_cast<size_t>(f) * frameBytes;
                    float acc = 0.0f;
                    for (int ch = 0; ch < srcChannels; ++ch)
                        acc += readSampleFloat(fp, ch, kind, bytesPerSample);
                    monoBuf[base + f] = acc / static_cast<float>(srcChannels);
                }
            }

            capture->ReleaseBuffer(numFrames);

            // Чтобы monoBuf не рос бесконечно при srcRate=48k — ресэмплим инкрементально.
            if (monoBuf.size() >= 4096)
                resampleMono();
        }

        // Сессия инвалидирована аудио-движком → teardown; captureThread переподнимет захват.
        if (sessionDead) {
            qWarning().noquote() << "[audio]" << source
                                 << "сессия инвалидирована (hr=0x" + QString::number(static_cast<quint32>(hr), 16)
                                 << ") → переакквизиция";
            break;
        }

        // Доресэмплить остаток после дренажа пакетов.
        resampleMono();

        // --- Heartbeat + watchdog само-переакквизиции (раз в ~1с) ---
        const DWORD now = GetTickCount();
        if (now - lastWatchMs >= 1000) {
            lastWatchMs = now;

            // Heartbeat: видно, идёт ли звук и насколько громко (peak), сколько кадров.
            qInfo().noquote() << "[audio] hb" << source
                              << "plPid=" << plActivePid
                              << "peak=" << QString::number(winPeak, 'f', 3)
                              << "frames=" << winFrames;
            winPeak = 0.0f;
            winFrames = 0;

            // Watchdog: только process-loopback на конкретный процесс.
            if (mode == Mode::Loopback && !selGameExe.isEmpty()) {
                if (plActivePid != 0) {
                    // Захватываем конкретный процесс. Переакквизиция ТОЛЬКО если он УМЕР
                    // (закрыли/перезапустили). Пока жив — НЕ трогаем (нет осцилляции).
                    if (!isProcessAlive(plActivePid)) {
                        qInfo().noquote() << "[audio] watchdog:" << selGameExe
                                          << "(pid" << plActivePid << ") завершился → переакквизиция";
                        break; // teardown; captureThread переподнимет сессию
                    }
                } else {
                    // Мы на системном fallback (процесс не был найден при старте сессии).
                    // Если он ПОЯВИЛСЯ — переподнимаемся уже как process-loopback на него.
                    const DWORD np = resolveProcessId(selGameExe);
                    if (np != 0) {
                        qInfo().noquote() << "[audio] watchdog:" << selGameExe
                                          << "появился (pid" << np << ") → переакквизиция на process-loopback";
                        break;
                    }
                }
            }
        }
    }

    client->Stop();
    client->Reset();
    if (mmcss)
        AvRevertMmThreadCharacteristics(mmcss);
    // cleanup освободит COM-интерфейсы; com закроет COM-апартамент.
}

// ---------------------------------------------------------------------------
//  resolveProcessId — PID-таргет для process-loopback по basename exe (CI).
//
//  Берём КОРЕНЬ дерева одноимённых процессов (процесс, чей родитель — НЕ тот же exe).
//  Вместе с флагом INCLUDE_TARGET_PROCESS_TREE это захватывает ВСЁ поддерево, т.е. все
//  дочерние процессы приложения — рендерер, audio-service (utility) и пр. Для Discord/
//  Electron/Chromium звук рендерит дочерний процесс, и корень гарантированно его включает.
//
//  ПОЧЕМУ НЕ ВЛАДЕЛЕЦ RENDER-СЕССИИ: его PID — это, как правило, дочерний utility-процесс
//  (audio-service). После ПЕРЕЗАПУСКА приложения владельцем сессии может оказаться лист,
//  чьё поддерево НЕ содержит реального рендерера → process-loopback ловит ТИШИНУ. Лог
//  это поймал: новый Discord, render-сессия pid 40052 (utility) → peak 0.000, тогда как
//  корень дерева (pid 44268) включает и рендерер, и обе utility. Корень ещё и самый
//  долгоживущий PID — watchdog не дёргает переакквизицию на «текучке» дочерних процессов.
//  (Перечисление сессий осталось в enumerateAudioProcesses — там оно для СПИСКА в UI.)
//
//  0 — процесс с таким именем не запущен (→ fallback на системный loopback).
// ---------------------------------------------------------------------------
DWORD AudioCapture::resolveProcessId(const QString &exe) {
    const QString want = exe.trimmed();
    if (want.isEmpty())
        return 0;

    const DWORD viaTree = findPidByExeTree(want);
    qInfo().noquote() << "[audio] resolve" << want
                      << (viaTree ? "→ pid" : "→ pid 0 (процесс не найден)")
                      << (viaTree ? QString::number(viaTree) : QString())
                      << (viaTree ? "(корень дерева)" : "");
    return viaTree;
}

// ---------------------------------------------------------------------------
//  enumerateMicDevices — активные устройства захвата (eCapture).
//  Вызывается из UI-потока, поэтому свой ComScope.
// ---------------------------------------------------------------------------
QList<AudioCapture::MicDeviceInfo> AudioCapture::enumerateMicDevices() {
    QList<MicDeviceInfo> out;

    ComScope com;
    if (!com.ok())
        return out;

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDeviceCollection *coll       = nullptr;

    do {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator));
        if (FAILED(hr) || !enumerator) break;

        hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
        if (FAILED(hr) || !coll) break;

        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice *dev = nullptr;
            if (FAILED(coll->Item(i, &dev)) || !dev)
                continue;

            MicDeviceInfo info;
            LPWSTR idStr = nullptr;
            if (SUCCEEDED(dev->GetId(&idStr)) && idStr) {
                info.id = QString::fromWCharArray(idStr);
                CoTaskMemFree(idStr);
            }

            IPropertyStore *props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                    pv.vt == VT_LPWSTR && pv.pwszVal) {
                    info.name = QString::fromWCharArray(pv.pwszVal);
                }
                PropVariantClear(&pv);
                props->Release();
            }
            if (info.name.isEmpty())
                info.name = QStringLiteral("Микрофон");

            if (!info.id.isEmpty())
                out.append(info);
            dev->Release();
        }
    } while (false);

    safeRelease(coll);
    safeRelease(enumerator);
    return out;
}

// ---------------------------------------------------------------------------
//  enumerateAudioProcesses — процессы с аудио-сессией на render по умолчанию.
//  Дедуп по basename исполняемого файла (CI). Вызывается из UI-потока.
// ---------------------------------------------------------------------------
QList<AudioCapture::AudioProcessInfo> AudioCapture::enumerateAudioProcesses() {
    QList<AudioProcessInfo> out;

    ComScope com;
    if (!com.ok())
        return out;

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDeviceCollection *coll       = nullptr;

    auto addExe = [&](DWORD pid, IAudioSessionControl2 *ctrl2) {
        if (pid == 0) return;
        const QString exe = processExeName(pid);
        if (exe.isEmpty()) return;
        for (const auto &p : out)
            if (p.exe.compare(exe, Qt::CaseInsensitive) == 0) return; // дедуп по exe (CI)
        AudioProcessInfo info;
        info.pid = static_cast<quint32>(pid);
        info.exe = exe;
        LPWSTR disp = nullptr;
        if (ctrl2 && SUCCEEDED(ctrl2->GetDisplayName(&disp)) && disp) {
            info.title = QString::fromWCharArray(disp).trimmed();
            CoTaskMemFree(disp);
        }
        if (info.title.isEmpty())
            info.title = exe;
        out.append(info);
    };

    do {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator));
        if (FAILED(hr) || !enumerator) break;

        // ВСЕ активные render-устройства (звук приложения может идти не на дефолтное —
        // напр. Discord на отдельный выход): обходим каждое и собираем их аудио-сессии.
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
        if (FAILED(hr) || !coll) break;

        UINT devCount = 0;
        coll->GetCount(&devCount);
        for (UINT di = 0; di < devCount; ++di) {
            IMMDevice               *device   = nullptr;
            IAudioSessionManager2   *mgr      = nullptr;
            IAudioSessionEnumerator *sessions = nullptr;
            if (SUCCEEDED(coll->Item(di, &device)) && device &&
                SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void **>(&mgr))) && mgr &&
                SUCCEEDED(mgr->GetSessionEnumerator(&sessions)) && sessions) {
                int count = 0;
                sessions->GetCount(&count);
                for (int i = 0; i < count; ++i) {
                    IAudioSessionControl  *ctrl  = nullptr;
                    IAudioSessionControl2 *ctrl2 = nullptr;
                    if (SUCCEEDED(sessions->GetSession(i, &ctrl)) && ctrl &&
                        SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                       reinterpret_cast<void **>(&ctrl2))) && ctrl2) {
                        DWORD pid = 0;
                        // Сессии в ЛЮБОМ состоянии (active/inactive/expired) — приложение могло
                        // выводить звук и сейчас молчать (как Discord вне голосового канала).
                        if (ctrl2->IsSystemSoundsSession() != S_OK &&
                            SUCCEEDED(ctrl2->GetProcessId(&pid)))
                            addExe(pid, ctrl2);
                    }
                    safeRelease(ctrl2);
                    safeRelease(ctrl);
                }
            }
            safeRelease(sessions);
            safeRelease(mgr);
            safeRelease(device);
        }
    } while (false);

    // Дополняем приложениями с ВИДИМЫМ ОКНОМ (можно выбрать источник, даже если он
    // сейчас молчит и аудио-сессии нет — напр. Discord вне голосового канала). Дедуп
    // по exe: «играющие сейчас» (из сессий выше) идут первыми, окна добавляются ниже.
    {
        std::vector<std::pair<DWORD, QString>> wins;
        EnumWindows(collectWindowProc, reinterpret_cast<LPARAM>(&wins));
        for (const auto &w : wins) {
            const QString exe = processExeName(w.first);
            if (exe.isEmpty())
                continue;
            bool dup = false;
            for (const auto &p : out)
                if (p.exe.compare(exe, Qt::CaseInsensitive) == 0) { dup = true; break; }
            if (dup)
                continue;
            AudioProcessInfo info;
            info.pid   = static_cast<quint32>(w.first);
            info.exe   = exe;
            info.title = w.second.isEmpty() ? exe : w.second;
            out.append(info);
        }
    }

    safeRelease(coll);
    safeRelease(enumerator);
    return out;
}
