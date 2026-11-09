#pragma once

#include <QDialog>
#include <QString>
#include <QPoint>
#include <QVariantList>

// Forward-declares: AudioCapture создаётся в main, диалог им только пользуется
// (перечисляет микрофоны + живой VU), не владеет.
class AudioCapture;
class ModelManager;   // докачка локальных моделей (живёт в OverlayWindow; диалог только показывает статус)

// Qt-классы виджетов — forward-declare, чтобы не тянуть заголовки в .h.
class QComboBox;
class QProgressBar;
class QSlider;
class QKeySequenceEdit;
class QCheckBox;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QTimer;
class QListWidget;
class QStackedWidget;
class QWidget;
class QMouseEvent;
class QShowEvent;

// SettingsDialog — окно настроек в стиле Majestic: плавающая crimson-карточка
// (frameless + кастомная шапка), слева — навигация по разделам (QListWidget),
// справа — страницы (QStackedWidget). Разделы: Звук · Управление · Оверлей ·
// Соединение · Сервер/RP · Аккаунт · Отладка.
// Выбор немедленно пишется в settings:: и сохраняется на диск, после чего
// эмитятся соответствующие *Changed()-сигналы (OverlayWindow применяет на лету).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    // audio — источник списка микрофонов (не владеем). Может быть nullptr:
    // тогда список содержит только дефолтный пункт + сохранённое значение.
    explicit SettingsDialog(AudioCapture *audio, QWidget *parent = nullptr);
    ~SettingsDialog() override;   // гасит предпросмотр-захват

    // Прокинуть список проектов/серверов (из OverlayWindow.projectsData_) + текущий server_id,
    // чтобы заполнить пикер в разделе «Сервер / RP».
    void setServerData(const QVariantList &projects, const QString &curServerId);

    // Прокинуть менеджер моделей (статус/прогресс/докачка по кнопке на странице «Управление»).
    void setModelManager(ModelManager *mm);

signals:
    // Эмитится после смены микрофона/источника звука игры и settings::save().
    void audioSettingsChanged();
    // Эмитится после смены настроек окна (прозрачность/шрифт/монитор/хоткеи/невидимость
    // для записи экрана) и settings::save().
    void windowSettingsChanged();
    // Эмитится после смены выбранного проекта/сервера или режима при запуске (settings::setServer()).
    void serverSettingsChanged();
    // Эмитится после смены чувствительности авто-советов — OverlayWindow переконфигурирует эндпоинтер.
    void autoAdviceChanged();
    // Эмитится при нажатии «Выйти из аккаунта» (OverlayWindow вызывает AuthClient::logout()).
    void logoutRequested();

protected:
    // Перетаскивание frameless-окна за шапку.
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void showEvent(QShowEvent *e) override;     // снять системную рамку Win11

private slots:
    void onMicActivated(int index);
    void onGameActivated(int index);
    void onRefresh();
    void onLevel(const QString &source, float peak);  // живой VU пока окно открыто
    void onWindowChanged();    // любой контрол окна → взвести дебаунс (см. commitWindowSettings)
    void onResetGeometry();    // сбросить размер/позицию к дефолту
    void onServerProjectChanged(int index);  // каскад: проект → серверы
    void onServerComboChanged(int index);     // запомнить выбранный server_id (+ save + сигнал)
    void onPttCaptureStart();   // начать захват PTT-биндинга («нажмите клавишу/кнопку мыши»)
    void onPttCaptureTick();    // опрос нажатия во время захвата
    void onLogout();            // «Выйти из аккаунта»: подтверждение → logoutRequested()

private:
    void commitWindowSettings();  // собрать WindowSettings + save() + сигнал (зовёт дебаунс-таймер/деструктор)
    void buildUi();
    QWidget *makePage(const QString &sectionTitle, QVBoxLayout *&outLayout);  // каркас страницы
    void buildSoundPage();     // Звук: микрофон + источник звука игры + VU обоих
    void buildControlPage();   // Управление: хоткеи + PTT + разбор сцены + авто-советы + модель STT
    void refreshModelUi();     // обновить статус/кнопки моделей под ModelManager::status()
    void selectServerInPicker(const QString &serverId);  // выбрать сервер в каскаде проект→сервер (без save)
    void buildOverlayPage();   // Оверлей: прозрачность/шрифт/монитор/сброс + невидимость для записи
    void buildConnPage();      // Соединение: канал связи + авто-обновление
    void buildServerPage();    // Сервер / RP: проект → сервер + режим при запуске + память диалога
    void buildAccountPage();   // Аккаунт: выход из Telegram
    void buildDebugPage();     // Отладка: режим отладки + логи
    void applyDialogChrome();  // бренд-лого/заголовок под активную тему (theme::isMajestic)
    void updateFooterServer(); // пилюля «Сервер: X» в футере

    void populateMicCombo();   // перезаполнить список микрофонов из audio_->enumerateMicDevices()
    void populateGameCombo();  // перезаполнить список источников из audio_->enumerateAudioProcesses()
    void startPreview();       // подписаться на level + запустить захват (если не идёт)
    void stopPreview();        // отписаться + остановить захват (если запускали мы)
    void restartPreviewIfOwned();  // перезапустить наш предпросмотр под новое устройство

    // Зависимость (не владеем).
    AudioCapture *audio_ = nullptr;

    // --- Каркас окна (плавающая карточка + навигация) ---
    QWidget        *contentRoot_   = nullptr;  // #dlgContent — сама карточка
    QWidget        *header_        = nullptr;  // #dlgHead — шапка (drag-зона)
    QLabel         *brandLogo_     = nullptr;  // wordmark Majestic (виден в теме majestic)
    QLabel         *titleText_     = nullptr;  // «· Настройки» / «⚙️ Настройки»
    QListWidget    *navList_       = nullptr;  // #dlgNav — список разделов
    QStackedWidget *stack_         = nullptr;  // страницы разделов
    QLabel         *footServerPill_= nullptr;  // #dlgServerPill — «Сервер: X»
    bool            dragging_      = false;
    QPoint          dragPos_;

    // UI-элементы.
    QComboBox    *micCombo_   = nullptr;
    QComboBox    *gameCombo_  = nullptr;  // источник звука игры (сценовые режимы)
    QProgressBar *micLevel_   = nullptr;  // VU выбранного микрофона
    QProgressBar *gameLevel_  = nullptr;  // VU источника игры/системы
    bool          startedPreview_ = false; // захват запущен ИМЕННО диалогом (гасить при закрытии)

    // --- Раздел «Оверлей» ---
    QSlider          *opacitySlider_ = nullptr;
    QLabel           *opacityVal_    = nullptr; // «88%»
    QSlider          *fontSlider_    = nullptr; // размер шрифта, % (80..180)
    QLabel           *fontVal_       = nullptr; // «130%»
    QComboBox        *monitorCombo_  = nullptr;
    QCheckBox        *captureInvisibleChk_ = nullptr; // скрывать оверлей из записи экрана/OBS (WDA_EXCLUDEFROMCAPTURE)
    QCheckBox        *stealthChk_          = nullptr; // мастер-«режим невидимка» (тестер-билд; nullptr в RPOVERLAY_QA)
    QKeySequenceEdit *hkToggleEdit_  = nullptr;
    QKeySequenceEdit *hkClickEdit_   = nullptr;
    QKeySequenceEdit *hkAskEdit_     = nullptr;
    QKeySequenceEdit *hkCollapseEdit_= nullptr;
    // --- Локальный разбор сцены (диаризация + STT на ПК) ---
    QKeySequenceEdit *hkLocalRecEdit_ = nullptr;  // хоткей «слушать сцену: вкл/пауза»
    QKeySequenceEdit *hkLocalAnsEdit_ = nullptr;  // хоткей «разобрать сцену сейчас»
    QSlider          *localWinSlider_ = nullptr;  // длина анализируемого окна, сек (5..120)
    QLabel           *localWinVal_    = nullptr;  // «30 с»
    // --- Модель распознавания (локальный STT): статус/докачка ---
    ModelManager *models_         = nullptr;  // не владеем (живёт в OverlayWindow)
    QLabel       *modelStatusLbl_ = nullptr;  // «Готово (252 МБ)» / «Не скачано» / «Ошибка»
    QProgressBar *modelProgress_  = nullptr;  // прогресс докачки
    QPushButton  *modelDlBtn_     = nullptr;  // «Скачать модели»
    QPushButton  *modelDelBtn_    = nullptr;  // «Удалить»
    QLabel       *modelPathVal_   = nullptr;  // текущий каталог моделей (дефолт = папка установки)
    QCheckBox        *autoUpdateChk_ = nullptr; // тихое авто-обновление при запуске
    QComboBox        *connModeCombo_ = nullptr; // канал связи: Авто/Основной/Резервный (РФ)
    QCheckBox        *debugChk_      = nullptr; // режим отладки (консоль + лог-файл)
    bool              windowUiReady_ = false; // не реагировать на изменения во время заполнения
    QTimer           *windowSaveTimer_ = nullptr; // дебаунс onWindowChanged (~200мс): слайдеры не пишут диск на каждый тик

    // --- Push-to-talk: кастомный захват (клавиша / numpad / КНОПКА МЫШИ) ---
    QLabel       *pttBindLabel_  = nullptr;  // показывает текущий биндинг
    QPushButton  *pttCaptureBtn_ = nullptr;  // «Изменить» → режим захвата
    QTimer       *captureTimer_  = nullptr;  // опрос GetAsyncKeyState во время захвата
    bool          capturing_     = false;
    bool          captureBaseline_[256];     // какие VK были зажаты в момент старта захвата (игнор)

    // --- Раздел «Сервер / RP» ---
    QComboBox    *projectCombo_  = nullptr;
    QComboBox    *serverCombo_   = nullptr;
    QComboBox    *defaultModeCombo_ = nullptr; // режим при запуске оверлея (ServerSettings.defaultMode)
    QVariantList  serverProjects_;            // кэш списка из OverlayWindow
    QString       curServerId_;               // текущий выбранный server_id
    bool          serverUiLoading_ = false;   // не сохранять во время первичного заполнения
};
