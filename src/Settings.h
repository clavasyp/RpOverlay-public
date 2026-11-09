#pragma once

#include <QString>

// Пользовательские настройки клиента (read+write).
// Лениво грузится из %APPDATA%/ProdBot/RP Overlay/settings.json при первом обращении
// (QStandardPaths::AppDataLocation — та же папка, что и device_id.txt).
struct AudioSettings {
    QString micDeviceId;     // id выбранного микрофона ("" = устройство по умолчанию)
    QString gameProcessExe;  // basename .exe источника звука игры ("" = весь системный звук)
};

// Настройки окна оверлея (компактный бар). Персист в settings.json (объект "window").
struct WindowSettings {
    double  opacity      = 1.0;             // прозрачность окна (0.40..1.0); фон бара и так полупрозрачный (QSS rgba)
    int     fontScalePct = 100;             // масштаб шрифта ответа в % (80..180)
    bool    hasGeometry  = false;           // была ли сохранена позиция (иначе — сверху по центру)
    int     x = 0, y = 0;                   // позиция бара (верхний левый угол)
    int     monitorIndex = -1;              // индекс монитора (-1 = основной/авто)
    bool    captureInvisible = false;       // невидимость для захвата экрана/OBS (WDA_EXCLUDEFROMCAPTURE). ДЕФОЛТ СНЯТ (backlog): по умолчанию бар ВИДЕН в OBS; юзер включает сам
    bool    stealthMode      = false;       // мастер-«невидимка» (тестер-билд): WDA на ВСЕ окна+попапы + Hub/Activation вне таскбара/Alt-Tab
    QString hkToggle      = "Ctrl+Shift+O"; // показать/скрыть бар
    QString hkClickThrough= "Ctrl+Shift+L"; // режим «сквозь клики»
    QString hkAsk         = "Ctrl+Shift+Q"; // фокус в строку вопроса (ввод с клавиатуры, Q&A)
    QString hkCollapse    = "Ctrl+Shift+E"; // скрыть/показать панель ответа
    QString hkPushToTalk  = "Ctrl+Shift+Space"; // push-to-talk (удержание) — голосовой вопрос (Q&A)
    // --- Сценовые режимы (Адвокат/Прокурор/Общий): локальный разбор сцены ---
    QString hkLocalRecord = "Ctrl+Shift+R";     // вкл/выкл прослушивание сцены (авто-советы)
    QString hkLocalAnswer = "Ctrl+Shift+D";     // разобрать сцену сейчас (окно последних N сек)
    int     localWindowSec = 30;                // длина анализируемого окна, сек
};

// Выбор сервера/проекта (законка) и режим при запуске. Персист в settings.json (объект "server").
struct ServerSettings {
    QString projectId;    // id выбранного проекта ("" = дефолт по cfg().serverId)
    QString serverId;     // id выбранного сервера ("" = дефолт cfg().serverId)
    QString defaultMode;  // режим при старте: qa|advocate|prosecutor|general ("" = qa)
};

// Выбор проекта/сервера для режима «Обзвон» (leadership) — ОТДЕЛЬНЫЙ от законки (ServerSettings).
// Свод правил лидерской проверки зависит от игрового проекта/сервера: пусто → единый свод Majestic
// (сервер сам фолбэчит); GTA5RP → projectId="gta5rp", serverId="davis". Персист в settings.json ("leadership").
struct LeadershipSettings {
    QString projectId;    // id игрового проекта свода Обзвона ("" = Majestic/дефолт)
    QString serverId;     // id сервера свода (для GTA5RP; "" = только проектный слой)
};

// Числовые пороги авто-триггера сцены, развёрнутые из пресета чувствительности (Тихо/Норма/Активно).
// Производное от settings::autoAdviceSensitivity() — в settings.json НЕ пишется. OverlayWindow копирует
// эти значения в TurnEndpointerConfig (мс → сек на стороне сборки конфига).
struct AutoAdviceTuning {
    float voiceMarginDb;        // речь должна быть громче фона на N дБ (адаптивный передний план)
    int   vadHangoverMs;        // тишина после речи = «реплика кончилась» (→ minSilence)
    int   minUtteranceMs;       // мин. длительность реплики, чтобы реагировать (→ minUtterance)
    int   cooldownMs;           // пауза между авто-советами
    float retriggerDeltaSec;    // новой речи, чтобы пробить кулдаун (→ minNewSpeech)
    int   maxDisputeMs;         // таймаут спора без пауз — форс-разбор (→ maxDispute)
    bool  requireAdjacency;     // смежность диалога ОБЯЗАТЕЛЬНА (строгий «Тихо»)
    bool  suppressWhileMicHot;  // не триггерить, пока говорит сам игрок
    float vadThreshold;         // порог вероятности речи silero
};

namespace settings {

const AudioSettings &audio();               // ленивая загрузка settings.json
void setMicDeviceId(const QString &id);     // мутирует + save()
void setGameProcessExe(const QString &exe); // мутирует + save()

const WindowSettings &window();             // ленивая загрузка (тот же файл)
void setWindow(const WindowSettings &w);    // мутирует + save()

const ServerSettings &server();             // выбранные проект/сервер/режим
void setServer(const ServerSettings &s);    // мутирует + save()

const LeadershipSettings &leadership();      // выбор проекта/сервера свода «Обзвона»
void setLeadership(const LeadershipSettings &l);  // мутирует + save()

bool autoUpdateEnabled();                   // авто-обновление при запуске (по умолчанию true)
void setAutoUpdate(bool on);                // мутирует + save()

// --- Автозапуск игрового бара (0.4.0: первым открывается окно-хаб). Ключ "overlayAutostart". ---
// true = старое поведение ≤0.3.x: бар поднимается сразу при старте (хаб при этом тоже создаётся);
// false (дефолт) = при старте только хаб, бар запускается кнопкой «Запустить оверлей» из него.
bool overlayAutostart();                    // по умолчанию false
void setOverlayAutostart(bool on);          // мутирует + save()

// --- Порог кластеризации диаризации (разделение спикеров). Ключ "localDiarThreshold". ---
// Дефолт 1.1. Ниже → больше спикеров (если двоих слепило), выше → меньше. Верх ограничен 1.15:
// при ≥1.2 диаризатор sherpa падает на многоголосой сцене (SherpaPipeline::diarizeGuarded).
double localDiarThreshold();                // 0.5..1.15, по умолчанию 1.1
void setLocalDiarThreshold(double t);       // мутирует + save()

// --- Тема оформления (используется ТОЛЬКО для QSS окна настроек, см. src/Theme.*). Ключ "theme". ---
QString uiTheme();                          // "graphite" | "majestic" (дефолт)
void setUiTheme(const QString &name);       // мутирует + save()

// --- Режим отладки (консоль + лог-файл, см. src/DebugLog.*). Ключ "debug" в settings.json. ---
bool debugMode();                           // по умолчанию false
void setDebugMode(bool on);                 // мутирует + save()

// --- Каталог моделей распознавания (STT). Ключ "modelsPath" в settings.json. ---
// Пусто (дефолт) = папка установки рядом с exe ({app}\models, per-user → записываема). Юзер может
// задать свой каталог в настройках. ModelManager: пустой путь → дефолт-резолв (см. downloadDir()).
QString modelsPath();                       // "" = дефолт (папка установки)
void setModelsPath(const QString &path);    // мутирует + save() ("" = вернуть дефолт)

// --- Память диалога: сколько ПОСЛЕДНИХ сообщений сессии учитывать в ответах (Q&A/совет). ---
// Ключ "contextLimit" в settings.json. Дефолт 10, диапазон 0..10 (0 = без контекста). Сервер
// дополнительно клампит к своему жёсткому максимуму (10). Шлётся как history_limit в запросах.
int contextLimit();                         // 0..10, по умолчанию 10
void setContextLimit(int n);                // мутирует + save()

// --- Авто-советы (Cluely-режим) в сценовых режимах Адвокат/Прокурор/Общий. Q&A не затрагивает. ---
// Старт/стоп прослушивания — РАНТАЙМ (кнопка/хоткей, не персистится). Здесь только персист-пресет.
QString autoAdviceSensitivity();            // "quiet" | "normal" (дефолт) | "active"
void setAutoAdviceSensitivity(const QString &name);  // мутирует + save()
const AutoAdviceTuning &autoAdviceTuning(); // разворачивает текущий пресет в числовые пороги гейта

// --- A/B модели совета (тест: что лучше — flash или flash-lite). Ключ "adviceModel" в settings.json. ---
// Клиент шлёт это как model_tier в запросе совета; сервер по белому списку выбирает модель.
QString adviceModelTier();                  // "auto" (дефолт, решает сервер) | "flash" | "lite"
void setAdviceModelTier(const QString &t);  // мутирует + save()

// --- Канал связи: основной (домен) / резервный (через RU-прокладку). См. src/Connectivity.*. ---
// Персист в settings.json (объект "connection").
QString connMode();                       // "auto" (дефолт) | "main" | "relay"
void setConnMode(const QString &mode);    // мутирует + save()
QString connLast();                       // "main"|"relay"|"" — итог прошлой авто-пробы (диагностика)
void setConnLast(const QString &v);       // мутирует + save()

void save();                                // QJsonDocument → settings.json

} // namespace settings
