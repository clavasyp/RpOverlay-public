#include "SettingsDialog.h"
#include "Connectivity.h"   // conn::chooseChannel — мгновенная смена канала из настроек (B3)
#include "Config.h"         // setActiveChannel / activeChannel / cfg() — применить канал на лету (B3)
#include "Settings.h"
#include <QDebug>           // qInfo() — лог смены канала вживую (B3)
#include "Theme.h"
#include "DebugLog.h"
#include "LayerRepaint.h"
#include "AudioCapture.h"
#include "HotkeyUtil.h"
#include "ModelManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QProgressBar>
#include <QSlider>
#include <QKeySequenceEdit>
#include <QKeySequence>
#include <QCheckBox>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QMessageBox>
#include <QFileDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QSignalBlocker>
#include <QPixmap>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QShowEvent>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  ifndef DWMWA_BORDER_COLOR
#    define DWMWA_BORDER_COLOR 34
#  endif
#  ifndef DWMWA_COLOR_NONE
#    define DWMWA_COLOR_NONE 0xFFFFFFFE
#  endif
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"   // реально задаётся в CMake из project(... VERSION ...)
#endif

// ---------------------------------------------------------------------------
// Стиль диалога вынесен в модуль тем (src/Theme.*): theme::dialogStyle() отдаёт QSS
// активной темы. Тема ставится один раз в main (theme::setActive) — UI выбора темы
// удалён в 0.3.0, диалог лишь применяет стиль при создании.
// Вёрстка: плавающая карточка (#dlgContent) + шапка (#dlgHead) + сайдбар-навигация
// (#dlgNav) слева + страницы (QStackedWidget) справа + футер (#dlgFoot).
// ---------------------------------------------------------------------------

namespace {
// Небольшой заголовок поля над контролом (серый, 12px) — см. QSS #dlgField.
QLabel *fieldLabel(QWidget *p, const QString &text)
{
    auto *l = new QLabel(text, p);
    l->setObjectName(QStringLiteral("dlgField"));
    return l;
}
} // namespace

// ---------------------------------------------------------------------------
SettingsDialog::SettingsDialog(AudioCapture *audio, QWidget *parent)
    : QDialog(parent), audio_(audio)
{
    setWindowTitle(QStringLiteral("Настройки"));
    // Плавающая карточка как у оверлея: frameless + полупрозрачный фон (скруглённые углы),
    // always-on-top (поверх оверлея). Фон рисует #dlgContent, не само окно.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(theme::dialogStyle());
    setModal(true);

    buildUi();
    applyDialogChrome();

    populateMicCombo();
    populateGameCombo();
    updateFooterServer();

    // Живой VU пока окно открыто: запускаем захват для предпросмотра уровней,
    // чтобы сразу видеть, есть ли звук с выбранного микрофона/источника.
    startPreview();
}

SettingsDialog::~SettingsDialog()
{
    // Флаш дебаунса настроек окна: если юзер закрыл диалог раньше, чем сработал
    // таймер (200мс), несохранённые изменения слайдеров/хоткеев досохраняем сейчас.
    if (windowSaveTimer_ && windowSaveTimer_->isActive()) {
        windowSaveTimer_->stop();
        commitWindowSettings();
    }
    stopPreview();
}

// ---------------------------------------------------------------------------
//  Каркас: карточка → (шапка) + (сайдбар + страницы) + (футер).
// ---------------------------------------------------------------------------
void SettingsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 12);   // прозрачные поля вокруг карточки
    root->setSpacing(0);

    contentRoot_ = new QWidget(this);
    contentRoot_->setObjectName(QStringLiteral("dlgContent"));
    root->addWidget(contentRoot_);

    auto *cl = new QVBoxLayout(contentRoot_);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);

    // --- Шапка (drag-зона): бренд + «Настройки» + ✕ ---
    header_ = new QWidget(contentRoot_);
    header_->setObjectName(QStringLiteral("dlgHead"));
    header_->setFixedHeight(48);
    auto *hl = new QHBoxLayout(header_);
    hl->setContentsMargins(16, 0, 8, 0);
    hl->setSpacing(10);

    brandLogo_ = new QLabel(header_);
    brandLogo_->setObjectName(QStringLiteral("dlgBrandLogo"));
    {
        const QPixmap logo(QStringLiteral(":/assets/majestic_logo.png"));
        if (!logo.isNull())
            brandLogo_->setPixmap(logo.scaledToHeight(16, Qt::SmoothTransformation));
    }
    hl->addWidget(brandLogo_, 0);

    titleText_ = new QLabel(QStringLiteral("Настройки"), header_);
    titleText_->setObjectName(QStringLiteral("dlgTitle"));
    hl->addWidget(titleText_, 1);

    auto *closeX = new QPushButton(QStringLiteral("✕"), header_);
    closeX->setObjectName(QStringLiteral("winClose"));
    closeX->setToolTip(QStringLiteral("Закрыть"));
    closeX->setCursor(Qt::PointingHandCursor);
    closeX->setFixedSize(30, 28);
    hl->addWidget(closeX, 0);
    connect(closeX, &QPushButton::clicked, this, &QDialog::accept);

    cl->addWidget(header_);

    // --- Тело: сайдбар + страницы ---
    auto *body = new QWidget(contentRoot_);
    auto *bl = new QHBoxLayout(body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);

    auto *side = new QWidget(body);
    side->setObjectName(QStringLiteral("dlgSide"));
    side->setFixedWidth(192);
    auto *sl = new QVBoxLayout(side);
    sl->setContentsMargins(10, 12, 10, 12);
    sl->setSpacing(0);

    navList_ = new QListWidget(side);
    navList_->setObjectName(QStringLiteral("dlgNav"));
    navList_->setFrameShape(QFrame::NoFrame);
    navList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    navList_->setFocusPolicy(Qt::NoFocus);
    navList_->addItem(QStringLiteral("🎤   Звук"));
    navList_->addItem(QStringLiteral("⌨️   Управление"));
    navList_->addItem(QStringLiteral("🪟   Оверлей"));
    navList_->addItem(QStringLiteral("🌐   Соединение"));
    navList_->addItem(QStringLiteral("🎮   Сервер / RP"));
    navList_->addItem(QStringLiteral("👤   Аккаунт"));
    navList_->addItem(QStringLiteral("🐞   Отладка"));
    sl->addWidget(navList_, 1);

    auto *ver = new QLabel(QStringLiteral("RP-ассистент · v%1").arg(QStringLiteral(APP_VERSION)), side);
    ver->setObjectName(QStringLiteral("dlgVer"));
    sl->addWidget(ver, 0);

    bl->addWidget(side, 0);

    stack_ = new QStackedWidget(body);
    bl->addWidget(stack_, 1);

    cl->addWidget(body, 1);

    // Страницы (порядок = порядок пунктов навигации).
    buildSoundPage();
    buildControlPage();
    buildOverlayPage();
    buildConnPage();
    buildServerPage();
    buildAccountPage();
    buildDebugPage();

    connect(navList_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
    navList_->setCurrentRow(0);

    // --- Футер: пилюля сервера + Обновить + Готово ---
    auto *foot = new QWidget(contentRoot_);
    foot->setObjectName(QStringLiteral("dlgFoot"));
    foot->setFixedHeight(54);
    auto *fl = new QHBoxLayout(foot);
    fl->setContentsMargins(16, 0, 16, 0);
    fl->setSpacing(10);

    footServerPill_ = new QLabel(foot);
    footServerPill_->setObjectName(QStringLiteral("dlgServerPill"));
    footServerPill_->setVisible(false);
    fl->addWidget(footServerPill_, 0);
    fl->addStretch(1);

    auto *refreshBtn = new QPushButton(QStringLiteral("⟳  Обновить"), foot);
    refreshBtn->setObjectName(QStringLiteral("secondary"));
    connect(refreshBtn, &QPushButton::clicked, this, &SettingsDialog::onRefresh);
    fl->addWidget(refreshBtn, 0);

    auto *doneBtn = new QPushButton(QStringLiteral("Готово"), foot);
    connect(doneBtn, &QPushButton::clicked, this, &QDialog::accept);
    fl->addWidget(doneBtn, 0);

    cl->addWidget(foot);

    windowUiReady_ = true;
    setMinimumSize(660, 520);
    resize(700, 560);
}

// Каркас одной страницы: прокручиваемый контейнер + вертикальный layout + crimson-заголовок.
//
// ⚠️ Страница ОБЯЗАНА быть прокручиваемой. Без этого при нехватке высоты окна layout давит
// виджеты ниже их минимума, и они налезают друг на друга: на «Управлении» (4 хоткея + PTT +
// целый блок модели распознавания с путём и тремя кнопками) кнопки перекрывали подписи.
// Со скроллом лишняя высота честно уезжает в полосу прокрутки.
QWidget *SettingsDialog::makePage(const QString &sectionTitle, QVBoxLayout *&outLayout)
{
    auto *page = new QWidget;            // родителя даст scroll area (setWidget)
    page->setObjectName(QStringLiteral("dlgPage"));
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(20, 18, 20, 18);
    lay->setSpacing(8);

    auto *sec = new QLabel(sectionTitle, page);
    sec->setObjectName(QStringLiteral("dlgSection"));
    lay->addWidget(sec);

    auto *scroll = new QScrollArea(stack_);
    scroll->setObjectName(QStringLiteral("dlgScroll"));
    scroll->setWidgetResizable(true);                                  // страница тянется по ширине
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);      // режем только по вертикали
    scroll->setWidget(page);

    stack_->addWidget(scroll);
    outLayout = lay;
    return page;
}

// ---------------------------------------------------------------------------
//  Страница «Звук»: микрофон + источник звука игры + живые VU обоих.
// ---------------------------------------------------------------------------
void SettingsDialog::buildSoundPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("ЗВУК"), lay);

    lay->addWidget(fieldLabel(page, QStringLiteral("Микрофон")));
    micCombo_ = new QComboBox(page);
    lay->addWidget(micCombo_);
    connect(micCombo_, SIGNAL(activated(int)), this, SLOT(onMicActivated(int)));
    micLevel_ = new QProgressBar(page);
    micLevel_->setObjectName(QStringLiteral("vuBar"));
    micLevel_->setRange(0, 100); micLevel_->setValue(0);
    micLevel_->setTextVisible(false); micLevel_->setFixedHeight(8);
    lay->addWidget(micLevel_);

#ifndef RPOVERLAY_QA
    // «Источник звука игры» — только для сценовых режимов (Адвокат/Прокурор/Общий/Обзвон).
    // В публичном Q&A-варианте режимов нет → секцию не показываем (Q&A слышит только микрофон).
    lay->addSpacing(4);
    lay->addWidget(fieldLabel(page, QStringLiteral("Источник звука игры")));
    gameCombo_ = new QComboBox(page);
    lay->addWidget(gameCombo_);
    connect(gameCombo_, SIGNAL(activated(int)), this, SLOT(onGameActivated(int)));
    gameLevel_ = new QProgressBar(page);
    gameLevel_->setObjectName(QStringLiteral("vuBar"));
    gameLevel_->setRange(0, 100); gameLevel_->setValue(0);
    gameLevel_->setTextVisible(false); gameLevel_->setFixedHeight(8);
    lay->addWidget(gameLevel_);

    auto *hint = new QLabel(
        QStringLiteral("Для сценовых режимов (Адвокат/Прокурор/Общий) — звук игры слушается "
                       "и разделяется по голосам. Q&A слышит только микрофон."),
        page);
    hint->setObjectName(QStringLiteral("dlgHint"));
    hint->setWordWrap(true);
    lay->addSpacing(4);
    lay->addWidget(hint);
#endif
    lay->addStretch(1);
}

namespace {
#ifdef Q_OS_WIN
// QKeySequenceEdit, снимающий ФИЗИЧЕСКУЮ клавишу по VK/scan-code, а не по символу раскладки.
// Штатный виджет берёт символ: на RU-раскладке Shift+3 = «№» → hk::tokenToVk даёт 0 → хоткей
// молча не регистрируется. Здесь клавиша нормализуется в латинский hk-токен (напр. «Shift+3»)
// и кладётся в property("hkToken"); отображение — через QKeySequence(токен). Backspace/Delete —
// очистка поля («отключить»), как у штатного виджета.
class VkKeySequenceEdit : public QKeySequenceEdit {
public:
    using QKeySequenceEdit::QKeySequenceEdit;
protected:
    void keyPressEvent(QKeyEvent *e) override {
        const int k = e->key();
        if (k == Qt::Key_Backspace || k == Qt::Key_Delete) {   // очистка = «отключить»
            setProperty("hkToken", QString());
            clear();
            e->accept();
            return;
        }
        if (k == Qt::Key_unknown || k == Qt::Key_Control || k == Qt::Key_Shift ||
            k == Qt::Key_Alt || k == Qt::Key_AltGr || k == Qt::Key_Meta ||
            k == Qt::Key_Super_L || k == Qt::Key_Super_R) {
            QKeySequenceEdit::keyPressEvent(e);   // ждём саму клавишу, модификаторы прокидываем
            return;
        }
        unsigned vk = e->nativeVirtualKey();
        if (vk == 0 || hk::vkToToken(vk).isEmpty()) {
            const unsigned mapped = MapVirtualKeyW(e->nativeScanCode(), MAPVK_VSC_TO_VK);
            if (mapped != 0) vk = mapped;
        }
        if (vk == 0 || hk::vkToToken(vk).isEmpty()) { e->accept(); return; }  // не поддерживаем — глотаем
        unsigned mods = 0;
        const Qt::KeyboardModifiers m = e->modifiers();
        if (m & Qt::ControlModifier) mods |= hk::Ctrl;
        if (m & Qt::ShiftModifier)   mods |= hk::Shift;
        if (m & Qt::AltModifier)     mods |= hk::Alt;
        if (m & Qt::MetaModifier)    mods |= hk::Win;
        const QString token = hk::format(mods, vk);
        setProperty("hkToken", token);
        setKeySequence(QKeySequence(token));   // визуальный фидбэк + keySequenceChanged → автосейв
        e->accept();
    }
};
#endif

// Токен хоткея из поля: если клавиша снята по VK (VkKeySequenceEdit кладёт латинский токен в
// property "hkToken") — берём его; иначе штатный PortableText (обратная совместимость с уже
// загруженным/не тронутым значением).
static QString hkEditToken(const QKeySequenceEdit *ed)
{
    if (!ed) return QString();
    const QVariant p = ed->property("hkToken");
    if (p.isValid() && !p.toString().isEmpty()) return p.toString();
    return ed->keySequence().toString(QKeySequence::PortableText);
}
}  // namespace

// ---------------------------------------------------------------------------
//  Страница «Управление»: хоткеи + Push-to-talk.
// ---------------------------------------------------------------------------
void SettingsDialog::buildControlPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("ГОРЯЧИЕ КЛАВИШИ"), lay);
    const WindowSettings &ws = settings::window();

    auto addHk = [&](const QString &cap, const QString &seq) -> QKeySequenceEdit * {
        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 2, 0, 2);
        auto *l = new QLabel(cap, page);
        l->setObjectName(QStringLiteral("dlgField"));
        row->addWidget(l, 1);
#ifdef Q_OS_WIN
        auto *ed = new VkKeySequenceEdit(QKeySequence(seq), page);   // захват по VK/scan (RU-раскладка)
#else
        auto *ed = new QKeySequenceEdit(QKeySequence(seq), page);
#endif
        ed->setMaximumSequenceLength(1);
        ed->setMaximumWidth(150);
        row->addWidget(ed, 0);
        lay->addLayout(row);
        connect(ed, &QKeySequenceEdit::keySequenceChanged, this, &SettingsDialog::onWindowChanged);
        return ed;
    };
    hkToggleEdit_   = addHk(QStringLiteral("Показать / скрыть"),            ws.hkToggle);
    hkClickEdit_    = addHk(QStringLiteral("Сквозь клики"),                 ws.hkClickThrough);
    hkAskEdit_      = addHk(QStringLiteral("Ввод вопроса с клавиатуры"),    ws.hkAsk);
    hkCollapseEdit_ = addHk(QStringLiteral("Скрыть/показать ответ"),        ws.hkCollapse);

    auto *pttSec = new QLabel(QStringLiteral("PUSH-TO-TALK (УДЕРЖИВАТЬ)"), page);
    pttSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(10);
    lay->addWidget(pttSec);

    {
        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 2, 0, 2);
        auto *l = new QLabel(QStringLiteral("Текущая привязка"), page);
        l->setObjectName(QStringLiteral("dlgField"));
        row->addWidget(l, 1);
        pttBindLabel_ = new QLabel(ws.hkPushToTalk.isEmpty() ? QStringLiteral("—") : ws.hkPushToTalk, page);
        pttBindLabel_->setObjectName(QStringLiteral("dlgKeyCap"));
        pttBindLabel_->setAlignment(Qt::AlignCenter);
        row->addWidget(pttBindLabel_, 0);
        pttCaptureBtn_ = new QPushButton(QStringLiteral("Изменить"), page);
        pttCaptureBtn_->setObjectName(QStringLiteral("secondary"));
        connect(pttCaptureBtn_, &QPushButton::clicked, this, &SettingsDialog::onPttCaptureStart);
        row->addWidget(pttCaptureBtn_, 0);
        lay->addLayout(row);
    }

    auto *hint = new QLabel(
        QStringLiteral("Можно назначить клавишу, цифру numpad или кнопку мыши."), page);
    hint->setObjectName(QStringLiteral("dlgHint"));
    hint->setWordWrap(true);
    lay->addSpacing(4);
    lay->addWidget(hint);

#ifndef RPOVERLAY_QA
    // --- Локальный разбор сцены + авто-советы — ТОЛЬКО сценовые режимы. В Q&A-варианте скрыто. ---
    // --- Локальный разбор сцены (диаризация + STT на ПК; режимы Адвокат/Прокурор/Общий) ---
    auto *locSec = new QLabel(QStringLiteral("ЛОКАЛЬНЫЙ РАЗБОР СЦЕНЫ"), page);
    locSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(10);
    lay->addWidget(locSec);
    hkLocalRecEdit_ = addHk(QStringLiteral("Слушать сцену: вкл/пауза"), ws.hkLocalRecord);
    hkLocalAnsEdit_ = addHk(QStringLiteral("Разобрать сцену сейчас"),   ws.hkLocalAnswer);

    // Длина анализируемого окна, сек (5..120).
    lay->addSpacing(4);
    {
        auto *l = new QLabel(QStringLiteral("Глубина сцены, сек"), page);
        l->setObjectName(QStringLiteral("dlgField"));
        lay->addWidget(l);
        auto *row = new QHBoxLayout();
        localWinSlider_ = new QSlider(Qt::Horizontal, page);
        localWinSlider_->setRange(5, 120);
        localWinSlider_->setValue(qBound(5, ws.localWindowSec, 120));
        row->addWidget(localWinSlider_, 1);
        localWinVal_ = new QLabel(QStringLiteral("%1 с").arg(localWinSlider_->value()), page);
        localWinVal_->setObjectName(QStringLiteral("dlgSliderVal"));
        localWinVal_->setMinimumWidth(44);
        localWinVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(localWinVal_, 0);
        lay->addLayout(row);
        connect(localWinSlider_, &QSlider::valueChanged, this, [this](int v) {
            if (localWinVal_) localWinVal_->setText(QStringLiteral("%1 с").arg(v));
            onWindowChanged();   // через дебаунс → commitWindowSettings
        });
    }

    // Порог разделения спикеров (диаризация). Ниже → больше спикеров, выше → меньше.
    lay->addSpacing(4);
    {
        auto *l = new QLabel(QStringLiteral("Разделение спикеров"), page);
        l->setObjectName(QStringLiteral("dlgField"));
        lay->addWidget(l);
        auto *row = new QHBoxLayout();
        auto *thrSlider = new QSlider(Qt::Horizontal, page);
        // /100 = 0.60..1.15 (дефолт 1.10). Верх срезан с 1.60: при пороге ≥1.2 диаризатор sherpa падает
        // access-violation на реальной многоголосой сцене (бэкстоп — SherpaPipeline::diarizeGuarded); к тому
        // же выше дефолта слайдер лишь сильнее СЛИВАЕТ спикеров в одного — полезное направление вниз.
        thrSlider->setRange(60, 115);
        thrSlider->setValue(qBound(60, static_cast<int>(settings::localDiarThreshold() * 100 + 0.5), 115));
        row->addWidget(thrSlider, 1);
        auto *thrVal = new QLabel(QString::number(thrSlider->value() / 100.0, 'f', 2), page);
        thrVal->setObjectName(QStringLiteral("dlgSliderVal"));
        thrVal->setMinimumWidth(44);
        thrVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(thrVal, 0);
        lay->addLayout(row);
        // Пишем сразу, без сигнала (как раньше): пайплайн перечитает порог при следующей сцене.
        connect(thrSlider, &QSlider::valueChanged, this, [thrVal](int v) {
            if (thrVal) thrVal->setText(QString::number(v / 100.0, 'f', 2));
            settings::setLocalDiarThreshold(v / 100.0);
        });
    }
    auto *thrHint = new QLabel(
        QStringLiteral("Если двух собеседников слепило в одного — снизь; если одного раздробило на двух — "
                       "немного повысь. По умолчанию 1.10."), page);
    thrHint->setObjectName(QStringLiteral("dlgHint"));
    thrHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(thrHint);

    auto *locHint = new QLabel(
        QStringLiteral("«Глубина сцены» — сколько последних секунд диалога уходит в разбор на КАЖДЫЙ совет "
                       "(контекст из нескольких реплик, а не «обрабатывать всё подряд»). Меньше — быстрее и "
                       "точнее по последней реплике; больше — больше контекста. «Разделение спикеров» — "
                       "порог диаризации."), page);
    locHint->setObjectName(QStringLiteral("dlgHint"));
    locHint->setWordWrap(true);
    lay->addSpacing(4);
    lay->addWidget(locHint);

    // --- Авто-советы (Cluely-режим): чувствительность + модель. Старт/стоп — кнопкой ▶ на баре / хоткеем. ---
    auto *autoSec = new QLabel(QStringLiteral("АВТО-СОВЕТЫ (СЦЕНА)"), page);
    autoSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(12);
    lay->addWidget(autoSec);

    {
        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 2, 0, 2);
        auto *l = new QLabel(QStringLiteral("Чувствительность"), page);
        l->setObjectName(QStringLiteral("dlgField"));
        row->addWidget(l, 1);
        auto *sens = new QComboBox(page);
        sens->addItem(QStringLiteral("Тихо"),    QStringLiteral("quiet"));
        sens->addItem(QStringLiteral("Норма"),   QStringLiteral("normal"));
        sens->addItem(QStringLiteral("Активно"), QStringLiteral("active"));
        const int si = sens->findData(settings::autoAdviceSensitivity());
        sens->setCurrentIndex(si >= 0 ? si : 1);
        sens->setMaximumWidth(150);
        row->addWidget(sens, 0);
        lay->addLayout(row);
        connect(sens, &QComboBox::currentIndexChanged, this, [this, sens](int) {
            settings::setAutoAdviceSensitivity(sens->currentData().toString());
            emit autoAdviceChanged();   // OverlayWindow переконфигурирует эндпоинтер под новый пресет
        });
    }

    {
        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 2, 0, 2);
        auto *l = new QLabel(QStringLiteral("Модель совета (A/B)"), page);
        l->setObjectName(QStringLiteral("dlgField"));
        row->addWidget(l, 1);
        auto *mdl = new QComboBox(page);
        mdl->addItem(QStringLiteral("Авто (сервер)"), QStringLiteral("auto"));
        mdl->addItem(QStringLiteral("Flash"),         QStringLiteral("flash"));
        mdl->addItem(QStringLiteral("Flash-lite"),    QStringLiteral("lite"));
        const int mi = mdl->findData(settings::adviceModelTier());
        mdl->setCurrentIndex(mi >= 0 ? mi : 0);
        mdl->setMaximumWidth(150);
        row->addWidget(mdl, 0);
        lay->addLayout(row);
        connect(mdl, &QComboBox::currentIndexChanged, this, [mdl](int) {
            settings::setAdviceModelTier(mdl->currentData().toString());
        });
    }

    auto *autoHint = new QLabel(
        QStringLiteral("В режимах Адвокат/Прокурор/Общий оверлей сам слушает звук игры и выдаёт совет, когда "
                       "собеседник договорил. Старт/стоп — кнопкой ▶ на баре или «%1»; «%2» — разобрать сейчас. "
                       "Тихо — реже и только по явным репликам; Активно — реагирует чаще. "
                       "Модель A/B: сравни Flash и Flash-lite на качестве/скорости.")
            .arg(ws.hkLocalRecord, ws.hkLocalAnswer), page);
    autoHint->setObjectName(QStringLiteral("dlgHint"));
    autoHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(autoHint);
#endif  // RPOVERLAY_QA — конец сценовых секций (локальный разбор + авто-советы)

    // --- Модель распознавания (локальный STT): статус + докачка ПО КНОПКЕ (не авто) ---
    auto *engSec = new QLabel(QStringLiteral("МОДЕЛЬ РАСПОЗНАВАНИЯ"), page);
    engSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(12);
    lay->addWidget(engSec);

    modelStatusLbl_ = new QLabel(QStringLiteral("…"), page);
    modelStatusLbl_->setObjectName(QStringLiteral("dlgField"));
    modelStatusLbl_->setWordWrap(true);
    lay->addWidget(modelStatusLbl_);

    modelProgress_ = new QProgressBar(page);
    modelProgress_->setRange(0, 100);
    modelProgress_->setValue(0);
    modelProgress_->setVisible(false);
    lay->addWidget(modelProgress_);

    {
        auto *row = new QHBoxLayout();
        modelDlBtn_ = new QPushButton(QStringLiteral("Скачать модели"), page);
        connect(modelDlBtn_, &QPushButton::clicked, this, [this] { if (models_) models_->startDownload(); });
        row->addWidget(modelDlBtn_, 1);
        modelDelBtn_ = new QPushButton(QStringLiteral("Удалить"), page);
        modelDelBtn_->setObjectName(QStringLiteral("secondary"));
        connect(modelDelBtn_, &QPushButton::clicked, this, [this] { if (models_) models_->deleteModels(); });
        row->addWidget(modelDelBtn_, 0);
        lay->addLayout(row);
    }

    // --- Каталог моделей: по умолчанию папка установки; можно сменить/открыть/сбросить ---
    modelPathVal_ = new QLabel(page);
    modelPathVal_->setObjectName(QStringLiteral("dlgHint"));
    modelPathVal_->setWordWrap(true);
    modelPathVal_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addSpacing(4);
    lay->addWidget(modelPathVal_);
    {
        auto *prow = new QHBoxLayout();
        auto *changeBtn = new QPushButton(QStringLiteral("Изменить папку…"), page);
        changeBtn->setObjectName(QStringLiteral("secondary"));
        connect(changeBtn, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Папка для моделей распознавания"), ModelManager::downloadDir());
            if (dir.isEmpty()) return;
            settings::setModelsPath(dir);   // модели НЕ переносятся: если папка пуста — статус «Не скачано»
            refreshModelUi();               // (юзер сам нажмёт «Скачать»)
        });
        prow->addWidget(changeBtn, 1);
        auto *openBtn = new QPushButton(QStringLiteral("Открыть"), page);
        openBtn->setObjectName(QStringLiteral("secondary"));
        connect(openBtn, &QPushButton::clicked, this, [] {
            const QString d = ModelManager::downloadDir();
            QDir().mkpath(d);
            QDesktopServices::openUrl(QUrl::fromLocalFile(d));
        });
        prow->addWidget(openBtn, 0);
        auto *resetBtn = new QPushButton(QStringLiteral("По умолчанию"), page);
        resetBtn->setObjectName(QStringLiteral("secondary"));
        connect(resetBtn, &QPushButton::clicked, this, [this] {
            settings::setModelsPath(QString());   // "" → папка установки
            refreshModelUi();
        });
        prow->addWidget(resetBtn, 0);
        lay->addLayout(prow);
    }

    auto *engHint = new QLabel(
        QStringLiteral("Нужна для голосового вопроса (PTT) и разбора сцены. "
                       "Текстовый ввод работает без неё. По умолчанию модель качается в папку "
                       "установки; сменишь папку — модель скачается в новую (старую можно удалить)."), page);
    engHint->setObjectName(QStringLiteral("dlgHint"));
    engHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(engHint);
    refreshModelUi();   // начальное состояние (models_ зададут через setModelManager)

    lay->addStretch(1);
}

void SettingsDialog::setModelManager(ModelManager *mm)
{
    models_ = mm;
    if (models_) {
        connect(models_, &ModelManager::statusChanged, this,
                [this](int, const QString &) { refreshModelUi(); });
        connect(models_, &ModelManager::progress, this,
                [this](qint64 rec, qint64 tot, int fi, int fc) {
                    if (!modelProgress_ || tot <= 0) return;
                    modelProgress_->setVisible(true);
                    modelProgress_->setValue(static_cast<int>(rec * 100 / tot));
                    modelProgress_->setFormat(QStringLiteral("%1 / %2 МБ  (файл %3/%4)")
                                                  .arg(rec / 1024 / 1024).arg(tot / 1024 / 1024)
                                                  .arg(fi + 1).arg(fc));
                });
    }
    refreshModelUi();
}

void SettingsDialog::refreshModelUi()
{
    if (modelPathVal_) {
        const bool custom = !settings::modelsPath().isEmpty();
        modelPathVal_->setText(QStringLiteral("📁 %1%2")
            .arg(QDir::toNativeSeparators(ModelManager::downloadDir()),
                 custom ? QStringLiteral("  (свой путь)") : QStringLiteral("  (папка установки)")));
    }
    if (!modelStatusLbl_) return;
    if (!models_) { modelStatusLbl_->setText(QStringLiteral("…")); return; }
    const QString mb = QString::number(models_->totalBytes() / 1024.0 / 1024.0, 'f', 0);

    auto setBtns = [this](const QString &dlText, bool dlEnabled, bool showDel, bool showProg) {
        if (modelDlBtn_)   { modelDlBtn_->setText(dlText); modelDlBtn_->setEnabled(dlEnabled); }
        if (modelDelBtn_)  modelDelBtn_->setVisible(showDel);
        if (modelProgress_) modelProgress_->setVisible(showProg);
    };

    switch (models_->status()) {
    case ModelManager::Status::Ready:
        modelStatusLbl_->setText(QStringLiteral("✅ Готово (%1 МБ)").arg(mb));
        setBtns(QStringLiteral("Перекачать"), true, /*del*/true, /*prog*/false);
        break;
    case ModelManager::Status::Downloading:
        modelStatusLbl_->setText(QStringLiteral("⏳ Скачиваю…"));
        setBtns(QStringLiteral("Скачать модели"), false, /*del*/false, /*prog*/true);
        break;
    case ModelManager::Status::Error:
        modelStatusLbl_->setText(QStringLiteral("⚠️ Ошибка загрузки — повтори."));
        setBtns(QStringLiteral("Повторить"), true, /*del*/false, /*prog*/false);
        break;
    default:  // NotReady
        modelStatusLbl_->setText(QStringLiteral("Не скачано (~%1 МБ)").arg(mb));
        setBtns(QStringLiteral("Скачать модели"), true, /*del*/false, /*prog*/false);
        break;
    }
}

// ---------------------------------------------------------------------------
//  Страница «Оверлей»: прозрачность, размер шрифта, монитор, сброс позиции,
//  невидимость для записи экрана/OBS.
// ---------------------------------------------------------------------------
void SettingsDialog::buildOverlayPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("ОКНО ОВЕРЛЕЯ"), lay);
    const WindowSettings &ws = settings::window();

    // Прозрачность (0.40..1.00).
    lay->addWidget(fieldLabel(page, QStringLiteral("Прозрачность")));
    {
        auto *row = new QHBoxLayout();
        opacitySlider_ = new QSlider(Qt::Horizontal, page);
        opacitySlider_->setRange(40, 100);
        opacitySlider_->setValue(qBound(40, static_cast<int>(ws.opacity * 100.0 + 0.5), 100));
        row->addWidget(opacitySlider_, 1);
        opacityVal_ = new QLabel(QStringLiteral("%1%").arg(opacitySlider_->value()), page);
        opacityVal_->setObjectName(QStringLiteral("dlgSliderVal"));
        opacityVal_->setMinimumWidth(44);
        opacityVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(opacityVal_, 0);
        lay->addLayout(row);
        connect(opacitySlider_, &QSlider::valueChanged, this, [this](int v) {
            if (opacityVal_) opacityVal_->setText(QStringLiteral("%1%").arg(v));
            onWindowChanged();
        });
    }

    // Размер шрифта, % (80..180).
    lay->addSpacing(4);
    lay->addWidget(fieldLabel(page, QStringLiteral("Размер шрифта")));
    {
        auto *row = new QHBoxLayout();
        fontSlider_ = new QSlider(Qt::Horizontal, page);
        fontSlider_->setRange(80, 180);
        fontSlider_->setSingleStep(10);
        fontSlider_->setPageStep(10);
        fontSlider_->setValue(qBound(80, ws.fontScalePct, 180));
        row->addWidget(fontSlider_, 1);
        fontVal_ = new QLabel(QStringLiteral("%1%").arg(fontSlider_->value()), page);
        fontVal_->setObjectName(QStringLiteral("dlgSliderVal"));
        fontVal_->setMinimumWidth(44);
        fontVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(fontVal_, 0);
        lay->addLayout(row);
        connect(fontSlider_, &QSlider::valueChanged, this, [this](int v) {
            const int snapped = (v / 10) * 10;   // шаг 10%
            if (snapped != v) { QSignalBlocker b(fontSlider_); fontSlider_->setValue(snapped); }
            if (fontVal_) fontVal_->setText(QStringLiteral("%1%").arg(snapped));
            onWindowChanged();
        });
    }

    // Монитор.
    lay->addSpacing(4);
    lay->addWidget(fieldLabel(page, QStringLiteral("Монитор")));
    monitorCombo_ = new QComboBox(page);
    monitorCombo_->addItem(QStringLiteral("Основной / авто"), -1);
    const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i)
        monitorCombo_->addItem(QStringLiteral("Монитор %1 — %2").arg(i + 1).arg(screens.at(i)->name()), i);
    {
        const int mi = monitorCombo_->findData(ws.monitorIndex);
        monitorCombo_->setCurrentIndex(mi >= 0 ? mi : 0);
    }
    lay->addWidget(monitorCombo_);
    connect(monitorCombo_, SIGNAL(activated(int)), this, SLOT(onWindowChanged()));

    auto *resetBtn = new QPushButton(QStringLiteral("Сбросить позицию"), page);
    resetBtn->setObjectName(QStringLiteral("secondary"));
    connect(resetBtn, &QPushButton::clicked, this, &SettingsDialog::onResetGeometry);
    lay->addSpacing(8);
    lay->addWidget(resetBtn);

    // --- Запуск (0.4.0: первым открывается окно-хаб; тумблер возвращает старое поведение) ---
    // Читает/пишет напрямую settings::overlayAutostart (не часть WindowSettings): применяется
    // на СЛЕДУЮЩЕМ старте приложения — main решает, поднимать ли бар сразу.
    auto *launchSec = new QLabel(QStringLiteral("ЗАПУСК"), page);
    launchSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(12);
    lay->addWidget(launchSec);

    auto *autostartChk = new QCheckBox(QStringLiteral("Запускать оверлей сразу при старте"), page);
    autostartChk->setChecked(settings::overlayAutostart());
    lay->addWidget(autostartChk);
    connect(autostartChk, &QCheckBox::toggled, this, [](bool on) {
        settings::setOverlayAutostart(on);
    });

    auto *autostartHint = new QLabel(
        QStringLiteral("Как в старых версиях: игровой бар поднимается сразу вместе с хабом. "
                       "Выключено — при старте открывается только хаб, бар запускается кнопкой из него."), page);
    autostartHint->setObjectName(QStringLiteral("dlgHint"));
    autostartHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(autostartHint);

    // --- Невидимость для захвата экрана (WDA_EXCLUDEFROMCAPTURE; применяет OverlayWindow) ---
    auto *capSec = new QLabel(QStringLiteral("ЗАПИСЬ ЭКРАНА / OBS"), page);
    capSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(12);
    lay->addWidget(capSec);

    captureInvisibleChk_ = new QCheckBox(
        QStringLiteral("Скрывать оверлей из записи экрана и стримов (OBS)"), page);
    captureInvisibleChk_->setChecked(ws.captureInvisible);
    lay->addWidget(captureInvisibleChk_);
    connect(captureInvisibleChk_, &QCheckBox::toggled, this, &SettingsDialog::onWindowChanged);

    auto *capHint = new QLabel(
        QStringLiteral("Оверлей видите только вы — в OBS/записи его нет. "
                       "Выключите, если хотите показать его на стриме."), page);
    capHint->setObjectName(QStringLiteral("dlgHint"));
    capHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(capHint);

#ifndef RPOVERLAY_QA
    // --- Мастер-«Режим невидимка» (только тестерский full-билд). Накрывает собой галку выше:
    //     прячет из захвата ВСЕ окна и попапы + Hub/активацию из панели задач/Alt-Tab. ---
    stealthChk_ = new QCheckBox(
        QStringLiteral("Режим невидимка — скрыть ВСЁ (окна, меню, панель задач, Alt-Tab)"), page);
    stealthChk_->setChecked(ws.stealthMode);
    lay->addSpacing(10);
    lay->addWidget(stealthChk_);
    connect(stealthChk_, &QCheckBox::toggled, this, [this](bool on) {
        // Мастер накрывает «скрывать из OBS»: при включении бар тоже скрыт, галку принудительно
        // ставим и блокируем (она — подмножество режима). При выключении — возвращаем управление.
        if (captureInvisibleChk_) {
            if (on) captureInvisibleChk_->setChecked(true);
            captureInvisibleChk_->setEnabled(!on);
        }
        onWindowChanged();   // дебаунс → commitWindowSettings → windowSettingsChanged (живое применение)
    });
    auto *stealthHint = new QLabel(
        QStringLiteral("Оверлей исчезает из OBS/стримов, скриншотов, панели задач и Alt-Tab — "
                       "остаётся только процессом в Диспетчере задач. Скрытие из захвата работает "
                       "сразу; хаб и окно активации уходят из панели задач после перезапуска.\n"
                       "⚠️ В играх с анти-читом (EAC/BattlEye) скрытые окна поверх игры могут "
                       "привести к бану аккаунта. Включайте на свой риск."), page);
    stealthHint->setObjectName(QStringLiteral("dlgHint"));
    stealthHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(stealthHint);
    // Начальное состояние: мастер уже включён → «скрывать из OBS» задизейблено (подмножество).
    if (ws.stealthMode && captureInvisibleChk_)
        captureInvisibleChk_->setEnabled(false);
#endif

    lay->addStretch(1);
}

// ---------------------------------------------------------------------------
//  Страница «Соединение»: канал связи + авто-обновление.
// ---------------------------------------------------------------------------
void SettingsDialog::buildConnPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("СОЕДИНЕНИЕ"), lay);

    lay->addWidget(fieldLabel(page, QStringLiteral("Канал связи")));
    connModeCombo_ = new QComboBox(page);
    connModeCombo_->addItem(QStringLiteral("Авто (рекомендуется)"), QStringLiteral("auto"));
    connModeCombo_->addItem(QStringLiteral("Основной"),             QStringLiteral("main"));
    connModeCombo_->addItem(QStringLiteral("Резервный (РФ)"),       QStringLiteral("relay"));
    {
        const QString cur = settings::connMode();
        int idx = connModeCombo_->findData(cur);
        connModeCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    lay->addWidget(connModeCombo_);
    connect(connModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const QString mode = connModeCombo_->currentData().toString();
        settings::setConnMode(mode);
        // Применяем БЕЗ перезапуска: chooseChannel выбирает канал под новый режим (для auto —
        // с быстрой API-пробой под модальным диалогом), setActiveChannel подменяет cfg().apiBaseUrl.
        // Клиенты (Advice/Auth) читают cfg() на каждый запрос → новые запросы поедут по новому каналу;
        // активный SSE/WS доживут на прежнем (это нормально — следующий запрос уже на новом).
        setActiveChannel(conn::chooseChannel(mode));
        qInfo().noquote() << "[settings] канал связи сменён вживую → mode=" << mode
                          << "active=" << activeChannel() << "base=" << cfg().apiBaseUrl;
    });

    auto *connHint = new QLabel(
        QStringLiteral("Авто: пробует напрямую, при блокировке — через резервный РФ-сервер. "
                       "Применяется сразу — для активного запроса может потребоваться перезапрос."), page);
    connHint->setObjectName(QStringLiteral("dlgHint"));
    connHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(connHint);

    auto *updSec = new QLabel(QStringLiteral("ОБНОВЛЕНИЯ"), page);
    updSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(12);
    lay->addWidget(updSec);

    autoUpdateChk_ = new QCheckBox(QStringLiteral("Обновлять автоматически при запуске"), page);
    autoUpdateChk_->setChecked(settings::autoUpdateEnabled());
    lay->addWidget(autoUpdateChk_);
    connect(autoUpdateChk_, &QCheckBox::toggled, this, [](bool on) {
        settings::setAutoUpdate(on);
    });
    lay->addStretch(1);
}

// ---------------------------------------------------------------------------
//  Страница «Сервер / RP»: проект → сервер (каскад).
// ---------------------------------------------------------------------------
void SettingsDialog::buildServerPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("СЕРВЕР ЗАКОНОВ"), lay);

    lay->addWidget(fieldLabel(page, QStringLiteral("Проект (игра)")));
    projectCombo_ = new QComboBox(page);
    lay->addWidget(projectCombo_);
    connect(projectCombo_, SIGNAL(activated(int)), this, SLOT(onServerProjectChanged(int)));

    lay->addSpacing(4);
    lay->addWidget(fieldLabel(page, QStringLiteral("Сервер")));
    serverCombo_ = new QComboBox(page);
    lay->addWidget(serverCombo_);
    connect(serverCombo_, SIGNAL(activated(int)), this, SLOT(onServerComboChanged(int)));

    auto *hint = new QLabel(
        QStringLiteral("Сервер выбирается здесь для всех режимов. Совет (Адвокат/Прокурор/Общий) "
                       "работает на серверах со сводом законов — они отмечены «✓ свод». "
                       "Список появляется после входа в оверлей."), page);
    hint->setObjectName(QStringLiteral("dlgHint"));
    hint->setWordWrap(true);
    lay->addSpacing(4);
    lay->addWidget(hint);

#ifndef RPOVERLAY_QA
    // --- Режим при запуске: с каким режимом стартует бар (пилюля на баре меняет его на лету) ---
    // В публичном Q&A-варианте режим залочен на Q&A → выбор не показываем.
    auto *modeSec = new QLabel(QStringLiteral("РЕЖИМ ПРИ ЗАПУСКЕ"), page);
    modeSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(14);
    lay->addWidget(modeSec);
    lay->addWidget(fieldLabel(page, QStringLiteral("Режим")));
    defaultModeCombo_ = new QComboBox(page);
    defaultModeCombo_->addItem(QStringLiteral("Вопрос-ответ"), QStringLiteral("qa"));
    defaultModeCombo_->addItem(QStringLiteral("Адвокат"),      QStringLiteral("advocate"));
    defaultModeCombo_->addItem(QStringLiteral("Прокурор"),     QStringLiteral("prosecutor"));
    defaultModeCombo_->addItem(QStringLiteral("Общий"),        QStringLiteral("general"));
    {
        QString dm = settings::server().defaultMode;
        if (dm.isEmpty()) dm = QStringLiteral("qa");   // пусто = Q&A (новый дефолт 0.3.0)
        const int i = defaultModeCombo_->findData(dm);
        if (i >= 0) defaultModeCombo_->setCurrentIndex(i);
    }
    lay->addWidget(defaultModeCombo_);
    // activated → только пользовательский выбор (без спама при программной установке индекса).
    connect(defaultModeCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        ServerSettings s = settings::server();
        s.defaultMode = defaultModeCombo_->currentData().toString();
        settings::setServer(s);
        emit serverSettingsChanged();   // OverlayWindow применит режим вживую (setMode)
    });
    auto *modeHint = new QLabel(
        QStringLiteral("Адвокат/Прокурор/Общий — разбор сцены локально, совет по своду ВЫБРАННОГО "
                       "сервера (если у него есть свод «✓»). Вопрос-ответ — вопрос голосом (PTT) "
                       "или текстом. Сменить режим можно и пилюлей на самом баре."), page);
    modeHint->setObjectName(QStringLiteral("dlgHint"));
    modeHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(modeHint);
#endif  // RPOVERLAY_QA — «Режим при запуске» скрыт в Q&A-варианте

    // --- Память диалога: сколько последних сообщений сессии учитывать (0..10) ---
    auto *ctxSec = new QLabel(QStringLiteral("ПАМЯТЬ ДИАЛОГА"), page);
    ctxSec->setObjectName(QStringLiteral("dlgSection"));
    lay->addSpacing(14);
    lay->addWidget(ctxSec);

    lay->addWidget(fieldLabel(page, QStringLiteral("Помнить последних сообщений")));
    {
        auto *row = new QHBoxLayout();
        auto *ctxSlider = new QSlider(Qt::Horizontal, page);
        ctxSlider->setRange(0, 10);
        ctxSlider->setSingleStep(1);
        ctxSlider->setPageStep(1);
        ctxSlider->setValue(settings::contextLimit());
        row->addWidget(ctxSlider, 1);
        auto *ctxVal = new QLabel(QString::number(ctxSlider->value()), page);
        ctxVal->setObjectName(QStringLiteral("dlgSliderVal"));
        ctxVal->setMinimumWidth(44);
        ctxVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(ctxVal, 0);
        lay->addLayout(row);
        connect(ctxSlider, &QSlider::valueChanged, this, [ctxVal](int v) {
            if (ctxVal) ctxVal->setText(QString::number(v));
            settings::setContextLimit(v);
        });
    }
    auto *ctxHint = new QLabel(
        QStringLiteral("Сколько последних реплик учитывать в ответах (follow-up-вопросы). "
                       "Максимум 10. 0 — каждый вопрос отдельно, без памяти."), page);
    ctxHint->setObjectName(QStringLiteral("dlgHint"));
    ctxHint->setWordWrap(true);
    lay->addSpacing(2);
    lay->addWidget(ctxHint);

    lay->addStretch(1);
}

// ---------------------------------------------------------------------------
//  Страница «Аккаунт»: выход из Telegram.
// ---------------------------------------------------------------------------
void SettingsDialog::buildAccountPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("АККАУНТ"), lay);

    auto *logoutBtn = new QPushButton(QStringLiteral("🚪  Выйти из аккаунта Telegram"), page);
    logoutBtn->setObjectName(QStringLiteral("secondary"));
    connect(logoutBtn, &QPushButton::clicked, this, &SettingsDialog::onLogout);
    lay->addWidget(logoutBtn);

    auto *accHint = new QLabel(
        QStringLiteral("Удаляет вход на этом ПК и открывает новый вход через Telegram "
                       "(нужно, например, чтобы сменить аккаунт)."), page);
    accHint->setObjectName(QStringLiteral("dlgHint"));
    accHint->setWordWrap(true);
    lay->addSpacing(4);
    lay->addWidget(accHint);
    lay->addStretch(1);
}

// ---------------------------------------------------------------------------
//  Страница «Отладка»: режим отладки + папка логов (для тестеров).
// ---------------------------------------------------------------------------
void SettingsDialog::buildDebugPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("ОТЛАДКА"), lay);

    debugChk_ = new QCheckBox(QStringLiteral("Режим отладки (консоль + лог-файл)"), page);
    debugChk_->setChecked(settings::debugMode());
    lay->addWidget(debugChk_);
    connect(debugChk_, &QCheckBox::toggled, this, [](bool on) {
        settings::setDebugMode(on);
        debuglog::setEnabled(on);   // вживую: открыть/закрыть консоль + лог-файл
    });

    auto *openLogsBtn = new QPushButton(QStringLiteral("📂  Открыть папку логов"), page);
    openLogsBtn->setObjectName(QStringLiteral("secondary"));
    connect(openLogsBtn, &QPushButton::clicked, this, [] { debuglog::openLogDir(); });
    lay->addSpacing(6);
    lay->addWidget(openLogsBtn);

    // Опись окон — для ловли «фантомных» полупрозрачных прямоугольников: они переживают
    // автоматические сторожа, и по описи видно, что это за окно, вместо гадания по скриншоту.
    auto *dumpBtn = new QPushButton(QStringLiteral("🪟  Записать список окон в лог"), page);
    dumpBtn->setObjectName(QStringLiteral("secondary"));
    connect(dumpBtn, &QPushButton::clicked, this, [this] {
        layerfix::dumpWindowInventory();
        QMessageBox::information(this, QStringLiteral("Список окон"),
            QStringLiteral("Список окон дописан в файл:\n%1\n\n"
                           "Записывается всегда, даже если режим отладки выключен.\n\n"
                           "Если на экране висит «фантомный» прямоугольник — нажми эту кнопку, "
                           "пока он виден, и пришли этот файл разработчику.")
                .arg(debuglog::logFilePath()));
    });
    lay->addSpacing(4);
    lay->addWidget(dumpBtn);

    auto *dbgHint = new QLabel(
        QStringLiteral("Включи, повтори проблему, затем пришли разработчику файл "
                       "overlay-debug.log из папки логов. Перезапуск не нужен."), page);
    dbgHint->setObjectName(QStringLiteral("dlgHint"));
    dbgHint->setWordWrap(true);
    lay->addSpacing(4);
    lay->addWidget(dbgHint);
    lay->addStretch(1);
}

// ---------------------------------------------------------------------------
//  Бренд-лого/заголовок под активную тему: в majestic — wordmark + «Настройки»;
//  в graphite — «⚙️ Настройки» без лого.
// ---------------------------------------------------------------------------
void SettingsDialog::applyDialogChrome()
{
    const bool maj = theme::isMajestic();
    if (brandLogo_) brandLogo_->setVisible(maj);
    if (titleText_) titleText_->setText(maj ? QStringLiteral("·  Настройки")
                                            : QStringLiteral("⚙️  Настройки"));
}

void SettingsDialog::updateFooterServer()
{
    if (!footServerPill_) return;
    const QString name = serverCombo_ ? serverCombo_->currentText() : QString();
    if (name.isEmpty()) { footServerPill_->setVisible(false); return; }
    footServerPill_->setVisible(true);
    footServerPill_->setText(QStringLiteral("🟢  Сервер: %1").arg(name));
}

// ---------------------------------------------------------------------------
//  Frameless: перетаскивание за шапку + снятие системной рамки Win11.
// ---------------------------------------------------------------------------
void SettingsDialog::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && header_) {
        const QPoint inHead = header_->mapFromGlobal(e->globalPosition().toPoint());
        if (header_->rect().contains(inHead)) {
            dragging_ = true;
            dragPos_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
            e->accept();
            return;
        }
    }
    QDialog::mousePressEvent(e);
}

void SettingsDialog::mouseMoveEvent(QMouseEvent *e)
{
    if (dragging_ && (e->buttons() & Qt::LeftButton)) {
        move(e->globalPosition().toPoint() - dragPos_);
        update();   // слоёное окно: очистить «хвост» прозрачной рамки при перетаскивании карточки
        e->accept();
        return;
    }
    QDialog::mouseMoveEvent(e);
}

void SettingsDialog::mouseReleaseEvent(QMouseEvent *e)
{
    dragging_ = false;
    QDialog::mouseReleaseEvent(e);
}

void SettingsDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
#ifdef Q_OS_WIN
    // Снять светлую системную рамку Win11 (как у оверлея) — иначе вокруг карточки hairline.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    COLORREF noBorder = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
#endif
    update();   // форс флаша translucent-карточки настроек при показе (симптом «квадрат при открытых настройках»)
}

// ---------------------------------------------------------------------------
//  Сервер / RP: проект → сервер (выбор переехал из главного окна).
// ---------------------------------------------------------------------------
void SettingsDialog::setServerData(const QVariantList &projects, const QString &curServerId)
{
    serverProjects_ = projects;
    curServerId_ = curServerId;
    if (!projectCombo_) return;

    serverUiLoading_ = true;
    {
        QSignalBlocker b(projectCombo_);
        projectCombo_->clear();
        int wantProj = 0;
        for (int i = 0; i < projects.size(); ++i) {
            const QVariantMap pm = projects[i].toMap();
            projectCombo_->addItem(pm.value(QStringLiteral("display_name")).toString(),
                                   pm.value(QStringLiteral("project_id")).toString());
            const QVariantList servs = pm.value(QStringLiteral("servers")).toList();
            for (const QVariant &sv : servs)
                if (sv.toMap().value(QStringLiteral("server_id")).toString() == curServerId) wantProj = i;
        }
        if (projects.isEmpty())
            projectCombo_->addItem(QStringLiteral("(войдите, чтобы загрузить)"), QString());
        if (wantProj < projectCombo_->count())
            projectCombo_->setCurrentIndex(wantProj);
    }
    onServerProjectChanged(projectCombo_->currentIndex());  // заполнить серверы (без save)
    serverUiLoading_ = false;
    selectServerInPicker(curServerId_);   // финальный ре-селект (в т.ч. кросс-проектный, без save)
    updateFooterServer();
}

// Выбрать конкретный server_id в каскаде проект→сервер БЕЗ сайд-эффектов (save/сигналы заблокированы).
void SettingsDialog::selectServerInPicker(const QString &serverId)
{
    if (!projectCombo_ || !serverCombo_ || serverProjects_.isEmpty()) return;
    int wantProj = projectCombo_->currentIndex();
    for (int i = 0; i < serverProjects_.size(); ++i) {
        const QVariantList servs = serverProjects_[i].toMap().value(QStringLiteral("servers")).toList();
        for (const QVariant &sv : servs)
            if (sv.toMap().value(QStringLiteral("server_id")).toString() == serverId) { wantProj = i; break; }
    }
    if (wantProj < 0 || wantProj >= serverProjects_.size()) return;
    { QSignalBlocker b(projectCombo_); if (wantProj < projectCombo_->count()) projectCombo_->setCurrentIndex(wantProj); }
    const QVariantList servs = serverProjects_[wantProj].toMap().value(QStringLiteral("servers")).toList();
    QSignalBlocker b(serverCombo_);
    serverCombo_->clear();
    int sel = 0;
    for (int i = 0; i < servs.size(); ++i) {
        const QVariantMap sm = servs[i].toMap();
        QString disp = sm.value(QStringLiteral("display_name")).toString();
        if (sm.value(QStringLiteral("rag")).toBool()) disp += QStringLiteral("   ✓ свод");
        serverCombo_->addItem(disp, sm.value(QStringLiteral("server_id")).toString());
        if (sm.value(QStringLiteral("server_id")).toString() == serverId) sel = i;
    }
    if (sel < serverCombo_->count()) serverCombo_->setCurrentIndex(sel);
}

void SettingsDialog::onServerProjectChanged(int index)
{
    if (!serverCombo_) return;
    if (index < 0 || index >= serverProjects_.size()) {
        QSignalBlocker b(serverCombo_);
        serverCombo_->clear();
        return;
    }
    const QVariantList servs = serverProjects_[index].toMap().value(QStringLiteral("servers")).toList();
    int sel = 0;
    {
        QSignalBlocker b(serverCombo_);
        serverCombo_->clear();
        for (int i = 0; i < servs.size(); ++i) {
            const QVariantMap sm = servs[i].toMap();
            QString disp = sm.value(QStringLiteral("display_name")).toString();
            if (sm.value(QStringLiteral("rag")).toBool()) disp += QStringLiteral("   ✓ свод");
            serverCombo_->addItem(disp, sm.value(QStringLiteral("server_id")).toString());
            if (sm.value(QStringLiteral("server_id")).toString() == curServerId_) sel = i;
        }
        if (sel < serverCombo_->count())
            serverCombo_->setCurrentIndex(sel);
    }
    onServerComboChanged(serverCombo_->currentIndex());
}

void SettingsDialog::onServerComboChanged(int index)
{
    if (!serverCombo_ || index < 0) { updateFooterServer(); return; }
    if (serverUiLoading_) { updateFooterServer(); return; }  // первичное заполнение — не сохраняем
    ServerSettings s = settings::server();
    if (projectCombo_) s.projectId = projectCombo_->currentData().toString();
    s.serverId = serverCombo_->itemData(index).toString();
    curServerId_ = s.serverId;
    settings::setServer(s);
    updateFooterServer();
    emit serverSettingsChanged();
}

// ---------------------------------------------------------------------------
//  Захват PTT-биндинга: ждём нажатия ЛЮБОЙ клавиши / numpad / кнопки мыши
//  (опрос GetAsyncKeyState — QKeySequenceEdit мышь не умеет).
// ---------------------------------------------------------------------------
void SettingsDialog::onPttCaptureStart()
{
#ifdef Q_OS_WIN
    if (capturing_) return;
    capturing_ = true;
    for (int v = 0; v < 256; ++v)                       // что уже зажато — игнорируем (вкл. ЛКМ от клика)
        captureBaseline_[v] = (GetAsyncKeyState(v) & 0x8000) != 0;
    if (pttCaptureBtn_) pttCaptureBtn_->setText(QStringLiteral("Нажмите… (Esc)"));
    if (!captureTimer_) {
        captureTimer_ = new QTimer(this);
        captureTimer_->setInterval(30);
        connect(captureTimer_, &QTimer::timeout, this, &SettingsDialog::onPttCaptureTick);
    }
    captureTimer_->start();
#endif
}

void SettingsDialog::onPttCaptureTick()
{
#ifdef Q_OS_WIN
    if (!capturing_) { if (captureTimer_) captureTimer_->stop(); return; }
    auto kd = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };

    auto finishCapture = [this](const QString &token) {
        capturing_ = false;
        if (captureTimer_) captureTimer_->stop();
        if (pttCaptureBtn_) pttCaptureBtn_->setText(QStringLiteral("Изменить"));
        if (!token.isEmpty()) {
            if (pttBindLabel_) pttBindLabel_->setText(token);
            WindowSettings ws = settings::window();
            ws.hkPushToTalk = token;
            settings::setWindow(ws);
            emit windowSettingsChanged();   // OverlayWindow перечитает PTT-биндинг
        }
    };

    if (kd(VK_ESCAPE) && !captureBaseline_[VK_ESCAPE]) { finishCapture(QString()); return; }  // отмена

    static const int kSkip[] = { VK_LBUTTON, VK_RBUTTON, VK_CONTROL, VK_SHIFT, VK_MENU,
                                 VK_LWIN, VK_RWIN, VK_LCONTROL, VK_RCONTROL, VK_LSHIFT,
                                 VK_RSHIFT, VK_LMENU, VK_RMENU, VK_ESCAPE };
    for (int v = 0x04; v <= 0xFE; ++v) {
        bool skip = false;
        for (int s : kSkip) if (v == s) { skip = true; break; }
        if (skip) continue;
        if (hk::vkToToken(static_cast<unsigned>(v)).isEmpty()) continue;   // только поддерживаемые клавиши
        if (kd(v) && !captureBaseline_[v]) {
            unsigned mods = 0;
            if (kd(VK_CONTROL)) mods |= hk::Ctrl;
            if (kd(VK_SHIFT))   mods |= hk::Shift;
            if (kd(VK_MENU))    mods |= hk::Alt;
            if (kd(VK_LWIN) || kd(VK_RWIN)) mods |= hk::Win;
            finishCapture(hk::format(mods, static_cast<unsigned>(v)));
            return;
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Микрофон: [«По умолчанию»(data="")] + активные eCapture-устройства (data=id).
// ---------------------------------------------------------------------------
void SettingsDialog::populateMicCombo()
{
    if (!micCombo_) return;

    QSignalBlocker block(micCombo_);  // перезаполнение не должно эмитить activated/изменения
    micCombo_->clear();

    micCombo_->addItem(QStringLiteral("По умолчанию"), QString());

    if (audio_) {
        const auto devices = audio_->enumerateMicDevices();
        for (const auto &d : devices)
            micCombo_->addItem(d.name, d.id);
    }

    const QString savedId = settings::audio().micDeviceId;
    if (savedId.isEmpty()) {
        micCombo_->setCurrentIndex(0);  // «По умолчанию»
        return;
    }

    int idx = micCombo_->findData(savedId);
    if (idx < 0) {
        // Сохранённое устройство сейчас не доступно — показываем его как «(не найден)».
        micCombo_->addItem(QStringLiteral("%1 (не найден)").arg(savedId), savedId);
        idx = micCombo_->count() - 1;
    }
    micCombo_->setCurrentIndex(idx);
}

// ---------------------------------------------------------------------------
// Источник звука игры: [«Весь системный звук»(data="")] + процессы с аудио-сессией.
// ---------------------------------------------------------------------------
void SettingsDialog::populateGameCombo()
{
    if (!gameCombo_) return;

    QSignalBlocker block(gameCombo_);
    gameCombo_->clear();

    gameCombo_->addItem(QStringLiteral("Весь системный звук"), QString());

    if (audio_) {
        const auto procs = audio_->enumerateAudioProcesses();
        for (const auto &p : procs) {
            const QString label = p.title.isEmpty()
                ? p.exe
                : QStringLiteral("%1 — %2").arg(p.exe, p.title);
            gameCombo_->addItem(label, p.exe);
        }
    }

    const QString savedExe = settings::audio().gameProcessExe;
    if (savedExe.isEmpty()) {
        gameCombo_->setCurrentIndex(0);  // «Весь системный звук»
        return;
    }

    int idx = gameCombo_->findData(savedExe);
    if (idx < 0) {
        gameCombo_->addItem(QStringLiteral("%1 (не запущен)").arg(savedExe), savedExe);
        idx = gameCombo_->count() - 1;
    }
    gameCombo_->setCurrentIndex(idx);
}

// ---------------------------------------------------------------------------
void SettingsDialog::onMicActivated(int index)
{
    if (!micCombo_ || index < 0) return;
    const QString id = micCombo_->itemData(index).toString();
    settings::setMicDeviceId(id);   // мутирует + save() внутри
    emit audioSettingsChanged();    // OverlayWindow применит на audio_
    restartPreviewIfOwned();        // под новое устройство — перезапустить наш предпросмотр
}

void SettingsDialog::onGameActivated(int index)
{
    if (!gameCombo_ || index < 0) return;
    const QString exe = gameCombo_->itemData(index).toString();
    settings::setGameProcessExe(exe);
    emit audioSettingsChanged();
    restartPreviewIfOwned();
}

void SettingsDialog::onRefresh()
{
    populateMicCombo();
    populateGameCombo();
}

// ---------------------------------------------------------------------------
// Живой предпросмотр уровней пока окно открыто.
// ---------------------------------------------------------------------------
void SettingsDialog::startPreview()
{
    if (!audio_) return;
    connect(audio_, SIGNAL(level(QString,float)), this, SLOT(onLevel(QString,float)));
    if (!audio_->isRunning()) {
        // Полный захват (мик + loopback игры): VU источника игры снова нужен — сценовые режимы
        // (Адвокат/Прокурор/Общий) слушают звук игры. Крашеопасный двойной process-loopback
        // вылечен в 0.2.19 (нативный float32-формат PL) — micOnly-обход больше не требуется.
        audio_->start(/*micOnly*/false);
        startedPreview_ = true;
    }
}

void SettingsDialog::stopPreview()
{
    if (!audio_) return;
    disconnect(audio_, SIGNAL(level(QString,float)), this, SLOT(onLevel(QString,float)));
    if (startedPreview_) {
        audio_->stop();
        startedPreview_ = false;
    }
}

void SettingsDialog::restartPreviewIfOwned()
{
    if (startedPreview_ && audio_) {
        audio_->stop();
        audio_->start(/*micOnly*/false);   // предпросмотру нужны оба VU — мик и звук игры (см. startPreview)
    }
}

void SettingsDialog::onLevel(const QString &source, float peak)
{
    // mic → VU микрофона; прочее (loopback игры/системы) → VU источника игры.
    QProgressBar *bar = (source == QLatin1String("mic")) ? micLevel_ : gameLevel_;
    if (!bar) return;
    int v = static_cast<int>(peak * 100.0f + 0.5f);
    if (v > 100) v = 100;
    const int decayed = bar->value() - 8;   // лёгкий спад, чтобы видеть пики
    bar->setValue(v > decayed ? v : (decayed > 0 ? decayed : 0));
}

// ---------------------------------------------------------------------------
void SettingsDialog::onLogout()
{
    const auto btn = QMessageBox::question(
        this, QStringLiteral("Выход из аккаунта"),
        QStringLiteral("Выйти из текущего аккаунта Telegram на этом ПК?\n"
                       "Понадобится снова войти через бота."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (btn != QMessageBox::Yes)
        return;
    emit logoutRequested();
    accept();   // закрыть окно настроек — OverlayWindow покажет экран входа
}

void SettingsDialog::onWindowChanged()
{
    if (!windowUiReady_) return;

    // Дебаунс: слайдеры прозрачности/шрифта эмитят valueChanged десятки раз в секунду
    // при перетаскивании — без задержки каждый тик писал бы settings.json на диск и
    // дёргал windowSettingsChanged (а OverlayWindow на него перерегистрирует глобальные
    // хоткеи ×5 — они мерцали бы всё время прокрутки). PTT-захват сюда НЕ ходит —
    // onPttCaptureTick сохраняет и эмитит мгновенно, как раньше.
    if (!windowSaveTimer_) {
        windowSaveTimer_ = new QTimer(this);
        windowSaveTimer_->setSingleShot(true);
        windowSaveTimer_->setInterval(200);
        connect(windowSaveTimer_, &QTimer::timeout, this, &SettingsDialog::commitWindowSettings);
    }
    windowSaveTimer_->start();   // повторный вызов перезапускает отсчёт
}

// Фактическое сохранение настроек окна (вызывается дебаунс-таймером из onWindowChanged
// либо флашем из деструктора — чтобы последние движения слайдеров не терялись при закрытии).
void SettingsDialog::commitWindowSettings()
{
    WindowSettings ws = settings::window();  // геометрию сохраняем как есть
    const int newMon = monitorCombo_ ? monitorCombo_->currentData().toInt() : ws.monitorIndex;
    if (newMon != ws.monitorIndex)
        ws.hasGeometry = false;              // сменили монитор → переложить панель на него
    ws.monitorIndex = newMon;

    if (opacitySlider_) ws.opacity = opacitySlider_->value() / 100.0;
    if (fontSlider_)    ws.fontScalePct = fontSlider_->value();
    // Хоткеи пишем КАК ЕСТЬ, включая пустую строку: пустой = «отключён»
    // (registerHotkeys в окне такой просто пропустит). Раньше пустые игнорировались —
    // юзер стирал поле, а старый глобальный хоткей продолжал работать (UI врал).
    if (hkToggleEdit_)
        ws.hkToggle = hkEditToken(hkToggleEdit_);
    if (hkClickEdit_)
        ws.hkClickThrough = hkEditToken(hkClickEdit_);
    if (hkAskEdit_)
        ws.hkAsk = hkEditToken(hkAskEdit_);
    if (hkCollapseEdit_)
        ws.hkCollapse = hkEditToken(hkCollapseEdit_);
    // hkPushToTalk сохраняется отдельно (кастомный захват, см. onPttCaptureTick).

    // Локальный разбор сцены: хоткеи (пустое поле = выключить) + длина окна.
    if (hkLocalRecEdit_)
        ws.hkLocalRecord = hkEditToken(hkLocalRecEdit_);
    if (hkLocalAnsEdit_)
        ws.hkLocalAnswer = hkEditToken(hkLocalAnsEdit_);
    if (localWinSlider_)
        ws.localWindowSec = localWinSlider_->value();

    if (captureInvisibleChk_)
        ws.captureInvisible = captureInvisibleChk_->isChecked();   // OverlayWindow применит WDA-аффинити
#ifndef RPOVERLAY_QA
    if (stealthChk_)
        ws.stealthMode = stealthChk_->isChecked();   // мастер-«невидимка»: StealthGuard накроет все окна
#endif

    settings::setWindow(ws);
    emit windowSettingsChanged();
}

void SettingsDialog::onResetGeometry()
{
    // Размера у бара нет (ширина фиксированная) — сбрасываем только сохранённую позицию:
    // OverlayWindow вернёт бар наверх по центру выбранного монитора.
    WindowSettings ws = settings::window();
    ws.hasGeometry = false;
    settings::setWindow(ws);
    emit windowSettingsChanged();
}
