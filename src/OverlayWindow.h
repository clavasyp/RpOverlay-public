#pragma once

#include <QWidget>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "RingBuffer.h"     // value-члены: кольцевые буферы микрофона/звука игры для локального STT
#include "SpeakerLabels.h"  // value-член: стабильные метки спикеров сцены (A/B/C… по появлению)

#include <vector>

class AuthClient;
class AudioCapture;
class VoiceClient;     // НЕ используется (зарезервирован как фолбэк серверного STT) — только хранится
class AdviceClient;
class LawsClient;      // поиск по законке (бета) — режим «Поиск» (laws)
class Updater;
class SherpaPipeline;
struct PipelineResult;
class ModelManager;
class TurnEndpointer;  // VAD-детектор «собеседник договорил» (авто-разбор сцены)

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QFrame;
class QTimer;
class QStackedWidget;
class QSystemTrayIcon;
class QMoveEvent;
class QShowEvent;
class QEvent;
class QMouseEvent;
class QResizeEvent;
class QShortcut;

// OverlayWindow — компактный бар (Cluely-стиль): тонкая плашка ~520×44 сверху по центру.
// Режимы (пилюля #modePill): Q&A — вопрос голосом (зажать PTT → локальный STT) или текстом
// (клик в строку → Enter; работает БЕЗ модели); Поиск (laws, бета) — как Q&A, но запрос идёт
// в поиск по каталогу законки (LawsClient; только на серверах с флагом search из /servers —
// динамический гейт беты); Адвокат/Прокурор/Общий — сценовые: непрерывный
// захват микрофон+звук игры, локальная диаризация+STT, авто-советы (VAD-эндпоинтер) либо разбор
// по хоткею. Ответ/совет выпадает панелью ПОД баром (cap ~40% высоты экрана) и висит до
// следующего вопроса. Окно: frameless, always-on-top, нет в таскбаре/Alt-Tab (Qt::Tool), не
// отбирает фокус у игры (WS_EX_NOACTIVATE; снимается ТОЛЬКО на время набора в строке), невидимо
// для захвата экрана/OBS (WDA_EXCLUDEFROMCAPTURE, тумблер в настройках).
class OverlayWindow : public QWidget {
    Q_OBJECT
public:
    // sharedTray — ОБЩАЯ иконка трея приложения (с 0.4.0 ею владеет окно-хаб, см. HubWindow):
    // если передана, свою не создаём (меню/клики обслуживает хаб), указатель используется
    // только для balloon-уведомлений. nullptr → историческое поведение: собственная иконка.
    explicit OverlayWindow(AuthClient *auth,
                           AudioCapture *audio,
                           VoiceClient *voice,
                           AdviceClient *advice,
                           QSystemTrayIcon *sharedTray = nullptr,
                           QWidget *parent = nullptr);

    // Показать/поднять окно (из трея или при повторном запуске exe — single-instance IPC).
    void showFromExternal();
    // Привязать Updater — уведомление «обновление готово» в трей.
    void setUpdater(Updater *u);
    // Менеджер локальных моделей (создаётся внутри, parented). main зовёт prepareForLaunch().
    ModelManager *models() const { return models_; }

public slots:
    // Открыть модальный SettingsDialog (весь виринг применения «на лету» — здесь). Публичный
    // слот: с 0.4.0 то же окно настроек открывается и из хаба (HubWindow::settingsRequested →
    // main → сюда; бар при этом может быть создан скрытым).
    void onOpenSettings();

protected:
    void mousePressEvent(QMouseEvent *e) override;   // перетаскивание за фон бара
    void mouseMoveEvent(QMouseEvent *e) override;
    void moveEvent(QMoveEvent *e) override;          // дебаунс-сохранение позиции
    void showEvent(QShowEvent *e) override;          // WDA-невидимость + NOACTIVATE + topmost
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override; // WM_HOTKEY
    bool eventFilter(QObject *obj, QEvent *ev) override; // клик/Esc/фокус строки ввода

private slots:
    // --- Auth / Q&A / серверы ---
    void onAuthorized();                            // догон пост-логина (окно создаётся ПОСЛЕ authorized)
    void onAuthError(const QString &message);
    void onQaAnswer(const QString &answer);         // ответ Q&A (лёгкий HTML) → панель под баром
    void onServersReady(const QVariantList &projects);  // кэш списка серверов (для пикера в настройках)
    void onLeadershipServersReady(const QVariantList &projects);  // кэш проектов/серверов свода «Обзвона»
    void onServersError(const QString &msg);            // сброс защёлки — разрешить ретрай
    void onServerSettingsChanged();                 // из SettingsDialog: сервер + режим при запуске

    // --- Совет по сцене (SSE-стрим) + чек-лист (сценовые режимы) ---
    void onAdviceSources(const QStringList &articles, int chunksUsed);  // «ст. 17 · ст. 19 ч.2» → #chipsLabel
    void onAdviceDelta(const QString &text);        // плейн-аппенд токена совета в панель
    void onAdviceDone();                            // стрим завершён: failsafe stop + состояние назад
    // Общий канал ошибок AdviceClient: и advise-стрим, и Q&A-запрос (бывш. onQaError —
    // для Q&A-пути поведение прежнее).
    void onAdviceError(const QString &msg);
    void onChecklistReady(const QVariantList &items);   // ✓/✗/? цветами → #checklistLabel

    // --- Режим «Поиск» (laws): выдача поиска по законке. Ошибки — ОТДЕЛЬНЫМ сигналом
    //     LawsClient::searchError (не adviceError: своё владение этапом и свои таймауты) ---
    void onLawsResults(const QString &query, const QVariantList &results,
                       const QVariantList &related, bool degraded);
    void onLawsError(const QString &code, const QString &message);

    // --- UI actions / настройки ---
    void onLogout();                // выход из аккаунта → main покажет окно активации
    void applyAudioSettings();      // применить микрофон/источник звука игры из settings::audio()
    void applyWindowSettings();     // позиция/прозрачность/шрифт/хоткеи/WDA из settings::window()

    // --- Хоткеи ---
    void toggleVisibility();        // hkToggle
    void toggleClickThrough();      // hkClickThrough
    void toggleAnswerPanel();       // hkCollapse: скрыть/показать панель ответа
    void focusInput();              // hkAsk: фокус в строку вопроса (снимает WS_EX_NOACTIVATE)
    void toggleSceneListening();    // hkLocalRecord / #listenBtn: сцена вкл ↔ пауза (сценовые режимы)
    void triggerSceneAnalysis();    // hkLocalAnswer / авто-триггер: разобрать окно сцены сейчас
    void onModePillClicked();       // #modePill: QMenu выбора режима → setMode
    void onLdrPillClicked();        // #ldrPill: QMenu выбора проекта/сервера свода «Обзвона»

    // --- Push-to-talk (локальный STT) ---
    void startPtt();                // PTT зажата → запись вопроса (mic-only) в micRing_
    void stopPtt();                 // PTT отпущена → snapshot буфера → SherpaPipeline::submit
    void startScenePtt();           // «Обзвон»: PTT зажата → захват звука собеседника (mic+игра)
    void stopScenePtt();            // «Обзвон»: PTT отпущена → разбор окна → ответ (mode=leadership)
    void pollPtt();                 // опрос GetAsyncKeyState 25мс (клавиша/numpad/кнопка мыши)

private:
    // Состояние центра бара: Idle = строка ввода (только Q&A); остальные — страница-статус.
    // Сценовые режимы живут ТОЛЬКО на странице-статусе: SceneIdle (пауза/подсказка) ↔
    // SceneListening (непрерывный захват) ↔ SceneThinking (разбор сцены / совет в полёте).
    enum class BarState { Idle, Listening, Recognizing, Thinking,
                          SceneIdle, SceneListening, SceneThinking };

    // Кламп окна в экран после роста панели ответа (layout растит окно строго ВНИЗ — у нижнего
    // края панель уезжала бы за экран). Override приватный: protected-секция заморожена
    // контрактом, а виртуальная диспетчеризация от секции доступа не зависит.
    void resizeEvent(QResizeEvent *e) override;

    void buildUi();
    void setupTray();                       // иконка в трее + меню (показ/свернуть/настройки/выход)
    void minimizeToTray();                  // ✕ бара = спрятать в трей (одноразовый balloon)
    void registerHotkeys();                 // RegisterHotKey + PTT-опрос + QShortcut-фолбэк
    void healHotkeySettings();              // одноразовая починка старых не-ASCII токенов хоткеев (RU-раскладка)
    // WS_EX_TRANSPARENT|WS_EX_LAYERED тоггл; notify=false — тихое повторное применение
    // (showEvent после пересоздания HWND), без транзиент-статуса.
    void applyClickThrough(bool enabled, bool notify = true);
    void applyCaptureAffinity();            // WDA_EXCLUDEFROMCAPTURE по settings::window().captureInvisible
    void applyWindowConfig();               // позиция/монитор/прозрачность/шрифт/потолок панели ответа
    void reapplyTopmost();                  // пере-утвердить HWND_TOPMOST (таймер 1500мс)
    void saveGeometryNow();                 // x/y/monitorIndex → settings (дебаунс 600мс из moveEvent)
    void setInputActivatable(bool on);      // фокус-трюк: снять/вернуть WS_EX_NOACTIVATE (набор текста)
    void enterState(BarState s);            // переключить страницу/текст статуса + индикатор
    void showTransientStatus(const QString &text, int ms = 5000);  // временный статус → назад к вводу
    void updateMicDot();                    // цвет/tooltip индикатора (idle/запись/busy/нет модели)
    void askQuestion(const QString &q);     // вопрос (голос или текст) → advice_->requestQa
    void startLawsSearch(const QString &q); // режим «Поиск»: запрос (голос/текст) → laws_->requestSearch
    bool serverHasSearch(const QString &serverId) const;  // флаг search из кэша /servers (projectsData_)
    void updateSearchModeGate();            // гейт беты: демоут laws→qa + видимость пилюли (QA-вариант)
    void updateLdrPill();                   // видимость+текст пилюли «Обзвона» (проект/сервер) по режиму и настройке
    void onAudioFrame(const QByteArray &pcm16, const QString &source);  // mic → micRing_; other → otherRing_+VAD
    void onAudioLevel(const QString &source, float peak);               // пик mic (диагностика «тихо» + смежность VAD)
    void ensureLocalPipeline();             // лениво создать+init SherpaPipeline (STT+диаризация)
    // Не слот (коннект функтором): PipelineResult — не метатип для строковых SIGNAL/SLOT макросов.
    void onLocalResult(const PipelineResult &r);

    // --- Режимы (Q&A / Поиск / Адвокат / Прокурор / Общий / Обзвон) ---
    QString currentMode() const { return mode_; }   // "qa"|"laws"|"advocate"|"prosecutor"|"general"|"leadership"
    void setMode(const QString &mode);      // стоп активностей старого режима + персист defaultMode + applyModeUi
    void applyModeUi();                     // пилюля/кнопка сцены/страница центра под mode_
    void updateListenBtn();                 // ▶/⏸ + tooltip (#listenBtn)
    void ensureEndpointer();                // лениво создать TurnEndpointer + коннект авто-триггера
    void requestAdviceFor(const QString &situation);  // текст сцены → SSE-совет (+чек-лист у адвоката/прокурора)
    void clearAdviceExtras();               // скрыть/очистить #chipsLabel и #checklistLabel
    // Пересчитать бюджет высоты answerView_ (answerMaxH_ минус шапка/chips/чек-лист) и поджать
    // уже зафиксированную высоту; чек-листу — собственный кэп. Зовётся при смене содержимого
    // панели (sources/checklist/clear) и потолка (applyWindowConfig/saveGeometryNow).
    void updateAnswerViewCap();
    // «Спокойное» состояние текущего режима — восстановление после ответа/ошибки/failsafe:
    // qa → Listening/Recognizing/Idle по факту PTT; сценовые → SceneListening/SceneIdle.
    BarState restingState() const;

    // QKeySequence-строка ("Ctrl+Shift+Q") → Win32 (mods, vk). false если мышь/не распарсилось.
    static bool parseHotkey(const QString &seq, unsigned &mods, unsigned &vk);

    // Зависимости (не владеем).
    AuthClient   *auth_   = nullptr;
    AudioCapture *audio_  = nullptr;
    VoiceClient  *voice_  = nullptr;   // не используется: хранится как резерв серверного STT-фолбэка
    AdviceClient *advice_ = nullptr;
    LawsClient   *laws_   = nullptr;   // поиск по законке (наш, parented в ctor; у хаба СВОЙ инстанс)
    Updater      *updater_ = nullptr;

    // Состояние.
    QString      sessionId_;            // QUuid без скобок, один на сессию окна (rolling-память Q&A)
    QString      selectedServerId_;     // выбранная законка (фолбэк cfg().serverId)
    QVariantList projectsData_;         // кэш ответа /servers (для пикера в настройках)
    bool         serversRequested_ = false;  // защёлка одноразового requestServers
    QVariantList leadershipProjects_;        // кэш /leadership/servers (проекты/сервера свода «Обзвона»)
    bool         ldrServersRequested_ = false;  // защёлка одноразового requestLeadershipServers
    bool         greetedOnce_      = false;  // защёлка приветствия (authorized эмитится и на refresh токена)
    bool         clickThrough_     = false;
    bool         layeredBeforeClickThrough_ = false;  // был ли WS_EX_LAYERED до включения click-through
    bool         inputActivatable_ = false;  // WS_EX_NOACTIVATE сейчас СНЯТ (идёт набор в строке)
    bool         everShown_        = false;  // бар хоть раз реально показан (гард show-путей хоткеев)
    bool         inSettingsDlg_    = false;  // модальный SettingsDialog открыт (гард от 2-го вложенного диалога)
    void        *prevFg_           = nullptr; // HWND окна игры до активации бара — вернуть ему фокус после Enter/Esc
    BarState     state_            = BarState::Idle;

    // Push-to-talk.
    bool     pttActive_   = false;      // PTT зажата сейчас (идёт запись в micRing_)
    bool     scenePtt_    = false;      // PTT-захват «Обзвона» (звук собеседника, не mic-only Q&A)
    bool     awaitingStt_ = false;      // ждём resultReady по отправленному окну

    // Поколения заданий локального пайплайна: resultReady атрибутируется этапу СТРОГО по тегу
    // (r.tag == sttGen_/sceneGen_). Голые bool'ы не отличали бы запоздавший результат СТАРОГО
    // задания медленного worker'а от результата нового этапа, взведённого после failsafe/смены
    // режима (сцена ушла бы Q&A-вопросом и наоборот); коалесинг pending_ съедает задания
    // недетерминированно, поэтому «пропустить N результатов» не работает. 0 = ожидания нет.
    quint64 pipelineGen_ = 0;           // счётчик: ++ на каждый submit/submitScene
    quint64 sttGen_      = 0;           // тег ожидаемого Q&A-STT-прогона (пара к awaitingStt_)
    quint64 sceneGen_    = 0;           // тег ожидаемого сценового прогона (пара к localBusy_)
    float    utterancePeak_ = 0.0f;     // макс. пик микрофона за вопрос (диагностика «тихого микрофона»)
    unsigned pttVk_   = 0;              // Win32 VK PTT (опрос GetAsyncKeyState)
    unsigned pttMods_ = 0;              // требуемые модификаторы (биты hk::Ctrl/Shift/Alt/Win)
    bool     pttPrevDown_ = false;      // предыдущее состояние (детект фронтов)

    // Сценовые режимы (Адвокат/Прокурор/Общий): локальный разбор сцены + советы.
    QString mode_ = QStringLiteral("qa");   // инициализируется в ctor из settings::server().defaultMode
    RingBuffer otherRing_{120 * 16000};     // ~120с моно 16к — звук игры (собеседники)
    SpeakerLabels speakerLabels_;           // кластеры диаризации → стабильные «A»/«B»/… (reset на сцену)
    TurnEndpointer *endpointer_ = nullptr;  // лениво (ensureEndpointer), parented к this
    bool sceneListening_ = false;           // непрерывный сценовый захват ВКЛ (рантайм, не персистится)
    bool localBusy_ = false;                // submitScene в полёте (диаризация+STT на worker)
    std::vector<float> lastOtherWindow_;    // other-окно последнего разбора — фильтр энергии сегментов
    int  lastWindowSr_ = 16000;             // sample rate этого окна
    bool awaitingFirstDelta_ = false;       // первый delta совета чистит answerView (анти-мигание)

    // Локальный STT.
    RingBuffer      micRing_{120 * 16000};  // ~120с моно 16к — микрофон (вопрос целиком влезает)
    SherpaPipeline *pipeline_ = nullptr;    // лениво, parented к this
    // Инициализация движка провалилась (битая модель/OOM/сборка-заглушка): submit завис бы
    // навсегда — PTT отбиваем подсказкой «спросите текстом». Сбрасывается по modelsReady.
    bool            pipelineFailed_ = false;
    ModelManager   *models_   = nullptr;    // создаётся в ctor, parented к this
    int             modelDlPct_ = -1;       // прогресс докачки модели (для tooltip), -1 = не качается

    // Перетаскивание / трей.
    QPoint dragOffset_;
    bool   shownTrayHint_ = false;
    int    answerMaxH_ = 400;            // потолок ВСЕЙ панели ответа (~40% высоты экрана)
    int    answerViewCap_ = 400;         // бюджет ИМЕННО answerView_: answerMaxH_ минус шапка/chips/чек-лист

    // UI-элементы.
    QFrame          *bar_         = nullptr;  // #bar — тонкая плашка 520×44
    QLabel          *micDot_      = nullptr;  // #micDot — индикатор записи/модели
    QPushButton     *modePill_    = nullptr;  // #modePill — пилюля режима (клик → QMenu выбора)
    QPushButton     *ldrPill_     = nullptr;  // #ldrPill — пилюля проекта/сервера «Обзвона» (видна в leadership)
    QStackedWidget  *centerStack_ = nullptr;  // страница-ввод ↔ страница-статус
    QLineEdit       *askInput_    = nullptr;  // #askInput
    QLabel          *statusText_  = nullptr;  // #statusText
    QPushButton     *listenBtn_   = nullptr;  // #listenBtn — ▶/⏸ сцены (виден только в сценовых режимах)
    QPushButton     *gearBtn_     = nullptr;  // ⚙ настройки
    QPushButton     *hideBtn_     = nullptr;  // ✕ в трей
    QFrame          *answerPanel_ = nullptr;  // #answerPanel — выпадает под баром
    QLabel          *qLabel_      = nullptr;  // #qLabel — вопрос/шапка совета (elided, полный в tooltip)
    QPushButton     *copyBtn_     = nullptr;  // «Копировать» (plain-текст ответа)
    QPushButton     *closeAnsBtn_ = nullptr;  // ✕ панели ответа
    QLabel          *chipsLabel_  = nullptr;  // #chipsLabel — статьи-источники совета («ст. 17 · …»)
    QTextBrowser    *answerView_  = nullptr;  // #answerView (setHtml / plain-аппенд SSE-совета)
    QLabel          *checklistLabel_ = nullptr; // #checklistLabel — чек-лист ✓/✗/? (адвокат/прокурор)
    QSystemTrayIcon *tray_        = nullptr;

    // Таймеры.
    QTimer *topmostTimer_      = nullptr;  // пере-утверждение HWND_TOPMOST
    QTimer *geoSaveTimer_      = nullptr;  // дебаунс сохранения позиции
    QTimer *pttPollTimer_      = nullptr;  // опрос PTT-клавиши
    QTimer *statusRevertTimer_ = nullptr;  // возврат из временного статуса к строке ввода
    // Failsafe от «вечного статуса»: Recognizing (STT-результат не пришёл — мёртвый движок)
    // и Thinking (сервер молчит — полуоткрытый TCP без transferTimeout). Один переиспользуемый
    // singleShot: этапы строго последовательны, одновременно активен максимум один.
    QTimer *failsafeTimer_     = nullptr;

    // QShortcut-фолбэк (если RegisterHotKey не удался) — пересоздаются в registerHotkeys.
    QList<QShortcut *> fallbackShortcuts_;
    bool hotkeysRegistered_ = false;
    // Строки хоткеев, применённые последним registerHotkeys() — чтобы applyWindowSettings
    // не дёргал Unregister/Register на каждый тик слайдера (глобальные хоткеи «мерцали»).
    QString appliedHotkeysKey_;

    // Хоткеи: id для Win32 RegisterHotKey.
    static constexpr int kHotkeyToggle      = 0xB001;
    static constexpr int kHotkeyClickThru   = 0xB002;
    static constexpr int kHotkeyAsk         = 0xB003;
    static constexpr int kHotkeyCollapse    = 0xB004;
    static constexpr int kHotkeyPtt         = 0xB005;  // зарезервирован: PTT идёт опросом (мышь), не RegisterHotKey
    static constexpr int kHotkeyLocalRec    = 0xB006;  // hkLocalRecord: сцена вкл/пауза (как в ≤0.2.20)
    static constexpr int kHotkeyLocalAnswer = 0xB007;  // hkLocalAnswer: разобрать сцену сейчас

    static constexpr int kBarWidth  = 520;
    static constexpr int kBarHeight = 44;
};
