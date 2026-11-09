#include "OverlayWindow.h"
#include "Config.h"
// Полные типы соседних модулей (в .h — только forward-declare).
#include "AuthClient.h"
#include "AudioCapture.h"
#include "VoiceClient.h"      // НЕ используется: хранится как резерв серверного STT-фолбэка
#include "AdviceClient.h"
#include "LawsClient.h"       // поиск по законке (режим «Поиск», бета)
#include "Theme.h"            // цвета HTML-выдачи поиска (QTextBrowser QSS-палитрой не красится)
#include "Updater.h"
#include "SettingsDialog.h"
#include "Settings.h"
#include "StealthGuard.h"
#include "HotkeyUtil.h"
#include "SherpaPipeline.h"   // локальный STT+диаризация (GigaAM/CAM++) — PTT-вопрос и разбор сцены
#include "ModelManager.h"     // докачка STT-модели (фоновый гейт из main)
#include "TurnEndpointer.h"   // VAD-эндпоинтер «собеседник договорил» — авто-разбор сцены

#include <vector>
#include <algorithm>
#include <cstdint>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QAbstractSpinBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QFrame>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextCursor>
#include <QScrollBar>
#include <QStyle>
#include <QShortcut>
#include <QKeySequence>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QGuiApplication>
#include <QApplication>
#include <QCoreApplication>
#include <QClipboard>
#include <QScreen>
#include <QUuid>
#include <QVariantMap>
#include <QFont>
#include <QFontMetrics>
#include <QDebug>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QIcon>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
// Win11 рисует видимую 1px-рамку вокруг КАЖДОГО top-level окна (даже frameless+translucent) —
// она проступала светлым hairline вокруг плавающей карточки. Убираем через
// DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE. Константы появились в SDK Win11; объявим на случай
// старых заголовков mingw-w64.
#  ifndef DWMWA_BORDER_COLOR
#    define DWMWA_BORDER_COLOR 34
#  endif
#  ifndef DWMWA_COLOR_NONE
#    define DWMWA_COLOR_NONE 0xFFFFFFFE
#  endif
// Невидимость для захвата экрана (OBS/запись): окно видит только пользователь.
// WDA_EXCLUDEFROMCAPTURE появился в Win10 2004; на старых системах вызов просто не сработает.
#  ifndef WDA_NONE
#    define WDA_NONE 0x00
#  endif
#  ifndef WDA_EXCLUDEFROMCAPTURE
#    define WDA_EXCLUDEFROMCAPTURE 0x11
#  endif
// Форс-пересчёт неклиентской рамки/композиции слоя ПОСЛЕ смены GWL_EXSTYLE: без
// SWP_FRAMECHANGED система кэширует старый ex-стиль (в т.ч. WS_EX_LAYERED), и слоёное
// окно перерисовывается «хвостами»/залипшим контентом до следующего move/resize.
static inline void commitExStyle(HWND hwnd)
{
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
#endif

// ---------------------------------------------------------------------------
// Самодостаточный QSS бара (как у ActivationWindow — НЕ зависит от Theme.*, тема осталась
// только у SettingsDialog). Палитра: near-black + crimson-акцент. БЕЗ графических drop-shadow
// эффектов (краш-подозреваемый Qt6Gui) — никакого программного «свечения», только QSS.
// ---------------------------------------------------------------------------
static const char kBarQss[] =
    "QFrame#bar, QFrame#answerPanel {"
    "  background: rgba(16,16,16,235); border: 1px solid #2E2E2E; border-radius: 10px; }"
    "QLabel { color: #EAEAEA; font-family: 'Segoe UI'; font-size: 13px; background: transparent; }"
    // Индикатор «●»: серый idle / красный запись / оранжевый busy / тусклый — модели нет.
    "QLabel#micDot[state=\"idle\"] { color: #5C5C5C; }"
    "QLabel#micDot[state=\"rec\"]  { color: #E81C5A; }"
    "QLabel#micDot[state=\"busy\"] { color: #D78A2E; }"
    "QLabel#micDot[state=\"off\"]  { color: #383838; }"
    "QLabel#statusText { color: #9A9A9A; font-size: 12px; }"
    "QLabel#qLabel { color: #9A9A9A; font-size: 12px; }"
    "QStackedWidget { background: transparent; }"
    "QLineEdit#askInput { background: transparent; color: #EAEAEA; border: 1px solid transparent;"
    "  border-radius: 6px; padding: 2px 6px; font-family: 'Segoe UI'; font-size: 13px;"
    "  selection-background-color: #E81C5A; }"
    "QLineEdit#askInput:focus { border: 1px solid #E81C5A; }"
    "QPushButton { background: transparent; color: #9A9A9A; border: none;"
    "  font-family: 'Segoe UI'; font-size: 13px; }"
    "QPushButton:hover { color: #EAEAEA; }"
    "QPushButton#copyBtn { font-size: 11px; border: 1px solid #2E2E2E; border-radius: 6px;"
    "  padding: 2px 8px; }"
    "QPushButton#copyBtn:hover { border-color: #E81C5A; }"
    // Пилюля режима (Q&A/Адвокат/Прокурор/Общий) — компактная плоская кнопка с рамкой.
    "QPushButton#modePill { font-size: 12px; border: 1px solid #2E2E2E; border-radius: 6px;"
    "  padding: 2px 10px; }"
    "QPushButton#modePill:hover { color: #EAEAEA; border-color: #E81C5A; }"
    // Кнопка сцены ▶/⏸ (только сценовые режимы): акцент, когда прослушивание идёт.
    "QPushButton#listenBtn { font-size: 14px; }"
    "QPushButton#listenBtn[state=\"on\"] { color: #E81C5A; }"
    // Статьи-источники совета и чек-лист — внутри панели ответа.
    "QLabel#chipsLabel { color: #E81C5A; font-size: 11px; }"
    "QLabel#checklistLabel { color: #EAEAEA; font-size: 12px; }"
    // Меню выбора режима (+ контекст-меню трея — общий стиль попапов окна).
    "QMenu { background: #161616; color: #EAEAEA; border: 1px solid #2E2E2E; padding: 4px;"
    "  font-family: 'Segoe UI'; font-size: 13px; }"
    "QMenu::item { padding: 5px 16px 5px 10px; border-radius: 4px; }"
    "QMenu::item:selected { background: #2A2A2A; }"
    "QMenu::item:checked { color: #E81C5A; }"
    "QTextBrowser#answerView { background: transparent; color: #EAEAEA; border: none;"
    "  font-family: 'Segoe UI'; }"
    "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
    "QScrollBar::handle:vertical { background: #2E2E2E; border-radius: 4px; min-height: 24px; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
    "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }";

// Динамическое QSS-свойство [state=…] + repolish (для перекраски micDot без setStyleSheet).
static void setUiState(QWidget *w, const char *state)
{
    if (!w || w->property("state").toString() == QLatin1String(state))
        return;
    w->setProperty("state", QString::fromLatin1(state));
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();   // без явной инвалидации frameless/translucent-окно перекрашивается лишь на move-событие
}

// Ключ «какие строки хоткеев применены» (см. appliedHotkeysKey_): applyWindowSettings
// перерегистрирует хоткеи только при реальной смене строк, а не на каждый тик слайдера.
static QString hotkeysKey(const WindowSettings &ws)
{
    const QChar sep(0x1F);  // unit separator — не встречается в QKeySequence-строках
    return ws.hkToggle + sep + ws.hkClickThrough + sep + ws.hkAsk + sep
         + ws.hkCollapse + sep + ws.hkPushToTalk + sep
         + ws.hkLocalRecord + sep + ws.hkLocalAnswer;
}

// Человекочитаемое имя режима — пилюля на баре, пункты меню, шапка совета в панели.
static QString modeTitle(const QString &m)
{
    if (m == QLatin1String("laws"))       return QStringLiteral("Поиск");
    if (m == QLatin1String("advocate"))   return QStringLiteral("Адвокат");
    if (m == QLatin1String("prosecutor")) return QStringLiteral("Прокурор");
    if (m == QLatin1String("general"))    return QStringLiteral("Общий");
    if (m == QLatin1String("leadership")) return QStringLiteral("Обзвон");
    return QStringLiteral("Q&A");
}

// Компактная HTML-выдача поиска по законке для панели бара: топ-3 карточки — строка
// «[код] название — санкция», ниже сниппет серым. Цвета — под активную тему.
static QString lawsResultsHtml(const QVariantList &results, bool degraded)
{
    const char *accent = theme::isMajestic() ? "#E81C5A" : "#8B7CF8";
    const char *muted  = theme::isMajestic() ? "#BABAC0" : "#9AA0AE";
    const char *dim    = theme::isMajestic() ? "#7A7A80" : "#6A6F7A";
    QString h;
    if (degraded)
        // Векторное плечо недоступно — маленький бейдж, НЕ ошибка (результаты валидны).
        h += QStringLiteral("<div style='color:%1;font-size:11px'>упрощённый поиск</div>")
                 .arg(QLatin1String(dim));
    int shown = 0;
    for (const QVariant &v : results) {
        if (shown >= 3)
            break;   // в баре компактно: топ-3, остальное — в хабе
        const QVariantMap m = v.toMap();
        const QString penalty = m.value(QStringLiteral("penalty_summary")).toString();
        const QString snippet = m.value(QStringLiteral("snippet")).toString();
        h += QStringLiteral("<div style='margin-bottom:8px'><span style='color:%1;"
                            "font-weight:700'>%2</span>")
                 .arg(QLatin1String(accent),
                      LawsClient::cardCaption(m).toHtmlEscaped());
        if (!penalty.isEmpty())
            h += QStringLiteral(" <span style='color:%1'>— %2</span>")
                     .arg(QLatin1String(muted), penalty.toHtmlEscaped());
        if (!snippet.isEmpty())
            h += QStringLiteral("<br><span style='color:%1;font-size:12px'>%2</span>")
                     .arg(QLatin1String(dim), snippet.toHtmlEscaped());
        h += QStringLiteral("</div>");
        ++shown;
    }
    if (shown == 0)
        h += QStringLiteral("<div style='color:%1'>Ничего не найдено — попробуй другие "
                            "слова (номер статьи, синонимы).</div>").arg(QLatin1String(muted));
    return h;
}

// Прогресс-плейсхолдеры панели ответа. Единые константы: их ставят askQuestion /
// startLawsSearch / requestAdviceFor, а setMode по точному совпадению отличает «этап был
// в полёте» (плейсхолдер мёртв — запрос отменён) от реального контента, который не трогаем.
static const char *kPhQaThinking   = "💭 Думаю…";
static const char *kPhLawsSearching = "🔎 Ищу статьи…";
static const char *kPhAdviceThinking = "💭 Думаю над советом…";

// Собрать конфиг VAD-эндпоинтера: путь к silero + развёрнутый пресет чувствительности (мс → сек).
static TurnEndpointerConfig makeEndpointerConfig(const QString &vadPath)
{
    const AutoAdviceTuning &t = settings::autoAdviceTuning();
    TurnEndpointerConfig c;
    c.vadModelPath        = vadPath;
    c.sampleRate          = 16000;
    c.windowSize          = 512;
    c.vadThreshold        = t.vadThreshold;
    c.minSilenceSec       = t.vadHangoverMs / 1000.0f;
    c.minUtteranceSec     = t.minUtteranceMs / 1000.0f;
    c.maxDisputeSec       = t.maxDisputeMs / 1000.0f;
    c.voiceMarginDb       = t.voiceMarginDb;
    c.cooldownSec         = t.cooldownMs / 1000.0f;
    c.minNewSpeechSec     = t.retriggerDeltaSec;
    c.requireAdjacency    = t.requireAdjacency;
    c.suppressWhileMicHot = t.suppressWhileMicHot;
    return c;
}

// Путь к silero_vad.onnx. В отличие от STT-моделей (качаются в %LOCALAPPDATA%/.../models), VAD
// крошечный и ЗАШИТ в установщик рядом с exe ({app}/models). Порядок поиска: рядом с exe (prod) →
// dev-thirdparty → download-каталог (если юзер положил туда). Если файла нет — эндпоинтер неактивен.
static QString resolveVadPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/models/silero_vad.onnx"),                                   // prod (bundled)
        QDir(appDir + QStringLiteral("/../thirdparty/models")).absolutePath()
            + QStringLiteral("/silero_vad.onnx"),                                             // dev
        ModelManager::modelsDir() + QStringLiteral("/silero_vad.onnx"),                       // download-каталог
    };
    for (const QString &p : candidates)
        if (QFileInfo::exists(p)) return p;
    return candidates.first();   // дефолт — prod-путь (эндпоинтер сам деградирует, если файла нет)
}

// ---------------------------------------------------------------------------
OverlayWindow::OverlayWindow(AuthClient *auth,
                             AudioCapture *audio,
                             VoiceClient *voice,
                             AdviceClient *advice,
                             QSystemTrayIcon *sharedTray,
                             QWidget *parent)
    : QWidget(parent), auth_(auth), audio_(audio), voice_(voice), advice_(advice)
{
    // voice_ сознательно только сохраняется (см. .h): серверный STT-путь в 0.3.0 не используется,
    // указатель оставлен как точка возврата фолбэка, если локальный движок где-то не взлетит.
    Q_UNUSED(voice_);

    // Общий трей приложения (с 0.4.0 иконкой владеет хаб): если передан — setupTray() в конце
    // конструктора увидит tray_ != nullptr и вторую иконку не создаст; указатель нужен бару
    // только для balloon-уведомлений (свернулся в трей / обновление готово).
    tray_ = sharedTray;

    sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Выбранная законка — из настроек (пикер живёт в SettingsDialog), фолбэк — зашитый дефолт.
    const ServerSettings &srv = settings::server();
    selectedServerId_ = srv.serverId.isEmpty() ? cfg().serverId : srv.serverId;
    // Режим при запуске — из тех же настроек. Пусто/неизвестное → Q&A (новый дефолт 0.3.0;
    // раньше пустой defaultMode значил general). Здесь только читаем — персистит setMode().
#if defined(RPOVERLAY_LAWS)
    // laws-билд: ЕДИНСТВЕННЫЙ режим — «Поиск по законке». Никаких Q&A/ролей/сцены.
    mode_ = QStringLiteral("laws");
#elif defined(RPOVERLAY_QA)
    // Публичный QA-вариант: ЕДИНСТВЕННЫЙ режим — Q&A. Поиск (laws) ВЫРЕЗАН на компиляции
    // (RPOVERLAY_SEARCH не определён) → режим laws недоступен даже при персисте defaultMode=laws.
    mode_ = QStringLiteral("qa");
#else
    mode_ = (srv.defaultMode == QLatin1String("advocate")
             || srv.defaultMode == QLatin1String("prosecutor")
             || srv.defaultMode == QLatin1String("general")
             || srv.defaultMode == QLatin1String("leadership")
             || srv.defaultMode == QLatin1String("laws"))
                ? srv.defaultMode : QStringLiteral("qa");
#endif

    setWindowTitle(QStringLiteral("RP Overlay"));
    // Qt::Tool — нет кнопки в таскбаре/Alt-Tab; frameless + translucent — «плашка в воздухе».
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    // Маркер для StealthGuard::sweepAllTopLevel(): бар держит своё WDA-аффинити сам
    // (applyCaptureAffinity, с учётом captureInvisible) — sweep его не трогает.
    setProperty("stealthIsBar", true);

    // Менеджер моделей — ДО buildUi (индикатор модели читает его статус) и до main->prepareForLaunch().
    models_ = new ModelManager(this);

    buildUi();
    // Native HWND (winId ниже) создаётся ДО первого show → без этого он был бы дефолтного ~640×480,
    // и скрытый бар (созданный ради настроек) висел бы невидимым прямоугольником в видимой зоне =
    // прозрачный квадрат при любом всплытии. Активируем layout + adjustSize → HWND сразу размером пилюли.
    if (layout()) layout()->activate();
    adjustSize();

#ifdef Q_OS_WIN
    {
        // Окно НЕ активируется кликом (не отбирает фокус у игры). Снимается ТОЛЬКО на время
        // набора в строке вопроса — см. setInputActivatable().
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
        commitExStyle(hwnd);   // применить смену ex-стиля к слою (SWP_FRAMECHANGED) — без «хвостов»
        // Снять светлую системную рамку Win11 вокруг окна (hairline вокруг карточки).
        COLORREF noBorder = DWMWA_COLOR_NONE;
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
    }
#endif

    // Таймеры — ДО connect'ов: догон onAuthorized ниже уже зовёт showTransientStatus
    // (нужен statusRevertTimer_), а applyWindowConfig двигает окно (нужен geoSaveTimer_).
    // Дебаунс сохранения позиции (после перетаскивания).
    geoSaveTimer_ = new QTimer(this);
    geoSaveTimer_->setSingleShot(true);
    geoSaveTimer_->setInterval(600);
    connect(geoSaveTimer_, &QTimer::timeout, this, &OverlayWindow::saveGeometryNow);

    // Возврат из временного статуса: enterState(state_) восстановит правильную страницу/текст.
    statusRevertTimer_ = new QTimer(this);
    statusRevertTimer_->setSingleShot(true);
    connect(statusRevertTimer_, &QTimer::timeout, this, [this] { enterState(state_); });

    // Failsafe от «вечного статуса»: страница-статус перекрывает строку ввода, поэтому
    // зависший этап блокировал бы И голос, И текст до перезапуска. Recognizing — STT-результат
    // может не прийти никогда (движок умер между init и submit); Thinking — Q&A-запрос идёт
    // без transferTimeout (полуоткрытый TCP через relay). SceneThinking — те же риски у разбора
    // сцены (60с, см. triggerSceneAnalysis) и SSE-совета (90с, см. requestAdviceFor). Этапы
    // строго последовательны — один переиспользуемый singleShot. По срабатыванию — мягкий сброс.
    failsafeTimer_ = new QTimer(this);
    failsafeTimer_->setSingleShot(true);
    connect(failsafeTimer_, &QTimer::timeout, this, [this] {
        if (state_ == BarState::Recognizing) {
            awaitingStt_ = false;
            sttGen_ = 0;   // запоздавший resultReady живого-но-медленного worker'а станет «чужим поколением»
            enterState(BarState::Idle);
            showTransientStatus(QStringLiteral("Не удалось распознать — попробуй ещё раз."));
        } else if (state_ == BarState::Thinking) {
            enterState(BarState::Idle);
            showTransientStatus(QStringLiteral("Сервер не ответил — попробуйте ещё раз."));
        } else if (state_ == BarState::SceneThinking) {
            // Два под-этапа сцены различаем по localBusy_: true — молчит локальный прогон
            // (диаризация+STT), false — молчит SSE-совет. Запоздавший resultReady отсечётся
            // по поколению (sceneGen_=0 — worker мог быть просто медленным, а не мёртвым);
            // запоздавшие SSE и чек-лист глушим cancelAdvice.
            const bool sttStage = localBusy_;
            localBusy_ = false;
            sceneGen_ = 0;
            awaitingFirstDelta_ = false;
            if (!sttStage && advice_) advice_->cancelAdvice();
            enterState(restingState());
            showTransientStatus(sttStage
                ? QStringLiteral("Не удалось разобрать сцену — попробуйте ещё раз.")
                : QStringLiteral("Сервер не ответил — совет не получен."));
        }
    });

    // Держаться поверх игры: периодически пере-утверждаем TOPMOST (лаунчеры/игра перехватывают z-order).
    topmostTimer_ = new QTimer(this);
    topmostTimer_->setInterval(1500);
    connect(topmostTimer_, &QTimer::timeout, this, &OverlayWindow::reapplyTopmost);
    topmostTimer_->start();

    // Аккуратный выход (трей → «Выход»): окно создаётся в main без родителя и деструктор не
    // зовётся — глушим захват (PTT И сценовый) и join'им worker'ы STT/VAD вручную. Иначе
    // инференс (секунды CPU) доживает до разрушения статиков CRT/onnxruntime → редкие краши
    // на выходе процесса.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if ((pttActive_ || sceneListening_) && audio_) audio_->stop();
        pttActive_ = false;
        sceneListening_ = false;
        delete endpointer_;   // деструктор стопит и join'ит VAD-worker
        endpointer_ = nullptr;
        delete pipeline_;     // деструктор корректно стопит и join'ит worker-поток
        pipeline_ = nullptr;
    });

    // Стартовый режим: страница центра/пилюля/кнопка сцены под mode_. СТРОГО ДО auth-догона
    // ниже — его транзиент-приветствие не должно быть затёрто enterState().
    applyModeUi();

    // --- Подключения (функторные: полные заголовки подключены выше) ---
    if (auth_) {
        connect(auth_, &AuthClient::authorized, this, [this](const QString &) { onAuthorized(); });
        connect(auth_, &AuthClient::authError, this, &OverlayWindow::onAuthError);
        // Окно создаётся ПОСЛЕ сигнала authorized (main → ensureOverlay) — догоняем вручную,
        // иначе пост-логин-настройка (requestServers) не выполнится.
        if (auth_->isAuthorized())
            onAuthorized();
    }
    if (audio_) {
        connect(audio_, &AudioCapture::error, this, [this](const QString &m) {
            showTransientStatus(QStringLiteral("Звук: %1").arg(m));
        });
        connect(audio_, &AudioCapture::level, this,
                [this](const QString &src, float peak) { onAudioLevel(src, peak); });
        connect(audio_, &AudioCapture::frame, this,
                [this](const QByteArray &pcm, const QString &src) { onAudioFrame(pcm, src); });
    }
    if (advice_) {
        connect(advice_, &AdviceClient::qaAnswer, this, &OverlayWindow::onQaAnswer);
        // adviceError — общий канал ошибок клиента: и SSE-совет сцены, и Q&A-запрос.
        connect(advice_, &AdviceClient::adviceError, this, &OverlayWindow::onAdviceError);
        connect(advice_, &AdviceClient::serversReady, this, &OverlayWindow::onServersReady);
    connect(advice_, &AdviceClient::leadershipServersReady, this, &OverlayWindow::onLeadershipServersReady);
        connect(advice_, &AdviceClient::serversError, this, &OverlayWindow::onServersError);
        // SSE-совет по сцене (сценовые режимы): источники → токены → done; чек-лист отдельно.
        connect(advice_, &AdviceClient::sources, this, &OverlayWindow::onAdviceSources);
        connect(advice_, &AdviceClient::delta, this, &OverlayWindow::onAdviceDelta);
        connect(advice_, &AdviceClient::done, this, &OverlayWindow::onAdviceDone);
        connect(advice_, &AdviceClient::checklistReady, this, &OverlayWindow::onChecklistReady);
    }
    // Поиск по законке (режим «Поиск», бета) — СВОЙ клиент (у хаба отдельный инстанс:
    // их отмены запросов и выдачи не должны пересекаться). Ошибки — отдельным сигналом.
    laws_ = new LawsClient(auth_, this);
    connect(laws_, &LawsClient::searchReady, this, &OverlayWindow::onLawsResults);
    connect(laws_, &LawsClient::searchError, this, &OverlayWindow::onLawsError);
    // Статус модели → индикатор micDot (НЕ блокируем ввод: текстовый вопрос работает без модели).
    connect(models_, &ModelManager::statusChanged, this,
            [this](int, const QString &) { updateMicDot(); });
    connect(models_, &ModelManager::progress, this,
            [this](qint64 rcv, qint64 total, int, int) {
                modelDlPct_ = total > 0 ? static_cast<int>(rcv * 100 / total) : 0;
                updateMicDot();
            });
    connect(models_, &ModelManager::modelsReady, this, [this] {
        modelDlPct_ = -1;
        // Перекачанная модель — повод дать движку второй шанс (pipelineFailed_ ставится
        // при провале инициализации, типовая причина — битый файл модели).
        pipelineFailed_ = false;
        updateMicDot();
    });

    healHotkeySettings();   // починить старые не-ASCII привязки (RU-раскладка) ДО регистрации
    registerHotkeys();

    // Позиция/прозрачность/шрифт/потолок ответа — из сохранённых настроек окна.
    applyWindowConfig();
    // Программный move выше взвёл дебаунс сохранения — гасим, иначе дефолтная позиция
    // персистится как hasGeometry уже на первом запуске (сломало бы «верх по центру» при
    // смене монитора). Сохраняем только то, что двигал ПОЛЬЗОВАТЕЛЬ.
    if (geoSaveTimer_) geoSaveTimer_->stop();

    // Стартовое состояние уже выставлено applyModeUi() выше; индикатор освежаем после
    // applyWindowConfig (PTT-клавиша в tooltip могла поменяться вместе с настройками).
    updateMicDot();

    setupTray();   // иконка в трее присутствует всё время работы
}

// ---------------------------------------------------------------------------
// Раскладка: вертикально #bar (520×44) + #answerPanel (скрыта). SetFixedSize —
// высота окна сама следует за видимостью/высотой панели ответа.
// ---------------------------------------------------------------------------
void OverlayWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);
    root->setSizeConstraint(QLayout::SetFixedSize);

    // --- Бар ---
    bar_ = new QFrame(this);
    bar_->setObjectName("bar");
    bar_->setFixedSize(kBarWidth, kBarHeight);
    auto *bl = new QHBoxLayout(bar_);
    bl->setContentsMargins(12, 0, 6, 0);
    bl->setSpacing(8);

    micDot_ = new QLabel(QStringLiteral("●"), bar_);
    micDot_->setObjectName("micDot");
    micDot_->setFixedWidth(14);
    micDot_->setAlignment(Qt::AlignCenter);
    bl->addWidget(micDot_, 0);

    // Пилюля режима — между индикатором и центром: клик → QMenu из 4 режимов.
    modePill_ = new QPushButton(modeTitle(mode_), bar_);
    modePill_->setObjectName("modePill");
    modePill_->setToolTip(QStringLiteral("Режим ассистента"));
    modePill_->setCursor(Qt::PointingHandCursor);
    modePill_->setFixedHeight(26);
    bl->addWidget(modePill_, 0);
    connect(modePill_, &QPushButton::clicked, this, &OverlayWindow::onModePillClicked);

    // Пилюля выбора свода «Обзвона» (проект/сервер) — рядом с пилюлей режима, стиль #modePill.
    // Видна ТОЛЬКО в режиме leadership (updateLdrPill из applyModeUi). Для Majestic — просто проект,
    // для GTA5RP — проект · сервер (Davis).
    ldrPill_ = new QPushButton(bar_);
    ldrPill_->setObjectName("modePill");
    ldrPill_->setToolTip(QStringLiteral("Свод правил «Обзвона»: проект/сервер"));
    ldrPill_->setCursor(Qt::PointingHandCursor);
    ldrPill_->setFixedHeight(26);
    ldrPill_->setVisible(false);
    bl->addWidget(ldrPill_, 0);
    connect(ldrPill_, &QPushButton::clicked, this, &OverlayWindow::onLdrPillClicked);
#ifdef RPOVERLAY_QA
    // Публичный QA-вариант: базово единственный режим Q&A — пилюля скрыта. Появится, когда
    // выбранный сервер даст флаг search (updateSearchModeGate): тогда доступен «Поиск» (бета).
    modePill_->setVisible(false);
#endif

    centerStack_ = new QStackedWidget(bar_);
    askInput_ = new QLineEdit(centerStack_);
    askInput_->setObjectName("askInput");
    askInput_->setPlaceholderText(QStringLiteral("Спросить… (Enter — отправить)"));
    askInput_->installEventFilter(this);   // клик → снять NOACTIVATE; Esc/FocusOut → вернуть
    // Фильтр на всё приложение — только ради PTT-комбинации (см. eventFilter): она не должна
    // «дожимать» кнопку под фокусом. Хаб живёт отдельным окном, поэтому фильтр именно на qApp.
    qApp->installEventFilter(this);
    centerStack_->addWidget(askInput_);    // страница 0 — ввод (Idle)
    statusText_ = new QLabel(centerStack_);
    statusText_->setObjectName("statusText");
    centerStack_->addWidget(statusText_);  // страница 1 — статус (запись/распознаю/думаю/ошибки)
    bl->addWidget(centerStack_, 1);

    connect(askInput_, &QLineEdit::returnPressed, this, [this] {
        const QString cm = currentMode();
        if (cm != QLatin1String("qa") && cm != QLatin1String("laws"))
            return;   // текстовый ввод — только режимы со строкой (в сценовых она скрыта)
        const QString q = askInput_->text().trimmed();
        askInput_->clear();
        askInput_->clearFocus();
        setInputActivatable(false);   // вернуть фокус игре сразу после отправки
        if (q.isEmpty())
            return;
        // Ветвление по режиму: «Поиск» ищет по каталогу законки, Q&A спрашивает ассистента.
        if (cm == QLatin1String("laws"))
            startLawsSearch(q);
        else
            askQuestion(q);
    });

    // Кнопка сцены ▶/⏸ — только сценовые режимы (applyModeUi прячет в Q&A).
    listenBtn_ = new QPushButton(QStringLiteral("▶"), bar_);
    listenBtn_->setObjectName("listenBtn");
    listenBtn_->setCursor(Qt::PointingHandCursor);
    listenBtn_->setFixedSize(26, 26);
    listenBtn_->setVisible(false);
    bl->addWidget(listenBtn_, 0);
    connect(listenBtn_, &QPushButton::clicked, this, &OverlayWindow::toggleSceneListening);

    gearBtn_ = new QPushButton(QStringLiteral("⚙"), bar_);
    gearBtn_->setObjectName("gearBtn");
    gearBtn_->setToolTip(QStringLiteral("Настройки"));
    gearBtn_->setCursor(Qt::PointingHandCursor);
    gearBtn_->setFixedSize(26, 26);
    bl->addWidget(gearBtn_, 0);
    connect(gearBtn_, &QPushButton::clicked, this, &OverlayWindow::onOpenSettings);

    hideBtn_ = new QPushButton(QStringLiteral("✕"), bar_);
    hideBtn_->setObjectName("hideBtn");
    hideBtn_->setToolTip(QStringLiteral("Свернуть в трей (выход — через меню трея)"));
    hideBtn_->setCursor(Qt::PointingHandCursor);
    hideBtn_->setFixedSize(26, 26);
    bl->addWidget(hideBtn_, 0);
    connect(hideBtn_, &QPushButton::clicked, this, &OverlayWindow::minimizeToTray);

    root->addWidget(bar_);

    // --- Панель ответа (выпадает под баром; висит до следующего вопроса / hkCollapse / ✕) ---
    answerPanel_ = new QFrame(this);
    answerPanel_->setObjectName("answerPanel");
    answerPanel_->setFixedWidth(kBarWidth);
    auto *al = new QVBoxLayout(answerPanel_);
    al->setContentsMargins(12, 8, 12, 10);
    al->setSpacing(6);

    auto *head = new QHBoxLayout();
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(6);
    qLabel_ = new QLabel(answerPanel_);
    qLabel_->setObjectName("qLabel");
    head->addWidget(qLabel_, 1);
    copyBtn_ = new QPushButton(QStringLiteral("Копировать"), answerPanel_);
    copyBtn_->setObjectName("copyBtn");
    copyBtn_->setCursor(Qt::PointingHandCursor);
    head->addWidget(copyBtn_, 0);
    closeAnsBtn_ = new QPushButton(QStringLiteral("✕"), answerPanel_);
    closeAnsBtn_->setObjectName("closeAnsBtn");
    closeAnsBtn_->setToolTip(QStringLiteral("Скрыть ответ"));
    closeAnsBtn_->setCursor(Qt::PointingHandCursor);
    closeAnsBtn_->setFixedSize(22, 22);
    head->addWidget(closeAnsBtn_, 0);
    al->addLayout(head);

    // Статьи-источники совета (SSE sources): «ст. 17 · ст. 19 ч.2». Скрыта, пока пусто.
    chipsLabel_ = new QLabel(answerPanel_);
    chipsLabel_->setObjectName("chipsLabel");
    chipsLabel_->setWordWrap(true);
    chipsLabel_->setVisible(false);
    al->addWidget(chipsLabel_);

    answerView_ = new QTextBrowser(answerPanel_);
    answerView_->setObjectName("answerView");
    answerView_->setOpenExternalLinks(false);
    answerView_->setFrameShape(QFrame::NoFrame);
    answerView_->viewport()->setAutoFillBackground(false);   // прозрачный фон вьюпорта
    answerView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    al->addWidget(answerView_);

    // Чек-лист процедуры (advocate/prosecutor): строки «✓/✗/?» rich-text'ом. Скрыт, пока пусто.
    // QLabel (не списочный виджет): скроллится только answerView_, чек-лист растёт панелью.
    checklistLabel_ = new QLabel(answerPanel_);
    checklistLabel_->setObjectName("checklistLabel");
    checklistLabel_->setTextFormat(Qt::RichText);
    checklistLabel_->setWordWrap(true);
    // Прижат кверху: у чек-листа собственный кэп высоты (см. updateAnswerViewCap) — при клипе
    // длинного списка резаться должен низ, а не середина (дефолт QLabel — вертикальный центр).
    checklistLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    checklistLabel_->setVisible(false);
    al->addWidget(checklistLabel_);

    // Высота ответа следует за содержимым, но не выше бюджета answerViewCap_ (= answerMaxH_
    // минус шапка/chips/чек-лист: потолок ~40% экрана — у ВСЕЙ панели, не только текста) —
    // дальше скролл.
    connect(answerView_->document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this](const QSizeF &sz) {
                const int h = qBound(40, static_cast<int>(sz.height()) + 12, answerViewCap_);
                answerView_->setFixedHeight(h);
            });

    connect(copyBtn_, &QPushButton::clicked, this, [this] {
        if (!answerView_) return;
        QApplication::clipboard()->setText(answerView_->toPlainText());
        showTransientStatus(QStringLiteral("Ответ скопирован."), 2000);
    });
    connect(closeAnsBtn_, &QPushButton::clicked, this,
            [this] { answerPanel_->setVisible(false); });

    answerPanel_->setVisible(false);
    root->addWidget(answerPanel_);

    setStyleSheet(QString::fromLatin1(kBarQss));
}

// ---------------------------------------------------------------------------
// Состояния бара: Idle = строка ввода (Q&A); остальное — страница-статус с текстом этапа.
// Сценовые режимы НИКОГДА не уходят со страницы-статуса (паритет со старой версией:
// текстовый ввод в них недоступен).
// ---------------------------------------------------------------------------
void OverlayWindow::enterState(BarState s)
{
    state_ = s;
    if (statusRevertTimer_) statusRevertTimer_->stop();
    if (!centerStack_) return;
    switch (s) {
    case BarState::Idle:
        centerStack_->setCurrentIndex(0);
        break;
    case BarState::Listening:
        statusText_->setText(QStringLiteral("🔴 Слушаю… (отпусти — распознаю)"));
        centerStack_->setCurrentIndex(1);
        break;
    case BarState::Recognizing:
        statusText_->setText(QStringLiteral("⏳ Распознаю вопрос…"));
        centerStack_->setCurrentIndex(1);
        break;
    case BarState::Thinking:
        // Thinking делят Q&A (LLM-ответ) и «Поиск» (мгновенный каталожный запрос) — текст по режиму.
        statusText_->setText(currentMode() == QLatin1String("laws")
            ? QStringLiteral("🔎 Ищу статьи…")
            : QStringLiteral("💭 Думаю…"));
        centerStack_->setCurrentIndex(1);
        break;
    case BarState::SceneIdle:
        if (currentMode() == QLatin1String("leadership"))
            statusText_->setText(QStringLiteral("👂 Зажми «%1» — слушать вопрос интервьюера")
                                     .arg(settings::window().hkPushToTalk));
        else
            statusText_->setText(QStringLiteral("👂 «%1» или ▶ — слушать сцену")
                                     .arg(settings::window().hkLocalRecord));
        centerStack_->setCurrentIndex(1);
        break;
    case BarState::SceneListening:
        statusText_->setText(currentMode() == QLatin1String("leadership")
            ? QStringLiteral("🔴 Слушаю вопрос… (отпусти — отвечу)")
            : QStringLiteral("👂 Слушаю сцену…"));
        centerStack_->setCurrentIndex(1);
        break;
    case BarState::SceneThinking:
        statusText_->setText(QStringLiteral("💭 Анализирую сцену…"));
        centerStack_->setCurrentIndex(1);
        break;
    }
    updateMicDot();
    update();   // слоёное окно: применить смену страницы/статуса без ожидания move
}

// «Спокойное» состояние текущего режима — куда возвращаться после ответа/ошибки/failsafe.
// Q&A: учитывает уже идущий следующий PTT-цикл (нельзя затирать «🔴 Слушаю…» строкой ввода —
// micDot остался бы красным при странице ввода, противоречивая картинка).
OverlayWindow::BarState OverlayWindow::restingState() const
{
    // Режимы со строкой ввода (Q&A и «Поиск») живут на Idle-ветке состояний.
    if (currentMode() == QLatin1String("qa") || currentMode() == QLatin1String("laws"))
        return pttActive_ ? BarState::Listening
                          : (awaitingStt_ ? BarState::Recognizing : BarState::Idle);
    return sceneListening_ ? BarState::SceneListening : BarState::SceneIdle;
}

// Временный статус (ошибки/подсказки): показываем на странице-статусе и через ms возвращаем
// текущее состояние (Idle → строка ввода). Активные этапы (запись/распознаю/думаю) не теряются —
// enterState(state_) восстановит их текст.
void OverlayWindow::showTransientStatus(const QString &text, int ms)
{
    if (!centerStack_ || !statusText_) return;
    qInfo().noquote() << "[ui] статус:" << text;
    statusText_->setText(text);
    centerStack_->setCurrentIndex(1);
    update();   // слоёное окно: показать статус сразу (иначе виден до move)
    if (statusRevertTimer_) statusRevertTimer_->start(ms);
}

// Индикатор «●»: красный — запись (PTT или сценовый захват); оранжевый — распознаю/думаю;
// тусклый — модели нет (с tooltip); серый — готов. НЕ блокирует ввод: без модели работает
// текстовый вопрос (Q&A).
void OverlayWindow::updateMicDot()
{
    if (!micDot_) return;
    const WindowSettings &ws = settings::window();
    const char *st = "idle";
    QString tip;
    if (currentMode() == QLatin1String("qa"))
        tip = QStringLiteral("Голосовой вопрос: зажми «%1» и говори").arg(ws.hkPushToTalk);
    else if (currentMode() == QLatin1String("laws"))
        tip = QStringLiteral("Голосовой поиск: зажми «%1» и назови статью или ситуацию")
                  .arg(ws.hkPushToTalk);
    else if (currentMode() == QLatin1String("leadership"))
        tip = QStringLiteral("Обзвон: зажми «%1», пока интервьюер задаёт вопрос — отпусти для ответа")
                  .arg(ws.hkPushToTalk);
    else
        tip = QStringLiteral("Сцена: «%1» — слушать/пауза · «%2» — разобрать сейчас")
                  .arg(ws.hkLocalRecord, ws.hkLocalAnswer);
    if (pttActive_) {
        st = "rec";
    } else if (state_ == BarState::Recognizing || state_ == BarState::Thinking
               || state_ == BarState::SceneThinking) {
        st = "busy";
    } else if (sceneListening_) {
        st = "rec";   // непрерывный сценовый захват — пишем микрофон+игру
    } else if (models_ && !models_->isReady()) {
        st = "off";
        if (models_->status() == ModelManager::Status::Downloading)
            tip = QStringLiteral("Модель распознавания скачивается… %1%")
                      .arg(qMax(0, modelDlPct_));
        else
            tip = QStringLiteral("Модель распознавания не скачана — голосовой вопрос недоступен "
                                 "(текстовый работает). Докачать: Настройки → Управление.");
    }
    micDot_->setToolTip(tip);
    setUiState(micDot_, st);
}

#ifdef Q_OS_WIN
// Починка СТАРОГО токена хоткея: если ключевая часть — одиночный НЕ-ASCII символ (напр. «№»/
// кириллица; писались до захвата по VK на RU-раскладке), tokenToVk даёт 0 → хоткей молча не
// регистрировался. Восстанавливаем VK через VkKeyScanExW по всем установленным раскладкам и
// возвращаем нормализованный латинский токен. Пусто = «отключён» (не трогаем). Не восстановили
// → дефолт (чтобы не завис в тихом фолбэке).
static QString healHotkeyToken(const QString &tok, const QString &defTok)
{
    unsigned mods = 0, vk = 0;
    if (hk::parse(tok, mods, vk)) return tok;   // уже валиден
    if (tok.trimmed().isEmpty())  return tok;   // «отключён» — валидное состояние
    unsigned m = 0; QString keyPart;
    const QStringList parts = tok.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString u = raw.trimmed().toUpper();
        if (u == QLatin1String("CTRL") || u == QLatin1String("CONTROL")) m |= hk::Ctrl;
        else if (u == QLatin1String("SHIFT")) m |= hk::Shift;
        else if (u == QLatin1String("ALT"))   m |= hk::Alt;
        else if (u == QLatin1String("WIN") || u == QLatin1String("META")) m |= hk::Win;
        else keyPart = raw.trimmed();
    }
    if (keyPart.size() == 1) {
        const wchar_t ch = static_cast<wchar_t>(keyPart.at(0).unicode());
        HKL layouts[16];
        const int n = GetKeyboardLayoutList(16, layouts);
        for (int i = 0; i < n; ++i) {
            const SHORT r = VkKeyScanExW(ch, layouts[i]);
            if (r != -1) {
                const unsigned v = static_cast<unsigned>(r & 0xFF);
                if (!hk::vkToToken(v).isEmpty())
                    return hk::format(m, v);   // нормализованный латинский токен
            }
        }
    }
    return defTok;   // не восстановили → дефолт (НЕ оставляем в тихом фолбэке)
}
#endif

// Одноразовая нормализация всех хоткей-настроек (см. healHotkeyToken) с персистом.
void OverlayWindow::healHotkeySettings()
{
#ifdef Q_OS_WIN
    WindowSettings ws = settings::window();
    const WindowSettings def;
    bool changed = false;
    auto fix = [&](QString &field, const QString &defTok) {
        const QString h = healHotkeyToken(field, defTok);
        if (h != field) { field = h; changed = true; }
    };
    fix(ws.hkToggle,       def.hkToggle);
    fix(ws.hkClickThrough, def.hkClickThrough);
    fix(ws.hkAsk,          def.hkAsk);
    fix(ws.hkCollapse,     def.hkCollapse);
    fix(ws.hkLocalRecord,  def.hkLocalRecord);
    fix(ws.hkLocalAnswer,  def.hkLocalAnswer);
    fix(ws.hkPushToTalk,   def.hkPushToTalk);
    if (changed) {
        settings::setWindow(ws);
        qInfo() << "[hotkey] нормализованы старые не-ASCII привязки хоткеев (RU-раскладка)";
    }
#endif
}

// ---------------------------------------------------------------------------
// Хоткеи: Win32 RegisterHotKey (глобально) + PTT-опрос. QShortcut — fallback.
// Повторно вызывается из applyWindowSettings (сначала Unregister всех).
// ---------------------------------------------------------------------------
void OverlayWindow::registerHotkeys()
{
    const WindowSettings &ws = settings::window();
    appliedHotkeysKey_ = hotkeysKey(ws);   // запомнить применённое (см. applyWindowSettings)
    // Пересоздаём фолбэк-шорткаты (хоткеи могли поменяться в настройках).
    qDeleteAll(fallbackShortcuts_);
    fallbackShortcuts_.clear();
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    // Снимаем прежние (повторный вызов из applyWindowSettings).
    UnregisterHotKey(hwnd, kHotkeyToggle);
    UnregisterHotKey(hwnd, kHotkeyClickThru);
    UnregisterHotKey(hwnd, kHotkeyAsk);
    UnregisterHotKey(hwnd, kHotkeyCollapse);
    UnregisterHotKey(hwnd, kHotkeyPtt);
    UnregisterHotKey(hwnd, kHotkeyLocalRec);
    UnregisterHotKey(hwnd, kHotkeyLocalAnswer);

    // Каждый хоткей регистрируется НЕЗАВИСИМО. Раньше hotkeysRegistered_ = ok1 && ok2 && ok3
    // (всё-или-ничего): ОДНА невалидная привязка (напр. символ без VK на RU-раскладке) роняла
    // ВСЕ глобальные хоткеи в локальный QShortcut-фолбэк (работает лишь при фокусе окна). Теперь
    // фолбэк — точечно и ТОЛЬКО для тех, чей RegisterHotKey реально не удался.
    auto reg = [&](const QString &seq, int id) -> bool {
        unsigned mm = 0, v = 0;
        if (!parseHotkey(seq, mm, v)) {
            if (!seq.trimmed().isEmpty())
                qWarning() << "[hotkey] не распарсен токен" << seq << "(id" << Qt::hex << id << ")";
            return false;
        }
        if (!RegisterHotKey(hwnd, id, mm | MOD_NOREPEAT, v)) {
            qWarning() << "[hotkey] RegisterHotKey не удался для" << seq << "(id" << Qt::hex << id
                       << ", GetLastError" << GetLastError() << ")";
            return false;
        }
        return true;
    };
    const bool okToggle   = reg(ws.hkToggle,       kHotkeyToggle);
    const bool okClick    = reg(ws.hkClickThrough, kHotkeyClickThru);
    const bool okAsk      = reg(ws.hkAsk,          kHotkeyAsk);
    const bool okCollapse = reg(ws.hkCollapse,     kHotkeyCollapse);
    const bool okLocalRec = reg(ws.hkLocalRecord,  kHotkeyLocalRec);
    const bool okLocalAns = reg(ws.hkLocalAnswer,  kHotkeyLocalAnswer);
    // Push-to-talk — ГИБКИЙ биндинг: клавиша / numpad / КНОПКА МЫШИ. RegisterHotKey мышь не умеет,
    // поэтому PTT детектим опросом GetAsyncKeyState (и нажатие, и отпускание) — см. pollPtt.
    pttVk_ = 0; pttMods_ = 0; pttPrevDown_ = false;
    hk::parse(ws.hkPushToTalk, pttMods_, pttVk_);
    if (!pttPollTimer_) {
        pttPollTimer_ = new QTimer(this);
        pttPollTimer_->setInterval(25);
        connect(pttPollTimer_, &QTimer::timeout, this, &OverlayWindow::pollPtt);
    }
    pttPollTimer_->start();
    // Точечный фолбэк ТОЛЬКО для провалившихся хоткеев (локальный QShortcut — при фокусе окна).
    auto mkScFail = [this](const QString &seq, void (OverlayWindow::*slot)()) {
        if (seq.trimmed().isEmpty()) return;   // пустая привязка = «отключён», фолбэк не нужен
        auto *sc = new QShortcut(QKeySequence(seq), this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this, slot);
        fallbackShortcuts_.append(sc);
    };
    if (!okToggle)   mkScFail(ws.hkToggle,       &OverlayWindow::toggleVisibility);
    if (!okClick)    mkScFail(ws.hkClickThrough, &OverlayWindow::toggleClickThrough);
    if (!okAsk)      mkScFail(ws.hkAsk,          &OverlayWindow::focusInput);
    if (!okCollapse) mkScFail(ws.hkCollapse,     &OverlayWindow::toggleAnswerPanel);
    if (!okLocalRec) mkScFail(ws.hkLocalRecord,  &OverlayWindow::toggleSceneListening);
    if (!okLocalAns) mkScFail(ws.hkLocalAnswer,  &OverlayWindow::triggerSceneAnalysis);
    hotkeysRegistered_ = okToggle && okClick && okAsk;   // диагностический флаг (прежняя семантика)
    return;   // Windows: обработка завершена (глобально + точечный фолбэк)
#endif
    // Fallback: локальные шорткаты (работают, пока окно в фокусе).
    auto mkSc = [this](const QString &seq, void (OverlayWindow::*slot)()) {
        auto *sc = new QShortcut(QKeySequence(seq), this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this, slot);
        fallbackShortcuts_.append(sc);
    };
    mkSc(ws.hkToggle,       &OverlayWindow::toggleVisibility);
    mkSc(ws.hkClickThrough, &OverlayWindow::toggleClickThrough);
    mkSc(ws.hkAsk,          &OverlayWindow::focusInput);
    mkSc(ws.hkCollapse,     &OverlayWindow::toggleAnswerPanel);
    mkSc(ws.hkLocalRecord,  &OverlayWindow::toggleSceneListening);
    mkSc(ws.hkLocalAnswer,  &OverlayWindow::triggerSceneAnalysis);
}

bool OverlayWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        MSG *msg = static_cast<MSG *>(message);
        if (msg && msg->message == WM_HOTKEY) {
            const int id = static_cast<int>(msg->wParam);
            if (id == kHotkeyToggle)      { toggleVisibility();     if (result) *result = 0; return true; }
            if (id == kHotkeyClickThru)   { toggleClickThrough();   if (result) *result = 0; return true; }
            if (id == kHotkeyAsk)         { focusInput();           if (result) *result = 0; return true; }
            if (id == kHotkeyCollapse)    { toggleAnswerPanel();    if (result) *result = 0; return true; }
            if (id == kHotkeyLocalRec)    { toggleSceneListening(); if (result) *result = 0; return true; }
            if (id == kHotkeyLocalAnswer) { triggerSceneAnalysis(); if (result) *result = 0; return true; }
            // PTT (kHotkeyPtt) НЕ через WM_HOTKEY — детектим опросом в pollPtt (мышь/отпускание).
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

// Токен-строка → Win32 (mods, vk) для RegisterHotKey. Через общий hk::parse:
// Ctrl/Shift/Alt/Win + одиночные клавиши, numpad, именованные (Space/F-ряд/...).
// Кнопки мыши через RegisterHotKey нельзя — для них false (PTT использует опрос, см. pollPtt).
bool OverlayWindow::parseHotkey(const QString &seq, unsigned &mods, unsigned &vk)
{
#ifdef Q_OS_WIN
    unsigned hm = 0, v = 0;
    if (!hk::parse(seq, hm, v)) return false;
    if (v == 0x01 || v == 0x02 || v == 0x04 || v == 0x05 || v == 0x06) return false;  // мышь → не сюда
    mods = 0;
    if (hm & hk::Ctrl)  mods |= MOD_CONTROL;
    if (hm & hk::Shift) mods |= MOD_SHIFT;
    if (hm & hk::Alt)   mods |= MOD_ALT;
    if (hm & hk::Win)   mods |= MOD_WIN;
    vk = v;
    return vk != 0;
#else
    Q_UNUSED(seq); Q_UNUSED(mods); Q_UNUSED(vk);
    return false;
#endif
}

void OverlayWindow::toggleVisibility()
{
    const bool willShow = !isVisible();
    // Гард: ПОКАЗАТЬ бар хоткеем можно только если он уже запускался через хаб (everShown_) и сейчас нет
    // модального диалога. Иначе случайный Ctrl+Shift+O (WM_HOTKEY идёт мимо Qt-модальности) выводил на
    // десктоп никогда не запускавшийся translucent-бар = прозрачный квадрат (особ. при открытых настройках,
    // где бар создан СКРЫТЫМ). СКРЫТЬ видимый бар — можно всегда.
    if (willShow && (!everShown_ || QApplication::activeModalWidget()))
        return;
    setVisible(willShow);
    if (willShow)
        raise();
    else
        setInputActivatable(false);   // скрыли бар — снять возможный залипший режим ввода (фокус игре)
}

void OverlayWindow::toggleClickThrough()
{
    applyClickThrough(!clickThrough_);
}

void OverlayWindow::applyClickThrough(bool enabled, bool notify)
{
    const bool wasEnabled = clickThrough_;
    clickThrough_ = enabled;
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (enabled) {
        // Запоминаем, был ли слой ДО нас: на Win11 с DWM Qt рисует полупрозрачное окно
        // без WS_EX_LAYERED, и снимать его в этом случае обязательно — иначе окно навсегда
        // остаётся слоёным и перекрашивается лишь на move/resize (те самые фантомы).
        // Только на ПЕРЕХОДЕ выкл→вкл: showEvent переприменяет режим повторно и иначе
        // запомнил бы уже НАШ же выставленный бит.
        if (!wasEnabled)
            layeredBeforeClickThrough_ = (ex & WS_EX_LAYERED) != 0;
        ex |= (WS_EX_TRANSPARENT | WS_EX_LAYERED);
    } else {
        ex &= ~WS_EX_TRANSPARENT;
        if (!layeredBeforeClickThrough_)
            ex &= ~WS_EX_LAYERED;   // возвращаем ровно то состояние, что было до включения
    }
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
    commitExStyle(hwnd);   // применить смену стиля к слою (без «хвостов» прозрачной рамки)
#endif
    if (!notify)
        return;   // тихое повторное применение (showEvent) — статус не показываем
    showTransientStatus(enabled
        ? QStringLiteral("Клики проходят насквозь («%1» — выкл)").arg(settings::window().hkClickThrough)
        : QStringLiteral("Бар снова кликабелен."));
}

// hkCollapse: скрыть/показать панель ответа (показываем только если ответ есть).
void OverlayWindow::toggleAnswerPanel()
{
    if (!answerPanel_) return;
    if (answerPanel_->isVisible())
        answerPanel_->setVisible(false);
    else if (answerView_ && !answerView_->document()->isEmpty())
        answerPanel_->setVisible(true);
}

// ---------------------------------------------------------------------------
// Фокус-трюк строки ввода. Окно создано с WS_EX_NOACTIVATE (не отбирает фокус у игры) —
// в таком окне QLineEdit НЕ принимает клавиатуру. На время набора СНИМАЕМ стиль и активируем
// окно; по Esc/FocusOut/после Enter — возвращаем (фокус уходит обратно игре).
// ---------------------------------------------------------------------------
void OverlayWindow::setInputActivatable(bool on)
{
    if (inputActivatable_ == on)
        return;
    inputActivatable_ = on;
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (on) {
        // Запоминаем окно игры ДО того, как сами станем foreground: возврат WS_EX_NOACTIVATE
        // ниже никого не реактивирует — фокус игре придётся отдавать вручную.
        prevFg_ = GetForegroundWindow();
    }
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (on) ex &= ~WS_EX_NOACTIVATE;
    else    ex |= WS_EX_NOACTIVATE;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
    commitExStyle(hwnd);   // применить смену NOACTIVATE к слою
    if (!on) {
        // Вернуть фокус игре после Enter/Esc: иначе оверлей остаётся foreground (его
        // активировал activateWindow выше), Qt-фокуса внутри нет — клавиатура не идёт
        // НИКУДА, игра деактивирована до ручного клика. Возвращаем ТОЛЬКО если foreground
        // всё ещё мы: при клике в другое окно фокус уже ушёл сам — насильная реактивация
        // была бы во вред.
        const HWND prev = reinterpret_cast<HWND>(prevFg_);
        prevFg_ = nullptr;
        if (prev && prev != hwnd && IsWindow(prev) && GetForegroundWindow() == hwnd)
            SetForegroundWindow(prev);
    }
#endif
    if (on) {
        activateWindow();
        if (askInput_) askInput_->setFocus(Qt::MouseFocusReason);
    }
}

// hkAsk: показать окно (если скрыто) и дать фокус строке ввода — то же, что клик в неё.
void OverlayWindow::focusInput()
{
    if (!isVisible()) {
        // Тот же гард, что и в toggleVisibility: не выводить хоткеем (Ctrl+Shift+Q) никогда не
        // запускавшийся бар / бар из-под модального диалога (иначе прозрачный квадрат).
        if (!everShown_ || QApplication::activeModalWidget())
            return;
        show();
        reapplyTopmost();
    }
    if (currentMode() != QLatin1String("qa") && currentMode() != QLatin1String("laws")) {
        // В сценовых режимах строки ввода нет (паритет со старой версией) — подсказываем.
        showTransientStatus(QStringLiteral("Текстовый ввод — в режимах Q&A и «Поиск»."));
        return;
    }
    if (state_ != BarState::Idle)
        return;   // во время записи/распознавания строка скрыта страницей-статусом
    if (statusRevertTimer_) statusRevertTimer_->stop();
    if (centerStack_) centerStack_->setCurrentIndex(0);
    setInputActivatable(true);
}

bool OverlayWindow::eventFilter(QObject *obj, QEvent *ev)
{
    // PTT-комбинация не должна ЖАТЬ КНОПКУ ПОД ФОКУСОМ.
    //
    // Space — штатная клавиша активации QPushButton, и Qt при этом НЕ смотрит на модификаторы.
    // Кнопка, по которой только что кликнули, сохраняет фокус: нажал в хабе «Запустить/скрыть
    // оверлей», затем зажал дефолтный Ctrl+Shift+Space — и Space «дожал» ту же кнопку, бар
    // исчезал целиком. С дефолтными настройками это ловилось на первом же голосовом вопросе.
    //
    // Сам PTT читается опросом GetAsyncKeyState (pollPtt), а не событиями Qt, поэтому событие
    // можно проглотить без потерь для записи голоса.
    if (pttVk_ && (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease)) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        const bool isPttKey = (ke->nativeVirtualKey() == pttVk_)
            && (ke->modifiers().testFlag(Qt::ControlModifier) == ((pttMods_ & hk::Ctrl)  != 0))
            && (ke->modifiers().testFlag(Qt::ShiftModifier)   == ((pttMods_ & hk::Shift) != 0))
            && (ke->modifiers().testFlag(Qt::AltModifier)     == ((pttMods_ & hk::Alt)   != 0));
        // Исключения: в модальном окне настроек ловят НОВУЮ привязку (там событие нужно),
        // а в текстовом поле человек печатает — вмешиваться в набор нельзя. Проверка широкая,
        // потому что PTT можно переназначить на обычную букву, и тогда глотать её при наборе
        // было бы куда хуже исходного бага.
        QWidget *fw = QApplication::focusWidget();
        const bool typing = fw && (fw->inputMethodHints() != Qt::ImhNone
                                   || qobject_cast<QLineEdit *>(fw)
                                   || qobject_cast<QTextEdit *>(fw)
                                   || qobject_cast<QPlainTextEdit *>(fw)
                                   || qobject_cast<QAbstractSpinBox *>(fw)
                                   || qobject_cast<QKeySequenceEdit *>(fw));
        if (isPttKey && !typing && !QApplication::activeModalWidget())
            return true;   // проглатываем: до кнопки под фокусом комбинация не доходит
    }

    if (obj == askInput_) {
        switch (ev->type()) {
        case QEvent::MouseButtonPress:
            // Клик в строку → разрешить активацию + фокус (не съедаем — каретка встанет по клику).
            setInputActivatable(true);
            break;
        case QEvent::FocusOut:
            // Кликнули мимо/ушли — окно снова «не активируемое», фокус возвращается игре.
            setInputActivatable(false);
            break;
        case QEvent::KeyPress: {
            auto *ke = static_cast<QKeyEvent *>(ev);
            if (ke->key() == Qt::Key_Escape) {
                askInput_->clearFocus();
                setInputActivatable(false);
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

// ---------------------------------------------------------------------------
//  Системный трей: показать/свернуть/настройки/выход. Окно — Qt::Tool (нет в таскбаре),
//  поэтому «свернуть» некуда, кроме трея; выход из приложения — тоже через него.
// ---------------------------------------------------------------------------
void OverlayWindow::setupTray()
{
    // С 0.4.0 иконкой трея владеет хаб (HubWindow) и отдаёт её сюда как sharedTray — тогда
    // tray_ уже задан, вторую иконку не плодим (меню/клики обслуживает хаб). Ветка ниже —
    // фолбэк исторического поведения (бар без хаба): собственная иконка со своим меню.
    if (tray_ || !QSystemTrayIcon::isSystemTrayAvailable())
        return;

    QIcon ic = qApp->windowIcon();
    if (ic.isNull())
        ic = QIcon(QStringLiteral(":/assets/app.ico"));

    tray_ = new QSystemTrayIcon(ic, this);
    tray_->setToolTip(QStringLiteral("RP-ассистент"));

    auto *menu = new QMenu(this);
    menu->addAction(QStringLiteral("Показать оверлей"), this, &OverlayWindow::showFromExternal);
    menu->addAction(QStringLiteral("Свернуть в трей"),  this, &OverlayWindow::minimizeToTray);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Настройки…"),       this, &OverlayWindow::onOpenSettings);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Выход"),            qApp, &QApplication::quit);
    tray_->setContextMenu(menu);

    // ЛКМ/двойной клик по иконке — показать/поднять окно.
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) {
                if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
                    showFromExternal();
            });
    tray_->show();
}

void OverlayWindow::minimizeToTray()
{
    hide();   // app.setQuitOnLastWindowClosed(false) → приложение живёт, иконка в трее остаётся
    if (tray_ && !shownTrayHint_) {
        // Клик по общей иконке трея открывает ХАБ — бар оттуда поднимается кнопкой/хоткеем.
        tray_->showMessage(QStringLiteral("RP-ассистент"),
                           QStringLiteral("Бар свёрнут. Открыть снова — из хаба (иконка в трее) или хоткеем."),
                           QSystemTrayIcon::Information, 2500);
        shownTrayHint_ = true;
    }
}

void OverlayWindow::showFromExternal()
{
    if (!isVisible())
        show();
    raise();
    reapplyTopmost();   // пере-утвердить HWND_TOPMOST (держаться поверх игры)
    // НЕ activateWindow(): бар — WS_EX_NOACTIVATE-окно поверх игры; показ НЕ должен красть фокус
    // клавиатуры. Иначе Qt автофокусит строку чата → курсор «залипает» в вводе, а окно остаётся
    // foreground без нормального выхода (юзер мог закрыть режим лишь через настройки). Набор
    // включается кликом в поле (см. eventFilter askInput_). Ниже — страховка от залипшего режима.
    setInputActivatable(false);
    if (askInput_) askInput_->clearFocus();
}

void OverlayWindow::setUpdater(Updater *u)
{
    updater_ = u;
    if (!updater_)
        return;
    // Обновление загружено и проверено → применится при следующем запуске.
    connect(updater_, &Updater::readyToApply, this, [this](const QString &ver) {
        showTransientStatus(
            QStringLiteral("Обновление %1 готово — применится при следующем запуске.").arg(ver), 6000);
        if (tray_)
            tray_->showMessage(QStringLiteral("RP Overlay"),
                QStringLiteral("Обновление %1 загружено. Перезапустите оверлей, чтобы применить.").arg(ver));
    });
}

// ---------------------------------------------------------------------------
// Auth / серверы
// ---------------------------------------------------------------------------
void OverlayWindow::onAuthorized()
{
    qInfo().noquote() << "[ui] authorized, tier=" << (auth_ ? auth_->tier() : QString());

    // Подтянуть список серверов для пикера в настройках (один раз, с защёлкой).
    if (advice_ && !serversRequested_) {
        serversRequested_ = true;
        advice_->requestServers();
    }
    // Своды «Обзвона» (проекты/сервера с правилами лидерской проверки) — для пикера в баре.
    // laws-билд «Обзвон» не имеет (тир laws → /leadership/servers даёт 403), запрос не шлём.
#ifndef RPOVERLAY_LAWS
    if (advice_ && !ldrServersRequested_) {
        ldrServersRequested_ = true;
        advice_->requestLeadershipServers();
    }
#endif
    // Приветствие — ТОЛЬКО при первом входе: authorized эмитится и на каждый тихий refresh
    // токена, и 6-секундная подсказка посреди игры прятала бы строку ввода (FocusOut —
    // набираемый вопрос обрывался).
    if (!greetedOnce_) {
        greetedOnce_ = true;
        // Подсказка — по РЕЖИМУ запуска (персист defaultMode мог вернуть сценовый): PTT и
        // строка ввода существуют только в Q&A/«Поиске» — в сценовых инструкция врала бы.
        if (currentMode() == QLatin1String("qa") || currentMode() == QLatin1String("laws"))
            showTransientStatus(
                QStringLiteral("Готово. Зажми «%1» и говори — или щёлкни в строку и спроси текстом.")
                    .arg(settings::window().hkPushToTalk), 6000);
        else
            showTransientStatus(
                QStringLiteral("Готово. «%1» или ▶ — слушать сцену.")
                    .arg(settings::window().hkLocalRecord), 6000);
    }
}

void OverlayWindow::onAuthError(const QString &message)
{
    showTransientStatus(QStringLiteral("Ошибка входа: %1").arg(message));
}

void OverlayWindow::onServersReady(const QVariantList &projects)
{
    // Кэшируем список (пикер проект/сервер живёт в Настройках → «Сервер / RP»).
    projectsData_ = projects;
    if (selectedServerId_.isEmpty())
        selectedServerId_ = cfg().serverId;
    // Флаг search приехал вместе со списком → применяем гейт беты поиска (демоут laws→qa,
    // видимость пилюли в QA-варианте).
    updateSearchModeGate();
}

// Есть ли у сервера флаг search: каталог законки построен И тир юзера допущен (сервер сам
// объединяет оба условия). Данные — из кэша /v1/overlay/servers; до его прихода — false
// (пункт меню «Поиск» не показываем, персист-режим демоутнется по факту данных).
bool OverlayWindow::serverHasSearch(const QString &serverId) const
{
    for (const QVariant &p : projectsData_) {
        const QVariantList servers = p.toMap().value(QStringLiteral("servers")).toList();
        for (const QVariant &sv : servers) {
            const QVariantMap sm = sv.toMap();
            if (sm.value(QStringLiteral("server_id")).toString() == serverId)
                return sm.value(QStringLiteral("search")).toBool();
        }
    }
    return false;
}

// Динамический гейт беты поиска (ср. как rag-флаг помечает «✓ свод» в пикере, но жёстче):
// сервер сменили / флаг сняли — выходим из «Поиска» в Q&A; в публичном QA-варианте флаг
// дополнительно управляет видимостью пилюли режимов (там «Поиск» — единственный второй режим).
void OverlayWindow::updateSearchModeGate()
{
#ifdef RPOVERLAY_LAWS
    // laws-билд: «Поиск» — единственный и безусловный режим (доступ даёт сам тир laws-ключа).
    // Пилюля режимов не нужна, демоута в Q&A нет (Q&A в этом билде не существует).
    if (modePill_) modePill_->setVisible(false);
    return;
#else
    const QString server = selectedServerId_.isEmpty() ? cfg().serverId : selectedServerId_;
    const bool has = serverHasSearch(server);
#ifdef RPOVERLAY_QA
    // Публичный QA: поиск ВЫРЕЗАН на компиляции → пилюля режимов не нужна (единственный режим Q&A).
    // Держим скрытой независимо от серверного флага search (раньше show по search=true = утечка поиска).
    if (modePill_) modePill_->setVisible(false);
#endif
    if (!has && !projectsData_.isEmpty() && currentMode() == QLatin1String("laws"))
        setMode(QStringLiteral("qa"));
#endif
}

void OverlayWindow::onLeadershipServersReady(const QVariantList &projects)
{
    // Кэш проектов/серверов свода «Обзвона» (для пилюли-пикера). Обновляем отображаемые имена.
    leadershipProjects_ = projects;
    updateLdrPill();
}

// Видимость пилюли свода «Обзвона» (только в режиме leadership) + текст «Проект» / «Проект · Сервер»
// по текущему выбору (settings::leadership()); имена — из кэша /leadership/servers.
void OverlayWindow::updateLdrPill()
{
    if (!ldrPill_) return;
    ldrPill_->setVisible(currentMode() == QLatin1String("leadership"));

    const LeadershipSettings &ldr = settings::leadership();
    QString projName, srvName;
    for (const QVariant &p : leadershipProjects_) {
        const QVariantMap pm = p.toMap();
        if (pm.value(QStringLiteral("project_id")).toString() != ldr.projectId)
            continue;
        projName = pm.value(QStringLiteral("display_name")).toString();
        for (const QVariant &sv : pm.value(QStringLiteral("servers")).toList()) {
            const QVariantMap sm = sv.toMap();
            if (sm.value(QStringLiteral("server_id")).toString() == ldr.serverId) {
                srvName = sm.value(QStringLiteral("display_name")).toString();
                break;
            }
        }
        break;
    }
    if (projName.isEmpty()) {
        // Ничего не выбрано / кэш не пришёл — показываем первый проект (обычно Majestic) или «Свод».
        if (ldr.projectId.isEmpty() && !leadershipProjects_.isEmpty())
            projName = leadershipProjects_.first().toMap().value(QStringLiteral("display_name")).toString();
        if (projName.isEmpty())
            projName = QStringLiteral("Свод");
    }
    ldrPill_->setText(srvName.isEmpty() ? projName : QStringLiteral("%1 · %2").arg(projName, srvName));
}

void OverlayWindow::onServersError(const QString &msg)
{
    // Сбрасываем защёлку — список можно будет запросить повторно (следующая авторизация).
    serversRequested_ = false;
    showTransientStatus(QStringLiteral("Не удалось получить список серверов: %1").arg(msg));
}

void OverlayWindow::onServerSettingsChanged()
{
    const ServerSettings &srv = settings::server();
    const QString newId = srv.serverId.isEmpty() ? cfg().serverId : srv.serverId;
    if (newId != selectedServerId_) {
        selectedServerId_ = newId;
        // Чистая память диалога на каждый сервер: rolling-контекст Q&A (history_limit)
        // не должен переносить follow-up'ы между сводами законов разных серверов.
        sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        // Новый сервер может не иметь беты поиска — гейт (демоут laws→qa, пилюля QA-варианта).
        updateSearchModeGate();
    }
    // Диалог сменил «режим при запуске» — применяем сразу (setMode сам остановит активности
    // уходящего режима и перерисует бар; повторный персист того же значения он не делает).
#ifndef RPOVERLAY_QA
    if (!srv.defaultMode.isEmpty() && srv.defaultMode != mode_)
        setMode(srv.defaultMode);
#endif
    showTransientStatus(QStringLiteral("Настройки сохранены."));
}

// ---------------------------------------------------------------------------
// Q&A: вопрос (голосом или текстом) → POST /v1/overlay/qa → HTML-ответ в панель.
// ---------------------------------------------------------------------------
void OverlayWindow::askQuestion(const QString &q)
{
    const QString question = q.trimmed();
    if (question.isEmpty())
        return;
    if (state_ == BarState::Thinking) {
        // Предыдущий Q&A-запрос ещё в полёте (см. guard в startPtt): новый requestQa отменил
        // бы его reply — почти готовый ответ потерялся бы. Просим подождать.
        showTransientStatus(QStringLiteral("Подожди — отвечаю на предыдущий вопрос…"));
        return;
    }
    if (auth_ && !auth_->isAuthorized()) {
        // После логаута глобальный hkToggle всё ещё может показать бар — текстовый вопрос
        // ушёл бы с пустым Bearer и вернулся «Ошибка: HTTP 401» (PTT-путь уже отбит).
        showTransientStatus(QStringLiteral("Сначала активируйте ключ."));
        return;
    }
    if (!advice_) {
        showTransientStatus(QStringLiteral("Внутренняя ошибка: нет AdviceClient."));
        return;
    }

    // Шапка панели: вопрос серым, elided (полный текст — в tooltip).
    if (qLabel_) {
        const QFontMetrics fm(qLabel_->font());
        qLabel_->setText(fm.elidedText(question, Qt::ElideRight, kBarWidth - 150));
        qLabel_->setToolTip(question);
    }
    clearAdviceExtras();   // статьи/чек-лист прошлого совета не относятся к новому вопросу
    if (answerView_)  answerView_->setPlainText(QString::fromUtf8(kPhQaThinking));
    if (answerPanel_) answerPanel_->setVisible(true);

    const QString server = selectedServerId_.isEmpty() ? cfg().serverId : selectedServerId_;
    qInfo().noquote() << "[ui] requestQa server=" << server << " q=" << question.left(120);
    advice_->requestQa(question, server, sessionId_);
    enterState(BarState::Thinking);
    // Failsafe — ВТОРАЯ линия: у запроса теперь свой клиентский transferTimeout (105с, AdviceClient),
    // дающий понятную ошибку в панель. Окно failsafe чуть длиннее (100с), чтобы эта ошибка успела
    // прийти ДО косметического сброса; если связи нет совсем — failsafe всё равно разблокирует ввод.
    if (failsafeTimer_) failsafeTimer_->start(100 * 1000);
}

void OverlayWindow::onQaAnswer(const QString &answer)
{
    // Guard владения этапом (по образцу onAdviceDone): qaAnswer ждём ТОЛЬКО в Thinking.
    // setMode/onLogout теперь отменяют Q&A-reply (cancelQa), но после failsafe Thinking он
    // ЖИВ — запоздавший ответ не должен гасить ЧУЖОЙ failsafe (Recognizing нового вопроса /
    // SceneThinking разбора сцены) и дёргать enterState: при «молчащем» сценовом прогоне
    // localBusy_ застрял бы true навсегда (авто-советы и hkLocalAnswer мертвы до перезапуска).
    if (state_ != BarState::Thinking) {
        // Поздний ответ в спокойном Idle (после failsafe) юзеру всё ещё полезен — показываем
        // display-only, не трогая failsafe/состояние. В остальных этапах чужую панель
        // (новый вопрос / совет сцены) не перетираем — отбрасываем.
        if (state_ == BarState::Idle) {
            if (answerView_)  answerView_->setHtml(answer);
            if (answerPanel_) answerPanel_->setVisible(true);
            update();   // слоёное окно: показать поздний ответ без ожидания move
        }
        return;
    }
    if (failsafeTimer_) failsafeTimer_->stop();   // ответ пришёл — страховка Thinking не нужна
    // Ответ бота — лёгкий HTML; рендерим как rich text. Панель висит до следующего вопроса.
    if (answerView_)  answerView_->setHtml(answer);
    if (answerPanel_) answerPanel_->setVisible(true);
    update();   // слоёное окно: форс-перерисовка ответа (иначе текст «залипает» до move)
    // Восстанавливаем ФАКТИЧЕСКОЕ состояние, а не безусловный Idle: если юзер уже снова
    // держит PTT/идёт распознавание, статус «🔴 Слушаю…» нельзя затирать строкой ввода
    // (micDot остался бы красным при странице ввода — противоречивая картинка).
    enterState(restingState());
}

// Общий обработчик ошибок AdviceClient: Q&A-запрос (бывш. onQaError — поведение для этого
// пути ПРЕЖНЕЕ) и SSE-совет сцены. Источник не важен: оба пути — «этап в полёте» со своей
// взведённой страховкой, и реакция одна: failsafe stop + ошибка в панель + состояние назад.
void OverlayWindow::onAdviceError(const QString &msg)
{
    qWarning().noquote() << "[ui] advice/qa error:" << msg;
    // Guard владения этапом (по образцу onAdviceDone): «боевая» ветка — только когда мы
    // действительно ждём ответ ЭТОГО клиента: Thinking (Q&A-запрос) или SSE-под-этап
    // SceneThinking (localBusy_=false — локальный прогон отдан, стримится совет). Всё прочее —
    // запоздавшая ошибка пережитого этапа (failsafe уже сработал / юзер запустил новый разбор
    // поверх стрима): его failsafe/панель/состояние принадлежат уже НЕ ей — только лог.
    if (localBusy_ || (state_ != BarState::Thinking && state_ != BarState::SceneThinking)) {
        if (localBusy_)
            awaitingFirstDelta_ = false;   // пережитый стрим мёртв — его анти-мигание не понадобится
        return;
    }
    if (failsafeTimer_) failsafeTimer_->stop();
    awaitingFirstDelta_ = false;   // сценовый совет: старое содержимое панели остаётся видимым
    // Не оставляем висеть «Думаю…» в панели — заменяем текстом ошибки.
    if (answerView_ && answerPanel_ && answerPanel_->isVisible())
        answerView_->setPlainText(QStringLiteral("Ошибка: %1").arg(msg));
    // Как в onQaAnswer: возвращаемся к фактическому этапу, а не безусловно к Idle.
    enterState(restingState());
    showTransientStatus(QStringLiteral("Ошибка: %1").arg(msg));
}

// ---------------------------------------------------------------------------
// Режим «Поиск» (laws): запрос (голос/текст) → POST /v1/overlay/laws/search → топ-3
// карточки в панель, related-коды в chips. Поиск каталожный и быстрый — свои таймауты
// (12с клиент / 15с failsafe), НЕ 100-секундные Q&A. Ошибки — отдельным сигналом.
// ---------------------------------------------------------------------------
void OverlayWindow::startLawsSearch(const QString &q)
{
    const QString query = q.trimmed();
    if (query.isEmpty())
        return;
    if (state_ == BarState::Thinking) {
        // Этап в полёте (см. guard в startPtt): новый requestSearch отменил бы предыдущий
        // reply — почти готовую выдачу/ответ. Просим подождать (симметрично askQuestion).
        showTransientStatus(QStringLiteral("Подожди — ищу по предыдущему запросу…"));
        return;
    }
    if (auth_ && !auth_->isAuthorized()) {
        showTransientStatus(QStringLiteral("Сначала активируйте ключ."));
        return;
    }
    if (!laws_) {
        showTransientStatus(QStringLiteral("Внутренняя ошибка: нет LawsClient."));
        return;
    }

    // Шапка панели: запрос серым, elided (полный текст — в tooltip).
    if (qLabel_) {
        const QFontMetrics fm(qLabel_->font());
        qLabel_->setText(fm.elidedText(QStringLiteral("Поиск: %1").arg(query),
                                       Qt::ElideRight, kBarWidth - 150));
        qLabel_->setToolTip(query);
    }
    clearAdviceExtras();   // chips/чек-лист прошлого этапа к новой выдаче не относятся
    if (answerView_)  answerView_->setPlainText(QString::fromUtf8(kPhLawsSearching));
    if (answerPanel_) answerPanel_->setVisible(true);

    const QString server = selectedServerId_.isEmpty() ? cfg().serverId : selectedServerId_;
    qInfo().noquote() << "[ui] lawsSearch server=" << server << " q=" << query.left(120);
    laws_->requestSearch(query, server, /*limit*/8);
    enterState(BarState::Thinking);
    // Failsafe — ВТОРАЯ линия: у запроса клиентский transferTimeout 12с (LawsClient) с внятной
    // ошибкой в панель; окно failsafe чуть длиннее, чтобы она успела прийти ДО сброса.
    if (failsafeTimer_) failsafeTimer_->start(15 * 1000);
}

void OverlayWindow::onLawsResults(const QString &query, const QVariantList &results,
                                  const QVariantList &related, bool degraded)
{
    Q_UNUSED(query);
    // Смена режима рубит запрос (cancelAll в setMode) — этот guard лишь бэкстоп для выдачи,
    // успевшей встать в очередь событий: в чужом режиме её не рендерим.
    if (currentMode() != QLatin1String("laws"))
        return;
    // Владение этапом — по образцу onQaAnswer: «боевая» ветка только в Thinking; поздняя
    // выдача в спокойном Idle (после failsafe) полезна — показываем display-only; в прочих
    // этапах (новый PTT-цикл) чужую панель/страховку не трогаем.
    const bool own = (state_ == BarState::Thinking);
    if (!own && state_ != BarState::Idle)
        return;
    if (own && failsafeTimer_) failsafeTimer_->stop();

    if (answerView_)  answerView_->setHtml(lawsResultsHtml(results, degraded));
    if (answerPanel_) answerPanel_->setVisible(true);
    // «Ещё релевантное» — кодами в строку над текстом (как «ст. 17 · ст. 19 ч.2» у советов).
    if (chipsLabel_) {
        QStringList parts;
        for (const QVariant &v : related) {
            const QString code = v.toMap().value(QStringLiteral("code")).toString();
            if (!code.isEmpty())
                parts << QStringLiteral("ст. %1").arg(code);
        }
        chipsLabel_->setText(parts.join(QStringLiteral(" · ")));
        chipsLabel_->setVisible(!parts.isEmpty());
        updateAnswerViewCap();   // строка заняла место в панели — поджать бюджет answerView_
    }
    update();   // слоёное окно: форс-перерисовка выдачи поиска (иначе «залипает» до move)
    if (own)
        enterState(restingState());
}

void OverlayWindow::onLawsError(const QString &code, const QString &message)
{
    qWarning().noquote() << "[ui] laws error:" << code << message;
    // Владение этапом (по образцу onAdviceError): ошибку ждём только в Thinking режима
    // «Поиск» — запоздавшая ошибка пережитого этапа не должна гасить чужой failsafe.
    if (currentMode() != QLatin1String("laws") || state_ != BarState::Thinking)
        return;
    if (failsafeTimer_) failsafeTimer_->stop();
    if (answerView_ && answerPanel_ && answerPanel_->isVisible())
        answerView_->setPlainText(QStringLiteral("Поиск: %1").arg(message));
    enterState(restingState());
    showTransientStatus(QStringLiteral("Поиск: %1").arg(message));
}

// ---------------------------------------------------------------------------
// Режимы: Q&A/Поиск (ввод/PTT) ↔ сценовые Адвокат/Прокурор/Общий (непрерывный захват + советы).
// ---------------------------------------------------------------------------
// Смена режима: остановить активности уходящего (фаза не должна «переехать» в новый),
// персистнуть выбор и перерисовать бар. Панель ответа НЕ чистим — прошлый ответ полезен.
void OverlayWindow::setMode(const QString &mode)
{
    QString nm = mode;
#if defined(RPOVERLAY_LAWS)
    // laws-билд: единственный режим «Поиск» — любой другой запрос режима форсится в laws.
    nm = QStringLiteral("laws");
#elif defined(RPOVERLAY_QA)
    // Публичный QA-вариант: ЕДИНСТВЕННЫЙ режим Q&A — поиск (laws) вырезан на компиляции,
    // любой запрос режима форсится в qa (роли/сцена тоже недоступны).
    nm = QStringLiteral("qa");
#else
    if (nm != QLatin1String("advocate") && nm != QLatin1String("prosecutor")
        && nm != QLatin1String("general") && nm != QLatin1String("leadership")
        && nm != QLatin1String("laws"))
        nm = QStringLiteral("qa");
#endif
#ifndef RPOVERLAY_LAWS
    // Гейт беты поиска: «Поиск» существует только на серверах с search=true. Пока /servers
    // не пришёл (projectsData_ пуст) — не решаем; демоут по факту сделает updateSearchModeGate.
    // (в laws-билде демоута нет — Q&A там не существует, доступ гарантирует тир ключа)
    if (nm == QLatin1String("laws") && !projectsData_.isEmpty()
        && !serverHasSearch(selectedServerId_.isEmpty() ? cfg().serverId : selectedServerId_))
        nm = QStringLiteral("qa");
#endif
    if (nm == mode_)
        return;

    // Сценовый захват уходящего режима — стоп (кнопку перерисует applyModeUi ниже).
    if (sceneListening_) {
        sceneListening_ = false;
        if (audio_ && audio_->isRunning() && !pttActive_) audio_->stop();
        if (endpointer_) endpointer_->resetState();
    }
    // PTT гейтится Q&A, но клавиша могла быть зажата прямо в момент выбора в меню.
    if (pttActive_) {
        pttActive_ = false;
        if (audio_) audio_->stop();
    }
    // Сбросить фазу целиком: запоздавшие resultReady отсекутся по поколению (sttGen_/sceneGen_=0),
    // SSE-стрим и чек-лист уходящего режима глушим (cancelAdvice), Q&A-запрос — тоже (cancelQa):
    // его поздний ответ/ошибка иначе гасили бы failsafe уже НОВОГО режима.
    awaitingStt_ = false;
    localBusy_ = false;
    sttGen_ = 0;
    sceneGen_ = 0;
    awaitingFirstDelta_ = false;
    if (failsafeTimer_) failsafeTimer_->stop();
    if (advice_) {
        advice_->cancelAdvice();
        advice_->cancelQa();
    }
    if (laws_) laws_->cancelAll();   // поиск уходящего режима — под корень (симметрично cancelQa)

    // Если этап был в полёте, в панели остался прогресс-плейсхолдер («Ищу статьи…»/«Думаю…») —
    // теперь он мёртв навсегда (запросы выше отменены, ответ не придёт). Заменяем нейтральным
    // текстом; реальный контент (прошлый ответ/выдача) не трогаем — он полезен и в новом режиме.
    if (answerView_) {
        const QString cur = answerView_->toPlainText();
        if (cur == QString::fromUtf8(kPhQaThinking) || cur == QString::fromUtf8(kPhLawsSearching)
            || cur == QString::fromUtf8(kPhAdviceThinking))
            answerView_->setPlainText(QStringLiteral("Запрос отменён — режим сменился."));
    }

    mode_ = nm;
    ServerSettings s = settings::server();
    if (s.defaultMode != mode_) {
        s.defaultMode = mode_;
        settings::setServer(s);   // режим переживает перезапуск (как в ≤0.2.20)
    }
    qInfo().noquote() << "[ui] режим:" << mode_;
    applyModeUi();
}

// Перерисовать бар под mode_: пилюля, кнопка сцены, страница центра. Q&A — строка ввода;
// сценовые — страница-статус ПОСТОЯННО (текстовый ввод недоступен — паритет со старой версией).
void OverlayWindow::applyModeUi()
{
    const QString cm = currentMode();
    // Режимы со строкой ввода: Q&A и «Поиск» (laws). Прочие — страница-статус/сцена.
    const bool input = (cm == QLatin1String("qa") || cm == QLatin1String("laws"));
    // «Обзвон» — PTT-режим (зажми → слушать вопрос собеседника), а не непрерывная сцена: кнопку ▶ прячем.
    const bool ldr = (cm == QLatin1String("leadership"));
    if (modePill_) modePill_->setText(modeTitle(cm));
    updateLdrPill();   // пилюля свода «Обзвона» видна только в leadership
    if (askInput_)
        askInput_->setPlaceholderText(cm == QLatin1String("laws")
            ? QStringLiteral("Найти статью или ситуацию… (Enter)")
            : QStringLiteral("Спросить… (Enter — отправить)"));
    if (listenBtn_) listenBtn_->setVisible(!input && !ldr);
    updateListenBtn();
    enterState(restingState());
}

void OverlayWindow::updateListenBtn()
{
    if (!listenBtn_) return;
    listenBtn_->setText(sceneListening_ ? QStringLiteral("⏸") : QStringLiteral("▶"));
    listenBtn_->setToolTip(sceneListening_
        ? QStringLiteral("Пауза прослушивания сцены («%1»)").arg(settings::window().hkLocalRecord)
        : QStringLiteral("Слушать сцену («%1»)").arg(settings::window().hkLocalRecord));
    setUiState(listenBtn_, sceneListening_ ? "on" : "off");
}

// Клик по пилюле — меню из 4 режимов с отметкой на текущем.
void OverlayWindow::onModePillClicked()
{
    if (!modePill_) return;
#ifdef RPOVERLAY_LAWS
    return;   // laws-билд: режим один («Поиск»), пилюля скрыта — меню не существует
#elif defined(RPOVERLAY_QA)
    return;   // публичный QA: единственный режим Q&A, поиск вырезан на компиляции — меню не нужно
#else
    // «Поиск» (бета) показываем ТОЛЬКО на серверах с search=true (динамический гейт из
    // /servers — как метка «✓ свод» у rag-серверов в пикере, но жёсткий).
    const bool hasSearch =
        serverHasSearch(selectedServerId_.isEmpty() ? cfg().serverId : selectedServerId_);
    static const struct { const char *id; const char *title; } kModes[] = {
        {"qa",         "Q&A"},
        {"laws",       "Поиск"},
        {"advocate",   "Адвокат"},
        {"prosecutor", "Прокурор"},
        {"general",    "Общий"},
        {"leadership", "Обзвон"},
    };
    QMenu menu(this);
    for (const auto &m : kModes) {
        if (!hasSearch && qstrcmp(m.id, "laws") == 0)
            continue;   // бета закрыта для сервера — пункта «Поиск» нет
        QAction *act = menu.addAction(QString::fromUtf8(m.title));
        act->setCheckable(true);
        act->setChecked(mode_ == QLatin1String(m.id));
        const QString id = QString::fromLatin1(m.id);
        connect(act, &QAction::triggered, this, [this, id] { setMode(id); });
    }
    // Попап не активирует окно (и не отбирает фокус у игры); reapplyTopmost его не схлопнет —
    // там guard на activePopupWidget.
    menu.exec(modePill_->mapToGlobal(QPoint(0, modePill_->height() + 6)));
#endif
}

// Пикер свода «Обзвона»: проекты (Majestic — единый свод; GTA5RP — подменю серверов).
// Выбор → settings::leadership() + чистая rolling-память (как при смене законочного сервера).
void OverlayWindow::onLdrPillClicked()
{
    if (!ldrPill_) return;
    if (leadershipProjects_.isEmpty()) {
        showTransientStatus(QStringLiteral("Список сводов «Обзвона» ещё загружается…"));
        if (advice_ && auth_ && auth_->isAuthorized())
            advice_->requestLeadershipServers();   // повторный запрос (кэш ещё не пришёл)
        return;
    }
    const LeadershipSettings cur = settings::leadership();
    auto choose = [this](const QString &pid, const QString &sid) {
        LeadershipSettings l;
        l.projectId = pid;
        l.serverId  = sid;
        settings::setLeadership(l);
        sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        updateLdrPill();
        showTransientStatus(QStringLiteral("Свод «Обзвона» обновлён."));
    };
    QMenu menu(this);
    for (const QVariant &p : leadershipProjects_) {
        const QVariantMap pm = p.toMap();
        const QString pid = pm.value(QStringLiteral("project_id")).toString();
        const QString pname = pm.value(QStringLiteral("display_name")).toString();
        const QVariantList servers = pm.value(QStringLiteral("servers")).toList();
        if (servers.isEmpty()) {
            // Проект без выбора сервера (Majestic — единый свод на все серверы).
            QAction *act = menu.addAction(pname);
            act->setCheckable(true);
            act->setChecked(cur.projectId == pid);
            connect(act, &QAction::triggered, this, [choose, pid] { choose(pid, QString()); });
        } else {
            QMenu *sub = menu.addMenu(pname);
            for (const QVariant &sv : servers) {
                const QVariantMap sm = sv.toMap();
                const QString sid = sm.value(QStringLiteral("server_id")).toString();
                const QString sname = sm.value(QStringLiteral("display_name")).toString();
                QAction *act = sub->addAction(sname);
                act->setCheckable(true);
                act->setChecked(cur.projectId == pid && cur.serverId == sid);
                connect(act, &QAction::triggered, this, [choose, pid, sid] { choose(pid, sid); });
            }
        }
    }
    menu.exec(ldrPill_->mapToGlobal(QPoint(0, ldrPill_->height() + 6)));
}

// ---------------------------------------------------------------------------
// Сценовый захват: непрерывно пишем микрофон (Вы) + звук игры (собеседники) в кольца,
// VAD-эндпоинтер сам решает «собеседник договорил — пора разбирать». Рантайм-тумблер:
// по умолчанию ВЫКЛ на каждый запуск, не персистится (как в ≤0.2.20).
// ---------------------------------------------------------------------------
void OverlayWindow::toggleSceneListening()
{
    if (currentMode() == QLatin1String("qa") || currentMode() == QLatin1String("laws"))
        return;   // в Q&A/«Поиске» неприменимо (хоткей глобальный — глотаем тихо)
    if (currentMode() == QLatin1String("leadership")) {
        // «Обзвон» — PTT-режим: непрерывная сцена/VAD не используется (ложные срабатывания на паузе).
        showTransientStatus(QStringLiteral("«Обзвон»: зажми клавишу PTT, чтобы слушать вопрос."));
        return;
    }
    if (QApplication::activeModalWidget()) {
        // Открыт модальный диалог (Настройки): захватом владеет его VU-предпросмотр
        // (startPreview/stopPreview), а WM_HOTKEY летит в HWND оверлея МИМО Qt-модальности.
        // Включение сейчас оставило бы «Слушаю сцену…» с МЁРТВЫМ захватом после закрытия
        // диалога (stopPreview глушит общий AudioCapture); выключение глушило бы предпросмотр.
        // По образцу гейта в pollPtt.
        showTransientStatus(QStringLiteral("Сначала закройте настройки."));
        return;
    }

    if (!sceneListening_) {
        if (auth_ && !auth_->isAuthorized()) {
            showTransientStatus(QStringLiteral("Сначала активируйте ключ."));
            return;
        }
        // Разбор сцены требует локальную модель — гейт как у PTT.
        if (models_ && !models_->isReady()) {
            showTransientStatus(QStringLiteral(
                "Модель распознавания не скачана — докачайте в Настройках."));
            return;
        }
        if (pipelineFailed_) {
            showTransientStatus(QStringLiteral(
                "Локальное распознавание недоступно — разбор сцены не работает."));
            return;
        }
        ensureLocalPipeline();   // движок диаризации/STT для submitScene (грузится в фоне)
        ensureEndpointer();
        if (endpointer_) {
            // start идемпотентен (повтор → applyConfig); пороги — из пресета чувствительности.
            endpointer_->start(makeEndpointerConfig(resolveVadPath()));
            endpointer_->resetState();
        }
        // Чистые кольца: в окно разбора не должен попасть звук до включения.
        micRing_.clear();
        otherRing_.clear();
        if (audio_) audio_->start(/*micOnly*/false);   // микрофон + звук игры
        sceneListening_ = true;
        if (state_ != BarState::SceneThinking)   // разбор в полёте не затираем
            enterState(BarState::SceneListening);
        else
            updateMicDot();
    } else {
        sceneListening_ = false;
        if (audio_ && audio_->isRunning() && !pttActive_) audio_->stop();
        if (endpointer_) endpointer_->resetState();
        if (state_ != BarState::SceneThinking)
            enterState(BarState::SceneIdle);
        showTransientStatus(QStringLiteral("Сцена на паузе…"));
    }
    updateListenBtn();
}

// Лениво создать VAD-эндпоинтер + коннект авто-триггера (queued: fireRequested эмитится
// с worker-потока эндпоинтера).
void OverlayWindow::ensureEndpointer()
{
    if (endpointer_) return;
    endpointer_ = new TurnEndpointer(this);
    connect(endpointer_, &TurnEndpointer::fireRequested, this, [this] {
        if (currentMode() == QLatin1String("qa")) return;   // двойная защита границы
        if (sceneListening_ && !localBusy_)
            triggerSceneAnalysis();
    });
    connect(endpointer_, &TurnEndpointer::debugState, this,
            [](const QString &line) { qInfo().noquote() << line; });
}

// Разобрать сцену СЕЙЧАС: snapshot обоих колец на окно localWindowSec → submitScene.
// Общее для авто-триггера (эндпоинтер) и ручного хоткея hkLocalAnswer; ручной работает
// и на паузе — в кольцах остаётся звук до останова захвата.
void OverlayWindow::triggerSceneAnalysis()
{
    if (currentMode() == QLatin1String("qa") || currentMode() == QLatin1String("laws"))
        return;   // глобальный хоткей в Q&A/«Поиске» — глотаем тихо (сцены в них нет)
    if (QApplication::activeModalWidget()) {
        // Модальный диалог (Настройки) владеет захватом — WM_HOTKEY проходит мимо
        // Qt-модальности (см. toggleSceneListening), а снапшот сейчас взял бы чужой/пустой буфер.
        showTransientStatus(QStringLiteral("Сначала закройте настройки."));
        return;
    }
    if (localBusy_) {
        showTransientStatus(QStringLiteral("Уже разбираю сцену…"));
        return;
    }
    if (!pipeline_ || !pipeline_->isReady()) {
        showTransientStatus(QStringLiteral("Локальный движок ещё не готов."));
        return;
    }
    // Ручной вызов идёт мимо гейта — отмечаем fire, чтобы кулдаун/дельта эндпоинтера
    // отсчитывались от него (для авто-пути noteFired избыточен, но безвреден).
    if (endpointer_) endpointer_->noteFired();

    const int sr = 16000;
    const int win = settings::window().localWindowSec * sr;
    std::vector<float> mic, other;
    micRing_.snapshot(mic, win);
    otherRing_.snapshot(other, win);

    // Сцена по КАНАЛАМ: микрофон = «Вы», звук игры (other) = собеседники. Выравниваем оба
    // канала по ДЛИНЕ (left-pad тишиной) — общий таймлайн для хронологии. Разделение по
    // каналу надёжнее микса: тебя не слепит с собеседником, даже если голоса похожи
    // (выравниваем по КОНЦУ — последние сэмплы синхронны по wall-clock, оба источника
    // пишутся одним каденсом ~100мс кадрами).
    const size_t len = std::max(mic.size(), other.size());
    if (len == 0) {
        showTransientStatus(QStringLiteral("Буфер пуст — нечего разбирать."));
        return;
    }
    std::vector<float> micPad(len, 0.0f), otherPad(len, 0.0f);
    for (size_t i = 0; i < mic.size(); ++i)   micPad[len - mic.size() + i]     = mic[i];
    for (size_t i = 0; i < other.size(); ++i) otherPad[len - other.size() + i] = other[i];

    lastOtherWindow_ = otherPad;   // фильтр энергии сегментов в onLocalResult (выкинуть тихий фон)
    lastWindowSr_ = sr;

    speakerLabels_.reset();        // новая сцена — метки A/B/C переназначаются
    pipeline_->setClusterThreshold(static_cast<float>(settings::localDiarThreshold()));  // live-порог
    localBusy_ = true;
    sceneGen_ = ++pipelineGen_;    // тег задания — onLocalResult примет только ЕГО результат
    pipeline_->submitScene(micPad, otherPad, sceneGen_);
    qInfo().noquote() << "[ui] разбор сцены:" << static_cast<int>(len / sr) << "с";
    enterState(BarState::SceneThinking);
    // Failsafe: resultReady может не прийти никогда (движок умер после init) — не залипаем
    // в «💭 Анализирую сцену…»; запоздавший результат отсечётся по поколению (sceneGen_).
    if (failsafeTimer_) failsafeTimer_->start(60 * 1000);
}

// ---------------------------------------------------------------------------
// Совет по сцене: SSE-стрим (sources → delta… → done) + чек-лист у процедурных ролей.
// ---------------------------------------------------------------------------
void OverlayWindow::requestAdviceFor(const QString &situation)
{
    if (!advice_ || situation.trimmed().isEmpty())
        return;
    const QString mode = currentMode();
    qInfo().noquote() << "[ui] requestAdvice mode=" << mode
                      << " situation=" << situation.left(160);

    // Шапка панели — что это совет и для какой роли; сам текст сцены — в tooltip.
    if (qLabel_) {
        qLabel_->setText(QStringLiteral("Совет (%1)").arg(modeTitle(mode)));
        qLabel_->setToolTip(situation);
    }
    clearAdviceExtras();   // статьи/чек-лист прошлого совета — к новой сцене не относятся
    if (answerView_)  answerView_->setPlainText(QString::fromUtf8(kPhAdviceThinking));
    if (answerPanel_) answerPanel_->setVisible(true);
    awaitingFirstDelta_ = true;   // первый delta очистит заглушку (анти-мигание)

    // RAG-совет/чек-лист работают на серверах со сводом (в пикере «✓ свод»). Шлём ВЫБРАННЫЙ
    // сервер; если свода нет — сервер вернёт нейтральный ответ без норм.
    // «Обзвон» ретривит СВОЙ свод правил (проект/сервер из отдельного пикера), не законку.
    QString server;
    QString projectId;
    if (mode == QLatin1String("leadership")) {
        const LeadershipSettings &ldr = settings::leadership();
        server    = ldr.serverId;   // свод сервера (пусто → сервер фолбэчит по проекту)
        projectId = ldr.projectId;  // пусто → единый свод Majestic
    } else {
        server = selectedServerId_.isEmpty() ? cfg().serverId : selectedServerId_;
    }
    advice_->requestAdvice(situation, mode, /*deep*/false, sessionId_, server, projectId);
    // Чек-лист — только для процедурных ролей (адвокат/прокурор), не для general/leadership (паритет).
    if (mode == QLatin1String("advocate") || mode == QLatin1String("prosecutor"))
        advice_->requestChecklist(situation, mode, sessionId_, server);

    enterState(BarState::SceneThinking);
    // Failsafe — ВТОРАЯ линия: SSE теперь рвётся клиентским idle-таймером (95с, AdviceClient) с
    // adviceError в панель. Окно failsafe чуть длиннее (100с), чтобы ошибка успела прийти ДО сброса.
    if (failsafeTimer_) failsafeTimer_->start(100 * 1000);
}

// SSE sources: статьи-источники → строка «ст. 17 · ст. 19 ч.2» над текстом совета.
void OverlayWindow::onAdviceSources(const QStringList &articles, int chunksUsed)
{
    Q_UNUSED(chunksUsed);
    if (!chipsLabel_) return;
    QStringList parts;
    parts.reserve(articles.size());
    for (const QString &a : articles)
        parts << QStringLiteral("ст. %1").arg(a);
    chipsLabel_->setText(parts.join(QStringLiteral(" · ")));
    chipsLabel_->setVisible(!parts.isEmpty());
    updateAnswerViewCap();   // строка статей заняла место в панели — поджать бюджет answerView_
}

// SSE delta: плейн-аппенд токена (совет — обычный текст, НЕ HTML) + автоскролл.
void OverlayWindow::onAdviceDelta(const QString &text)
{
    if (!answerView_) return;
    if (awaitingFirstDelta_) {   // пошёл новый совет — теперь чистим заглушку (анти-мигание)
        awaitingFirstDelta_ = false;
        answerView_->clear();
    }
    QTextCursor c(answerView_->document());
    c.movePosition(QTextCursor::End);
    c.insertText(text);  // QTextCursor вставляет плейн-текст (insertPlainText есть только у viewport-классов)
    if (auto *sb = answerView_->verticalScrollBar())
        sb->setValue(sb->maximum());
    update();   // слоёное окно: показать дописанный токен совета без ожидания move
    // Панель НЕ ре-показываем: закрытие пользователем (✕/hkCollapse) уважается до СЛЕДУЮЩЕГО
    // вопроса/совета (requestAdviceFor/askQuestion сами делают setVisible(true)) — иначе каждый
    // delta снова открывал бы панель и закрыть стрим было бы нельзя. Аппендим в скрытую молча.
}

void OverlayWindow::onAdviceDone()
{
    qInfo().noquote() << "[ui] advice done";
    if (state_ != BarState::SceneThinking || localBusy_) {
        // Не наш этап: failsafe уже сработал / режим сменили (стрим отменён) / юзер успел
        // запустить НОВЫЙ разбор сцены (localBusy_) — его failsafe гасить нельзя.
        awaitingFirstDelta_ = false;
        return;
    }
    if (failsafeTimer_) failsafeTimer_->stop();
    if (awaitingFirstDelta_) {
        // done без единого delta (пустой совет) — не оставляем «Думаю над советом…» навсегда.
        awaitingFirstDelta_ = false;
        if (answerView_) answerView_->setPlainText(QStringLiteral("Совет не получен."));
    }
    update();   // слоёное окно: применить финальный текст совета без ожидания move
    enterState(restingState());   // SceneListening, если слушаем; иначе пауза
}

// Чек-лист процедуры: items — список map'ов {id, text, status, note};
// status: ok=зелёный ✓, violation=красный ✗, иначе серый ?.
void OverlayWindow::onChecklistReady(const QVariantList &items)
{
    qInfo().noquote() << "[ui] checklist items=" << items.size();
    if (!checklistLabel_) return;
    // Чек-лист существует только у процедурных ролей (паритет с ≤0.2.20: applyModeUi прятал
    // его в прочих режимах): запоздавший ответ (запрос отменяем, но онFinished мог уже
    // встать в очередь) не должен влезть ни под Q&A-ответ, ни в «Общий».
    if (currentMode() != QLatin1String("advocate")
        && currentMode() != QLatin1String("prosecutor"))
        return;

    QStringList lines;
    lines.reserve(items.size());
    for (const QVariant &v : items) {
        const QVariantMap m = v.toMap();
        const QString text   = m.value(QStringLiteral("text")).toString();
        const QString status = m.value(QStringLiteral("status")).toString();
        const QString note   = m.value(QStringLiteral("note")).toString();

        QString mark, color;
        if (status == QLatin1String("ok")) {
            mark  = QStringLiteral("✓");
            color = QStringLiteral("#5ECC3C");
        } else if (status == QLatin1String("violation")) {
            mark  = QStringLiteral("✗");
            color = QStringLiteral("#EA3323");
        } else {
            mark  = QStringLiteral("?");
            color = QStringLiteral("#9A9A9A");
        }
        QString line = QStringLiteral("<span style=\"color:%1\">%2</span> %3")
                           .arg(color, mark, text.toHtmlEscaped());
        if (!note.isEmpty())
            line += QStringLiteral(" <span style=\"color:#9A9A9A\">(%1)</span>")
                        .arg(note.toHtmlEscaped());
        lines << line;
    }
    checklistLabel_->setText(lines.join(QStringLiteral("<br>")));
    checklistLabel_->setVisible(!lines.isEmpty());   // пусто → скрыт (не занимаем панель)
    // Панель НЕ ре-показываем (как в onAdviceDelta): закрытие пользователем уважается до
    // следующего запроса — чек-лист дорисуется в скрытую панель и появится при её открытии.
    updateAnswerViewCap();   // чек-лист занял место в панели — поджать бюджет answerView_
    update();   // слоёное окно: применить дорисованный чек-лист без ожидания move
}

// Скрыть/очистить статьи и чек-лист (новый вопрос Q&A / новая сцена).
void OverlayWindow::clearAdviceExtras()
{
    if (chipsLabel_) {
        chipsLabel_->clear();
        chipsLabel_->setVisible(false);
    }
    if (checklistLabel_) {
        checklistLabel_->clear();
        checklistLabel_->setVisible(false);
    }
    updateAnswerViewCap();   // место освободилось — вернуть answerView_ полный бюджет
}

// ---------------------------------------------------------------------------
// Push-to-talk: зажал → микрофон пишется в micRing_; отпустил → весь буфер в локальный STT.
// Опрос GetAsyncKeyState (а не RegisterHotKey): поддержка кнопок мыши и детект ОТПУСКАНИЯ.
// ---------------------------------------------------------------------------
void OverlayWindow::pollPtt()
{
#ifdef Q_OS_WIN
    // Печать в строке вопроса и открытое модальное окно имеют ПРИОРИТЕТ над PTT:
    // GetAsyncKeyState глобален, и PTT-клавиша без модификаторов (Space/буква) иначе
    // срабатывала бы прямо во время набора текста (поле скрылось бы страницей-статусом,
    // вопрос оборвался). Модалка закрывает и захват нового PTT-биндинга в настройках
    // (ещё зажатая клавиша детектилась бы как свежий фронт → фантомная запись), и конфликт
    // владения захватом с VU-предпросмотром диалога.
    if ((askInput_ && askInput_->hasFocus()) || QApplication::activeModalWidget()) {
        if (pttActive_) stopPtt();
        pttPrevDown_ = false;
        return;
    }
    // PTT работает в режимах со строкой ввода — Q&A и «Поиск» (микрофон→запрос) — И в «Обзвоне»
    // (звук собеседника→вопрос интервьюера). В прочих сценовых режимах (адвокат/прокурор)
    // голосового PTT нет — там непрерывная сцена.
    const QString cmPtt = currentMode();
    const bool ldrPtt = (cmPtt == QLatin1String("leadership"));
    const bool inputPtt = (cmPtt == QLatin1String("qa") || cmPtt == QLatin1String("laws"));
    if (!inputPtt && !ldrPtt) {
        if (pttActive_) { if (scenePtt_) stopScenePtt(); else stopPtt(); }
        pttPrevDown_ = false;
        return;
    }
    if (pttVk_ == 0) return;
    auto kd = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };
    const bool ctrl = kd(VK_CONTROL), shift = kd(VK_SHIFT), alt = kd(VK_MENU), win = kd(VK_LWIN) || kd(VK_RWIN);
    // Модификаторы — на ТОЧНОЕ равенство (зажат ⇔ требуется), иначе ловили бы чужие комбинации.
    const bool modsOk = (ctrl  == ((pttMods_ & hk::Ctrl)  != 0))
                     && (shift == ((pttMods_ & hk::Shift) != 0))
                     && (alt   == ((pttMods_ & hk::Alt)   != 0))
                     && (win   == ((pttMods_ & hk::Win)   != 0));
    const bool down = kd(static_cast<int>(pttVk_)) && modsOk;
    if (down && !pttPrevDown_) {
        pttPrevDown_ = true;
        if (!pttActive_) {
            if (ldrPtt) startScenePtt();   // «Обзвон»: захват звука собеседника
            else        startPtt();        // Q&A: захват микрофона (guard'ы внутри)
        }
    } else if (!down && pttPrevDown_) {
        pttPrevDown_ = false;
        if (pttActive_) { if (scenePtt_) stopScenePtt(); else stopPtt(); }
    }
#endif
}

void OverlayWindow::startPtt()
{
    if (currentMode() != QLatin1String("qa") && currentMode() != QLatin1String("laws"))
        return;   // mic-PTT — только режимы со строкой ввода (тихо, без спама статусом)
    if (pttActive_ || awaitingStt_)
        return;   // уже пишем / ещё распознаём предыдущий вопрос
    if (state_ == BarState::Thinking) {
        // Ответ на предыдущий вопрос ещё в полёте: новый requestQa отменил бы его reply —
        // почти готовый ответ потерялся бы. Просим подождать (отмена в requestQa — бэкстоп
        // для пути «failsafe вернул Idle, юзер спросил снова»).
        showTransientStatus(QStringLiteral("Подожди — отвечаю на предыдущий вопрос…"));
        return;
    }
    if (auth_ && !auth_->isAuthorized()) {
        showTransientStatus(QStringLiteral("Сначала активируйте ключ."));
        return;
    }
    // Голос требует локальную модель; текстовый ввод работает и без неё.
    if (models_ && !models_->isReady()) {
        showTransientStatus(QStringLiteral(
            "Модель распознавания не скачана — спросите текстом или докачайте в Настройках."));
        return;
    }
    if (pipelineFailed_) {
        // Инициализация движка уже проваливалась (битая модель/OOM/заглушка без STT):
        // submit повис бы навсегда. Флаг сбросится после перекачки модели (modelsReady).
        showTransientStatus(QStringLiteral(
            "Локальное распознавание недоступно — спросите текстом."));
        return;
    }
    ensureLocalPipeline();   // лениво поднять движок (первая загрузка — секунды, в фоне)
    micRing_.clear();        // в буфер пойдёт только текущий вопрос
    utterancePeak_ = 0.0f;   // сброс пика (диагностика «тихого микрофона»)
    if (audio_) audio_->start(/*micOnly*/true);   // ТОЛЬКО микрофон — Q&A не слышит игру
    pttActive_ = true;
    enterState(BarState::Listening);
}

void OverlayWindow::stopPtt()
{
    if (!pttActive_) return;
    pttActive_ = false;
    if (audio_) audio_->stop();

    std::vector<float> mic;
    micRing_.snapshot(mic, 0);   // весь буфер = только что произнесённый вопрос
    if (!pipeline_) {
        // Движок успел умереть между startPtt и отпусканием (initialized(false) занулил
        // pipeline_) — submit некуда, не оставляем бар в Recognizing.
        enterState(BarState::Idle);
        showTransientStatus(QStringLiteral(
            "Локальное распознавание недоступно — спросите текстом."));
        return;
    }
    if (mic.empty()) {
        enterState(BarState::Idle);
        showTransientStatus(QStringLiteral("Не расслышал — повтори."));
        return;
    }
    awaitingStt_ = true;
    sttGen_ = ++pipelineGen_;   // тег задания — onLocalResult примет только ЕГО результат
    pipeline_->submit(mic, /*diarize*/false, sttGen_);  // Q&A: чистый STT, один сегмент speaker=-1 → onLocalResult
    enterState(BarState::Recognizing);
    // Failsafe: если resultReady не придёт (движок рухнул после init) — не залипаем
    // в «⏳ Распознаю…» навсегда (страница-статус блокирует и текстовый ввод).
    if (failsafeTimer_) failsafeTimer_->start(30 * 1000);
}

// --- «Обзвон»: PTT по звуку СОБЕСЕДНИКА (вопрос задаёт интервьюер, не игрок) ---
// Зажал → пишем мик+звук игры (собеседник = звук Discord) в кольца; отпустил → стандартный разбор окна
// (диаризация по каналам) → ответ через requestAdviceFor(mode=leadership). Границы вопроса задаёт сам
// игрок (как PTT в Q&A) — VAD/эндпоинтер не нужен, нет ложных срабатываний на паузе-передышке.
void OverlayWindow::startScenePtt()
{
    if (currentMode() != QLatin1String("leadership"))
        return;
    if (pttActive_ || awaitingStt_ || localBusy_)
        return;   // уже пишем / ещё распознаём / разбор в полёте
    if (state_ == BarState::SceneThinking) {
        showTransientStatus(QStringLiteral("Подожди — отвечаю на предыдущий вопрос…"));
        return;
    }
    if (auth_ && !auth_->isAuthorized()) {
        showTransientStatus(QStringLiteral("Сначала активируйте ключ."));
        return;
    }
    if (models_ && !models_->isReady()) {
        showTransientStatus(QStringLiteral(
            "Модель распознавания не скачана — докачайте в Настройках."));
        return;
    }
    if (pipelineFailed_) {
        showTransientStatus(QStringLiteral("Локальное распознавание недоступно."));
        return;
    }
    ensureLocalPipeline();
    micRing_.clear();
    otherRing_.clear();
    if (audio_) audio_->start(/*micOnly*/false);   // мик + звук игры: собеседник = звук Discord
    pttActive_ = true;
    scenePtt_  = true;
    enterState(BarState::SceneListening);          // «Слушаю вопрос…»
}

void OverlayWindow::stopScenePtt()
{
    if (!pttActive_) return;
    pttActive_ = false;
    scenePtt_  = false;
    if (audio_) audio_->stop();
    // Окно вопроса захвачено в кольца за время удержания → штатный разбор → совет(leadership).
    triggerSceneAnalysis();
}

void OverlayWindow::ensureLocalPipeline()
{
    if (pipeline_) return;
    pipeline_ = new SherpaPipeline(this);
    // resultReady — функтором: PipelineResult не зарегистрирован для строковых SIGNAL/SLOT.
    connect(pipeline_, &SherpaPipeline::resultReady, this,
            [this](const PipelineResult &r) { onLocalResult(r); });
    connect(pipeline_, &SherpaPipeline::initialized, this, [this](bool ok, const QString &err) {
        if (!ok) {
            // Движок мёртв (битая модель при совпавшем размере/OOM/хайджек onnxruntime.dll):
            // worker вышел из цикла НЕ обслужив очередь — resultReady по уже отправленному
            // окну не придёт НИКОГДА. Разлочиваем state-машину, иначе бар навсегда застрял
            // бы в «⏳ Распознаю…» (блокируя И голос, И текст — нарушение инварианта
            // «текстовый ввод работает без модели»).
            pipelineFailed_ = true;
            pipeline_->deleteLater();
            pipeline_ = nullptr;
            if (awaitingStt_) {
                awaitingStt_ = false;
                sttGen_ = 0;
                if (failsafeTimer_) failsafeTimer_->stop();
                enterState(BarState::Idle);
            }
            // Сценовый захват с мёртвым движком бессмыслен (разбирать нечем) — глушим,
            // иначе «👂 Слушаю сцену…» висело бы при заведомо нерабочем разборе.
            if (sceneListening_) {
                sceneListening_ = false;
                if (audio_ && audio_->isRunning() && !pttActive_) audio_->stop();
                if (endpointer_) endpointer_->resetState();
                updateListenBtn();
                enterState(BarState::SceneIdle);
            }
            showTransientStatus(QStringLiteral("Локальный движок: %1").arg(err));
        } else {
            qInfo().noquote() << "[ui] локальный STT готов";
        }
    });
    SherpaPipeline::Config pc;
    pc.modelsDir  = ModelManager::modelsDir();
    pc.provider   = QStringLiteral("cpu");
    pc.numThreads = 4;
    pc.sampleRate = 16000;
    // Порог кластеризации диаризации (сценовые режимы; Q&A-прогону не мешает). Живая
    // подстройка перед каждым разбором — в triggerSceneAnalysis (setClusterThreshold).
    pc.clusterThreshold = static_cast<float>(settings::localDiarThreshold());
    pipeline_->init(pc);
}

void OverlayWindow::onLocalResult(const PipelineResult &r)
{
    // Атрибуция результата СТРОГО по поколению (r.tag == sttGen_/sceneGen_): worker может
    // секунды-десятки секунд доделывать ПРЕДЫДУЩЕЕ задание (диаризация на слабом CPU), и его
    // запоздавший результат прилетает уже при ВЗВЕДЁННОМ новом этапе — по одним флагам
    // awaitingStt_/localBusy_ сценовый текст ушёл бы Q&A-вопросом (и наоборот), а настоящий
    // результат нового этапа потом отбросился бы.

    // --- Q&A-ветка: PTT-вопрос (STT-only прогон, один сегмент speaker=-1) ---
    if (awaitingStt_ && r.tag == sttGen_) {
        awaitingStt_ = false;
        sttGen_ = 0;
        if (failsafeTimer_) failsafeTimer_->stop();   // результат пришёл — страховка не нужна

        QString q;
        if (r.ok)
            for (const AttributedSegment &seg : r.segments)
                q += seg.text + QChar(' ');
        q = q.trimmed();

        if (q.isEmpty()) {
            enterState(BarState::Idle);
            // «Тихий микрофон»: текста нет И пик был ≈тишина — проблема захвата/громкости, не STT.
            if (utterancePeak_ < 0.12f)
                showTransientStatus(QStringLiteral(
                    "🎙 Микрофон почти не слышно — проверьте громкость/устройство (Настройки → Звук)."));
            else
                showTransientStatus(QStringLiteral("Не расслышал вопрос — повтори."));
            return;
        }
        // Транскрипт → по режиму: «Поиск» ищет по каталогу законки, Q&A спрашивает ассистента.
        if (currentMode() == QLatin1String("laws"))
            startLawsSearch(q);
        else
            askQuestion(q);
        return;
    }

    // --- Сценовая ветка: submitScene из triggerSceneAnalysis (диаризация + посегментный STT) ---
    if (localBusy_ && r.tag == sceneGen_) {
        localBusy_ = false;
        sceneGen_ = 0;
        if (failsafeTimer_) failsafeTimer_->stop();

        if (!r.ok) {
            enterState(restingState());
            showTransientStatus(QStringLiteral("Локальный STT: %1").arg(r.error));
            return;
        }

        // Фильтр энергии (2-я линия против фоновых голосов): среди other-сегментов берём
        // громкость главного говорящего как опорную и выкидываем тихие (фон за стеной/в
        // стороне) — ОТНОСИТЕЛЬНО, не по абсолютному порогу. Сравниваем mean-square (без
        // sqrt). «Вы» (speaker<0 = микрофон) не трогаем.
        auto segMeanSquare = [this](const AttributedSegment &seg) -> double {
            if (lastOtherWindow_.empty()) return 1.0;
            const int sr = lastWindowSr_;
            long begin = static_cast<long>(seg.start * sr);
            long count = static_cast<long>((seg.end - seg.start) * sr);
            if (begin < 0) begin = 0;
            if (begin + count > static_cast<long>(lastOtherWindow_.size()))
                count = static_cast<long>(lastOtherWindow_.size()) - begin;
            if (count <= 0) return 0.0;
            double s = 0.0;
            for (long i = 0; i < count; ++i) {
                const double v = lastOtherWindow_[static_cast<size_t>(begin + i)];
                s += v * v;
            }
            return s / static_cast<double>(count);
        };
        double refMs = 0.0;
        for (const AttributedSegment &seg : r.segments)
            if (seg.speaker >= 0) refMs = std::max(refMs, segMeanSquare(seg));
        const bool canFilter = !lastOtherWindow_.empty() && refMs > 0.0;
        const double dropMs = refMs * 0.10;   // ~ −10 дБ по амплитуде → считаем фоном

        // Текст сцены построчно: «Вы: …» (микрофон) / «A: …» (собеседники по диаризации).
        QString flat;
        for (const AttributedSegment &seg : r.segments) {
            if (canFilter && seg.speaker >= 0 && segMeanSquare(seg) < dropMs)
                continue;   // тихий фоновый сегмент — не показываем и не шлём в облако
            const QString who = (seg.speaker < 0) ? QStringLiteral("Вы")
                                                  : speakerLabels_.labelFor(seg.speaker);
            flat += who + QStringLiteral(": ") + seg.text + QChar('\n');
        }
        flat = flat.trimmed();

        const double rtf = r.processSeconds / std::max(0.001, r.audioSeconds);
        qInfo().noquote() << "[ui] сцена: спикеров=" << r.numSpeakers
                          << " аудио=" << QString::number(r.audioSeconds, 'f', 1) << "с"
                          << " RTF=" << QString::number(rtf, 'f', 2);

        if (flat.isEmpty()) {
            enterState(restingState());
            showTransientStatus(QStringLiteral("В сцене не разобрал реплик."));
            return;
        }
        requestAdviceFor(flat);   // текст сцены → сервер за советом (SSE) [+чек-лист у ролей]
        return;
    }

    // Не наш прогон: запоздавший результат после сброса фазы (failsafe/смена режима/логаут)
    // или ЧУЖОГО поколения (worker доделал старое задание при уже взведённом новом) — игнор.
    qInfo().noquote() << "[ui] resultReady чужого поколения/этапа — игнор (tag=" << r.tag << ")";
}

void OverlayWindow::onAudioFrame(const QByteArray &pcm16, const QString &source)
{
    const int n = static_cast<int>(pcm16.size() / 2);
    if (n <= 0) return;
    const int16_t *s = reinterpret_cast<const int16_t *>(pcm16.constData());
    if (source == QLatin1String("other")) {
        // Звук игры (собеседники) — только сценовый захват (в Q&A micOnly=true, но страхуемся).
        otherRing_.pushInt16(s, n);
        // Авто-разбор сцены: кадры игры → VAD-эндпоинтер (его worker сам решит «пора разбирать»).
        if (endpointer_ && sceneListening_)
            endpointer_->pushOther(pcm16);
        return;
    }
    micRing_.pushInt16(s, n);
}

// Пик микрофона (0..1) — максимум за текущий вопрос: отличаем «не расслышал» (STT не понял)
// от «микрофон почти молчит» (тихо/занижен уровень/APO).
void OverlayWindow::onAudioLevel(const QString &source, float peak)
{
    if (source != QLatin1String("mic"))
        return;
    if (peak > utterancePeak_)
        utterancePeak_ = peak;
    // Авто-разбор сцены: активность игрока → эндпоинтер (разговорная смежность + подавление,
    // пока говорит сам игрок).
    if (endpointer_ && sceneListening_)
        endpointer_->noteMicLevel(peak);
}

// ---------------------------------------------------------------------------
// Настройки: модальный SettingsDialog; диалог сам пишет settings::* и эмитит сигналы —
// применяем на лету.
// ---------------------------------------------------------------------------
void OverlayWindow::onOpenSettings()
{
    // Единая точка защиты от ВТОРОГО вложенного диалога. onOpenSettings зовут ДВА пути: шестерёнка
    // бара (напрямую) и трей-меню/хаб «Настройки…» (через main). Трей-попап системный и доступен ПОВЕРХ
    // app-модального exec() → без гарда открывался второй translucent-диалог (двойной startPreview аудио,
    // перетирание настроек). Гард в main покрывал лишь хаб-путь — шестерёнка его не взводила.
    if (inSettingsDlg_) return;
    inSettingsDlg_ = true;
    // НЕ делать СКРЫТЫЙ бар (открыли настройки, не запуская оверлей) родителем/owner'ом модалки: иначе
    // диалог центрируется по невидимому 640×480-HWND и связан с ним как owner → прозрачный квадрат.
    // Диалог всё равно app-модальный (setModal+exec); connect'ы — по &dlg, от parent не зависят.
    SettingsDialog dlg(audio_, isVisible() ? this : nullptr);
    dlg.setServerData(projectsData_, selectedServerId_);   // страница «Сервер / RP»

    // Список серверов приезжает АСИНХРОННО (/servers), а диалог строится сразу. Если настройки
    // открыли раньше ответа — типично, когда их открывают из хаба, не запуская сам бар, — пикер
    // так и оставался пустым: «(войдите, чтобы загрузить)» при живом логине, и лечилось только
    // повторным открытием. Дозаполняем на лету и, если данных нет вовсе, перезапрашиваем.
    // Живой дозалив включаем ТОЛЬКО когда пикер пуст, иначе поздний ответ сбросил бы выбор,
    // который пользователь уже сделал руками.
    if (advice_ && projectsData_.isEmpty()) {
        connect(advice_, &AdviceClient::serversReady, &dlg,
                [this, &dlg](const QVariantList &projects) {
                    dlg.setServerData(projects, selectedServerId_);
                });
        if (auth_ && auth_->isAuthorized()) {
            serversRequested_ = true;
            advice_->requestServers();   // повтор безвреден: ответ просто перезальёт пикер
        }
    }
    dlg.setModelManager(models_);                          // статус/докачка STT-модели
    connect(&dlg, &SettingsDialog::audioSettingsChanged,
            this, &OverlayWindow::applyAudioSettings);
    connect(&dlg, &SettingsDialog::windowSettingsChanged,
            this, &OverlayWindow::applyWindowSettings);
    connect(&dlg, &SettingsDialog::serverSettingsChanged,
            this, &OverlayWindow::onServerSettingsChanged);
    connect(&dlg, &SettingsDialog::autoAdviceChanged, this, [this] {
        // Сменили пресет чувствительности авто-советов — применяем пороги вживую
        // (VAD пересоздаётся на worker-потоке эндпоинтера, захват не прерывается).
        if (endpointer_)
            endpointer_->applyConfig(makeEndpointerConfig(resolveVadPath()));
    });
    connect(&dlg, &SettingsDialog::logoutRequested,
            this, &OverlayWindow::onLogout);
    dlg.exec();
    inSettingsDlg_ = false;
}

// Выход из аккаунта: чистим токены и возвращаемся к окну активации. auth_->begin() при
// отсутствии токенов эмитит needKey → main прячет оверлей и поднимает ActivationWindow.
void OverlayWindow::onLogout()
{
    if (pttActive_) {
        pttActive_ = false;
        if (audio_) audio_->stop();
    }
    // Сценовый захват не должен пережить выход из аккаунта (советы требуют Bearer).
    if (sceneListening_) {
        sceneListening_ = false;
        if (audio_ && audio_->isRunning()) audio_->stop();
        if (endpointer_) endpointer_->resetState();
        updateListenBtn();
    }
    // Полный сброс фазы: страховку гасим, поэтому ни один «в полёте»-этап не должен пережить
    // логаут — локальные прогоны отсекутся по поколению (sttGen_/sceneGen_=0), сетевые
    // запросы (SSE+чек-лист, Q&A) рубим под корень.
    awaitingStt_ = false;
    localBusy_ = false;
    sttGen_ = 0;
    sceneGen_ = 0;
    awaitingFirstDelta_ = false;
    if (failsafeTimer_) failsafeTimer_->stop();
    if (advice_) {
        advice_->cancelAdvice();
        advice_->cancelQa();
    }
    if (laws_) laws_->cancelAll();   // поиск требует Bearer — логаут его не переживает
    enterState(restingState());
    if (auth_) {
        auth_->logout();   // POST /v1/auth/logout + удаление tokens.dat
        auth_->begin();    // токенов нет → needKey → окно активации
    }
    // Чистая память диалога на аккаунт: серверный rolling-контекст Q&A не должен
    // пережить смену пользователя.
    sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    showTransientStatus(QStringLiteral("Вы вышли. Введите ключ активации заново."));
}

void OverlayWindow::applyAudioSettings()
{
    if (!audio_) return;
    // Если прямо сейчас идёт захват (PTT или сцена) — перезапустить с новыми параметрами.
    const bool restart = (pttActive_ || sceneListening_) && audio_->isRunning();
    if (restart) audio_->stop();
    audio_->setMicDeviceId(settings::audio().micDeviceId);
    audio_->setGameProcessExe(settings::audio().gameProcessExe);   // источник звука игры (сцена)
    if (restart) {
        // PTT и сцена взаимоисключены гейтами режимов: micOnly строго по PTT.
        audio_->start(/*micOnly*/pttActive_);
        if (sceneListening_ && endpointer_) endpointer_->resetState();
    }
    showTransientStatus(QStringLiteral("Настройки звука сохранены."));
}

void OverlayWindow::applyWindowSettings()
{
    applyWindowConfig();      // позиция/прозрачность/шрифт/потолок панели
    // Программный move выше (дефолт/кламп/«Сбросить позицию») взвёл дебаунс сохранения —
    // гасим, как в ctor: иначе через 600мс hasGeometry снова стал бы true и сброс позиции
    // сам себя отменял. Персистим только то, что двигал ПОЛЬЗОВАТЕЛЬ.
    if (geoSaveTimer_) geoSaveTimer_->stop();
    // Перерегистрация хоткеев — только если их строки реально изменились: иначе каждый тик
    // слайдера прозрачности/шрифта дёргал Unregister/Register ×5 и глобальные хоткеи
    // «мерцали» на время перетаскивания.
    if (hotkeysKey(settings::window()) != appliedHotkeysKey_)
        registerHotkeys();    // внутри сперва UnregisterHotKey
    applyCaptureAffinity();   // тумблер «скрывать из записи экрана» применяем вживую
    // «Режим невидимка» мог смениться — переустановить аффинити на прочих открытых окнах
    // (Hub/настройки) без перезапуска. Бар уже накрыт applyCaptureAffinity выше.
    stealth::sweepAllTopLevel();
    updateMicDot();           // PTT/сценовые клавиши в tooltip могли измениться
    // Хоткей в подсказке SceneIdle («„R" или ▶ — слушать сцену») мог смениться — перерисовать
    // текущий статус. Только когда транзиента нет: его текст затирать нельзя.
    if (statusRevertTimer_ && !statusRevertTimer_->isActive())
        enterState(state_);
    updateListenBtn();        // tooltip кнопки сцены тоже содержит hkLocalRecord
}

// ---------------------------------------------------------------------------
//  Окно: геометрия / topmost / WDA-невидимость / сохранение позиции / перетаскивание.
// ---------------------------------------------------------------------------
void OverlayWindow::applyWindowConfig()
{
    const WindowSettings &ws = settings::window();
    setWindowOpacity(ws.opacity);

    // Масштаб шрифта ответа (база 13px).
    if (answerView_) {
        QFont f(QStringLiteral("Segoe UI"));
        f.setPixelSize(qMax(9, 13 * ws.fontScalePct / 100));
        answerView_->setFont(f);
    }

    const QList<QScreen *> screens = QGuiApplication::screens();
    QScreen *scr = (ws.monitorIndex >= 0 && ws.monitorIndex < screens.size())
                       ? screens.at(ws.monitorIndex) : QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 720);

    // Потолок панели ответа — ~40% высоты экрана (дальше — скролл). updateAnswerViewCap
    // дополнительно поджимает УЖЕ зафиксированную documentSizeChanged-высоту: при смене
    // монитора на меньший старый setFixedHeight иначе остался бы выше нового потолка.
    answerMaxH_ = static_cast<int>(avail.height() * 0.40);
    updateAnswerViewCap();

    // Позиция: сохранённая (с клампом в экран) или дефолт — верх по центру выбранного монитора.
    // Размер не трогаем: ширина фикс 520, высоту считает layout (SetFixedSize).
    if (ws.hasGeometry) {
        const int x = qBound(avail.left(), ws.x, qMax(avail.left(), avail.right() - kBarWidth));
        const int y = qBound(avail.top(),  ws.y, qMax(avail.top(),  avail.bottom() - kBarHeight));
        move(x, y);
    } else {
        move(avail.left() + (avail.width() - kBarWidth) / 2, avail.top() + 10);
    }
}

void OverlayWindow::applyCaptureAffinity()
{
    // Единая точка: бар скрыт из захвата, если включён мастер-«режим невидимка» ИЛИ старый
    // тумблер captureInvisible. Реализация Win32 (SetWindowDisplayAffinity) — в StealthGuard.
    stealth::applyAffinity(winId(), stealth::barHidden());
}

void OverlayWindow::reapplyTopmost()
{
    if (!isVisible()) return;
    // ВАЖНО: пока открыт выпадающий список / любой popup / модальный диалог — НЕ трогаем
    // z-order. Иначе SetWindowPos(HWND_TOPMOST) перекрывает popup combo-бокса, и Windows/Qt
    // его закрывают — выпадашки «схлопывались почти сразу» (и в окне настроек тоже).
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget())
        return;
    // Страховка от «залипшей» активируемости: если activateWindow() был отклонён системой
    // (foreground-lock полноэкранной игры), FocusIn не пришёл — не придёт и FocusOut,
    // и WS_EX_NOACTIVATE не вернулся бы никогда (каждый клик по бару крал бы фокус у игры).
    // Таймер тикает каждые 1.5с — откатываем, когда мы не активны и строка ввода без фокуса.
    if (inputActivatable_ && !isActiveWindow() && !(askInput_ && askInput_->hasFocus()))
        setInputActivatable(false);
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

void OverlayWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    everShown_ = true;   // бар хоть раз реально показан → гард show-путей хоткеев (toggleVisibility/focusInput)
#ifdef Q_OS_WIN
    // Native-хэндл мог пересоздаться — восстанавливаем ВСЁ HWND-зависимое, а не только
    // NOACTIVATE: глобальные хоткеи зарегистрированы на КОНКРЕТНЫЙ HWND (отвалились бы
    // молча), DWM-рамка Win11 вернулась бы, click-through-стили сбросились бы при
    // включённом clickThrough_ (рассинхрон флага и факта).
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!inputActivatable_) {
        const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
        commitExStyle(hwnd);   // применить восстановленный ex-стиль к слою
    }
    COLORREF noBorder = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
    registerHotkeys();   // внутри сперва UnregisterHotKey — повторный вызов безопасен
    if (clickThrough_)
        applyClickThrough(true, /*notify*/false);
#endif
    applyCaptureAffinity();
    reapplyTopmost();
    update();   // форс первого флаша слоёного окна: без него «показан, но не перекрашен» = прозрачный квадрат
}

void OverlayWindow::saveGeometryNow()
{
    WindowSettings ws = settings::window();
    const QPoint p = pos();
    ws.x = p.x();
    ws.y = p.y();
    ws.hasGeometry = true;
    QScreen *scr = QGuiApplication::screenAt(frameGeometry().center());
    if (scr) {
        ws.monitorIndex = QGuiApplication::screens().indexOf(scr);
        // Бар перетащили — потолок панели ответа пересчитываем от ФАКТИЧЕСКОГО экрана:
        // иначе на меньшем мониторе ответ вылезал бы за экран (SetFixedSize растёт вниз
        // без клампа), на большем — оставался неоправданно мелким со скроллом.
        // updateAnswerViewCap внутри поджимает и зафиксированную setFixedHeight-высоту.
        answerMaxH_ = static_cast<int>(scr->availableGeometry().height() * 0.40);
        updateAnswerViewCap();
    }
    settings::setWindow(ws);
}

// Бюджет высоты панели ответа. answerMaxH_ (~40% экрана) — потолок ВСЕЙ панели, а не только
// текста: шапка, chips и чек-лист тоже растят окно (layout SetFixedSize — строго вниз), и без
// их вычета длинный чек-лист выталкивал бы низ панели за потолок. Чек-листу — собственный кэп
// ~30% бюджета (клип по низу, см. AlignTop в buildUi); остаток — answerView_, чьё переполнение
// доедает его скролл.
void OverlayWindow::updateAnswerViewCap()
{
    if (!answerView_ || !answerPanel_) return;
    int extras = 0;
    if (QLayout *lay = answerPanel_->layout()) {
        const QMargins m = lay->contentsMargins();
        const int sp = qMax(0, lay->spacing());
        const int innerW = kBarWidth - m.left() - m.right();
        extras += m.top() + m.bottom();
        extras += (closeAnsBtn_ ? closeAnsBtn_->height() : 22) + sp;   // строка-шапка
        // isVisibleTo (не isVisible): учитываем элементы и при скрытой панели — бюджет должен
        // быть готов к её открытию.
        if (chipsLabel_ && chipsLabel_->isVisibleTo(answerPanel_))
            extras += chipsLabel_->heightForWidth(innerW) + sp;
        if (checklistLabel_ && checklistLabel_->isVisibleTo(answerPanel_)) {
            const int cap = answerMaxH_ * 3 / 10;
            checklistLabel_->setMaximumHeight(cap);
            extras += qMin(cap, checklistLabel_->heightForWidth(innerW)) + sp;
        }
    }
    answerViewCap_ = qMax(60, answerMaxH_ - extras);
    // setFixedHeight из documentSizeChanged мог зафиксировать высоту по СТАРОМУ бюджету
    // (смена монитора/появился чек-лист) — поджимаем сразу, не дожидаясь изменения документа.
    if (answerView_->height() > answerViewCap_)
        answerView_->setFixedHeight(answerViewCap_);
}

// Панель растёт layout'ом строго ВНИЗ: у бара, перетащенного к низу экрана, раскрывшийся
// ответ уезжал бы за рабочую область — поджимаем y, чтобы низ окна остался на экране.
// Программный move гасит дебаунс сохранения (персистим только ручные переносы, как в ctor).
void OverlayWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    QScreen *scr = QGuiApplication::screenAt(frameGeometry().center());
    if (!scr) scr = QGuiApplication::primaryScreen();
    if (!scr) return;
    const QRect avail = scr->availableGeometry();
    const int overflow = frameGeometry().bottom() - avail.bottom();
    if (overflow > 0) {
        move(x(), qMax(avail.top(), y() - overflow));
        if (geoSaveTimer_) geoSaveTimer_->stop();
    }
}

void OverlayWindow::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);
    if (geoSaveTimer_) geoSaveTimer_->start();   // дебаунс 600мс
}

// Перетаскивание за фон бара (в click-through события не приходят из-за WS_EX_TRANSPARENT).
void OverlayWindow::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        dragOffset_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
    // Клик по ФОНУ бара (клик в строку ввода съел бы сам QLineEdit — сюда не дошёл бы) при активном
    // наборе → выйти из режима ввода: escape-hatch от «залипшего» курсора в чате (клик мимо поля
    // возвращает фокус игре, а не держит клавиатуру в оверлее).
    if (inputActivatable_)
        setInputActivatable(false);
}

void OverlayWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton) {
        move(e->globalPosition().toPoint() - dragOffset_);
        update();   // слоёное окно: очистить «хвост» прозрачной рамки при перетаскивании
    }
}
