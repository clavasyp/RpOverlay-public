// HubWindow.cpp — окно-хаб (лаунчер): главное десктопное окно приложения с 0.4.0.
//
// Появляется первым после активации ключа; игровой бар (OverlayWindow) поднимается кнопкой
// «Запустить оверлей» отсюда (или сразу при старте — тумблер «Запускать оверлей сразу при
// старте» в настройках, старое поведение ≤0.3.x). Хаб владеет единственной иконкой трея
// приложения; ✕ хаба = свернуть в трей, выход — из меню трея. Виринг с main/баром — сигналами
// (launchOverlayRequested / hideOverlayRequested / settingsRequested), сам хаб бар не создаёт.
//
// Поиск по законке (бета): свой LawsClient (отдельный от бара — их отмены/выдачи не должны
// пересекаться), выдача/статья/история/избранное рендерятся HTML'ом в QTextBrowser
// (клики — якорями: art:/q:/fav:/back:/home:/copy:). История и избранное — ЛОКАЛЬНО в
// law_search.json рядом с settings.json, на сервер не шлются. Доступность поиска гейтится
// флагом search у выбранного сервера (/v1/overlay/servers) — динамический гейт беты.

#include "HubWindow.h"
#include "AuthClient.h"
#include "StealthGuard.h"
#include "LawsClient.h"
#include "Config.h"
#include "Settings.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTextBrowser>
#include <QMenu>
#include <QTimer>
#include <QApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QScreen>
#include <QPixmap>
#include <QIcon>
#include <QFrame>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QMouseEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QCloseEvent>
#include <QDebug>

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

namespace {

// Небольшой заголовок поля над контролом (как fieldLabel в SettingsDialog, QSS #dlgField).
QLabel *fieldLabel(QWidget *p, const QString &text)
{
    auto *l = new QLabel(text, p);
    l->setObjectName(QStringLiteral("dlgField"));
    return l;
}

// Серый hint-абзац (QSS #dlgHint) с переносом строк.
QLabel *hintLabel(QWidget *p, const QString &text)
{
    auto *l = new QLabel(text, p);
    l->setObjectName(QStringLiteral("dlgHint"));
    l->setWordWrap(true);
    return l;
}

// Цвета для HTML-рендера (QTextBrowser не красится QSS-палитрой) — под активную тему.
struct HubColors {
    const char *accent;   // фирменный акцент (crimson / индиго)
    const char *text;     // основной текст
    const char *muted;    // приглушённый серый
    const char *dim;      // самый тусклый (метаданные)
};
const HubColors &hubColors()
{
    static const HubColors majestic{"#E81C5A", "#FFFFFF", "#BABAC0", "#7A7A80"};
    static const HubColors graphite{"#8B7CF8", "#E8E8F0", "#9AA0AE", "#6A6F7A"};
    return theme::isMajestic() ? majestic : graphite;
}

QString esc(const QString &s) { return s.toHtmlEscaped(); }

// Заголовок карточки статьи — общий хелпер клиента (title с кодом → article_ref → код → номер).
QString cardCaption(const QVariantMap &m) { return LawsClient::cardCaption(m); }

// Локальное хранилище истории/избранного поиска — рядом с settings.json.
QString lawStorePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/law_search.json");
}

// Хаб-специфичный доп-QSS ПОВЕРХ theme::dialogStyle(): карточка статуса, крупная кнопка
// запуска, поле поиска (QLineEdit в QSS диалога не описан), браузеры выдачи/новостей. Обе темы.
QString hubExtraStyle()
{
    if (theme::isMajestic()) {
        return QStringLiteral(
            "#hubCard { background: #131313; border: 1px solid #2E2E2E; border-radius: 10px; }"
            "#hubStatKey { color: #BABAC0; font-size: 12px; font-weight: 600; }"
            "#hubStatVal { color: #FFFFFF; font-size: 13px; font-weight: 700; }"
            "QPushButton#hubLaunch { font-size: 15px; padding: 12px 20px; border-radius: 8px; }"
            "QLineEdit { background: #1D1D1D; color: #FFFFFF; border: 1px solid #2E2E2E;"
            "            border-radius: 6px; padding: 8px 10px; selection-background-color: #E81C5A; }"
            "QLineEdit:focus { border: 1px solid #E81C5A; }"
            "QLineEdit:disabled { color: #7A7A80; }"
            "#hubStub { color: #BABAC0; font-size: 16px; font-weight: 700; }"
            "#hubLawView, #hubNewsView { background: #131313; color: #BABAC0;"
            "               border: 1px solid #2E2E2E; border-radius: 8px; padding: 6px;"
            "               selection-background-color: #E81C5A; }");
    }
    // Графит (историческая тема) — те же элементы в её палитре.
    return QStringLiteral(
        "#hubCard { background: rgba(255,255,255,0.04); border: 1px solid rgba(255,255,255,0.08);"
        "           border-radius: 10px; }"
        "#hubStatKey { color: #9AA0AE; font-size: 12px; font-weight: 600; }"
        "#hubStatVal { color: #E8E8F0; font-size: 13px; font-weight: 700; }"
        "QPushButton#hubLaunch { font-size: 15px; padding: 12px 20px; border-radius: 8px; }"
        "QLineEdit { background: rgba(255,255,255,0.05); color: #E8E8F0;"
        "            border: 1px solid rgba(255,255,255,0.08); border-radius: 6px; padding: 8px 10px; }"
        "QLineEdit:focus { border: 1px solid rgba(109,94,246,0.55); }"
        "QLineEdit:disabled { color: #6A6F7A; }"
        "#hubStub { color: #9AA0AE; font-size: 16px; font-weight: 700; }"
        "#hubLawView, #hubNewsView { background: rgba(255,255,255,0.04); color: #9AA0AE;"
        "               border: 1px solid rgba(255,255,255,0.06); border-radius: 8px; padding: 6px; }");
}

// Человекочитаемый тир лицензии. Известные значения мапим, незнакомые показываем как есть
// (сервер может добавить новые тиры — хаб не должен их прятать или врать).
QString humanTier(const QString &tier)
{
    if (tier == QLatin1String("tester"))     return QStringLiteral("Тестер");
    if (tier == QLatin1String("subscriber")) return QStringLiteral("Подписка");
    if (tier == QLatin1String("sub"))        return QStringLiteral("Подписка");
    if (tier == QLatin1String("vip"))        return QStringLiteral("VIP");
    if (tier == QLatin1String("unlimited"))  return QStringLiteral("Безлимит");
    return tier;
}

} // namespace

// ---------------------------------------------------------------------------
HubWindow::HubWindow(AuthClient *auth, QWidget *parent)
    : QWidget(parent), auth_(auth)
{
    setWindowTitle(QStringLiteral("RP-ассистент"));
    // Обычное десктопное окно (есть в таскбаре/Alt-Tab — в отличие от Qt::Tool у бара), но
    // в фирменной «плавающей карточке»: frameless + прозрачный фон, карточку рисует #dlgContent.
    // В «режиме невидимка» (тестер-билд) — Qt::Tool: убирает хаб из панели задач, Alt-Tab и
    // списка окон OBS. Флаг фиксируется при создании окна (обычно = старт приложения); живое
    // переключение тумблера подхватится после перезапуска (скрытие из захвата — сразу, фильтром).
    setWindowFlags((stealth::masterOn() ? Qt::Tool : Qt::Window) | Qt::FramelessWindowHint);
    // Маркер для StealthGuard::sweepAllTopLevel(): хаб владеет треем — на живом переключении
    // режима невидимка sweep дёрнет applyStealthTray() и иконка спрячется/вернётся без рестарта.
    setProperty("stealthTrayOwner", true);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(theme::dialogStyle() + hubExtraStyle());

    // Свой LawsClient (не общий с баром: их отмены запросов и сигналы не должны пересекаться).
    laws_ = new LawsClient(auth_, this);
    connect(laws_, &LawsClient::searchReady,  this, &HubWindow::onLawSearchReady);
    connect(laws_, &LawsClient::searchError,  this, &HubWindow::onLawSearchError);
    connect(laws_, &LawsClient::articleReady, this, &HubWindow::onLawArticleReady);
    connect(laws_, &LawsClient::articleError, this, &HubWindow::onLawArticleError);
    connect(laws_, &LawsClient::favoriteRefreshed,     this, &HubWindow::onLawFavoriteRefreshed);
    connect(laws_, &LawsClient::favoriteMissing,       this, &HubWindow::onLawFavoriteMissing);
    connect(laws_, &LawsClient::favoriteRefreshFailed, this, &HubWindow::onLawFavoriteRefreshFailed);
    connect(laws_, &LawsClient::serversReady, this, &HubWindow::onLawServersReady);
    connect(laws_, &LawsClient::serversError, this, [this](const QString &msg) {
        // Гейт остаётся «неизвестно» → поиск разрешён оптимистично (на закрытой бете сервер
        // сам ответит 403, и onLawSearchError закроет гейт). Защёлку снимаем — следующий
        // заход в раздел перезапросит флаги.
        lawServersAsked_ = false;
        qWarning().noquote() << "[hub] флаги поиска не получены:" << msg;
    });
    // Новости — публичная статика (/updates/news.json), Bearer не нужен.
    nam_ = new QNetworkAccessManager(this);

    buildUi();
    setupTray();

    // Статус-панель освежается по таймеру, пока хаб виден (все источники локальные и дешёвые:
    // settings/cfg/auth-геттеры + isVisible бара). Старт/стоп — в showEvent/hideEvent.
    statusTimer_ = new QTimer(this);
    statusTimer_->setInterval(1500);
    connect(statusTimer_, &QTimer::timeout, this, &HubWindow::refreshStatus);

    // Смена статуса авторизации → мгновенно в статус-панель (не ждём тика таймера).
    if (auth_) {
        connect(auth_, &AuthClient::authorized, this,
                [this](const QString &) { refreshStatus(); });
        connect(auth_, &AuthClient::needKey, this, &HubWindow::refreshStatus);
    }

    // Первый показ — по центру основного экрана (frameless-окно позицию само не выберет).
    if (QScreen *scr = QGuiApplication::primaryScreen())
        move(scr->availableGeometry().center() - rect().center());

    refreshStatus();
}

void HubWindow::setOverlayProvider(std::function<QWidget *()> provider)
{
    overlayProvider_ = std::move(provider);
    refreshStatus();
}

// ---------------------------------------------------------------------------
//  Каркас: карточка → (шапка) + (сайдбар + страницы) + (футер). Повторяет SettingsDialog.
// ---------------------------------------------------------------------------
void HubWindow::buildUi()
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

    // --- Шапка (drag-зона): бренд + заголовок + свернуть/закрыть ---
    header_ = new QWidget(contentRoot_);
    header_->setObjectName(QStringLiteral("dlgHead"));
    header_->setFixedHeight(48);
    auto *hl = new QHBoxLayout(header_);
    hl->setContentsMargins(16, 0, 8, 0);
    hl->setSpacing(10);

    auto *brandLogo = new QLabel(header_);
    brandLogo->setObjectName(QStringLiteral("dlgBrandLogo"));
    {
        const QPixmap logo(QStringLiteral(":/assets/majestic_logo.png"));
        if (!logo.isNull() && theme::isMajestic())
            brandLogo->setPixmap(logo.scaledToHeight(16, Qt::SmoothTransformation));
    }
    hl->addWidget(brandLogo, 0);

    auto *title = new QLabel(QStringLiteral("RP-ассистент"), header_);
    title->setObjectName(QStringLiteral("dlgTitle"));
    hl->addWidget(title, 1);

    // «Свернуть» — обычная минимизация в таскбар (в трей уводит ✕/футер).
    auto *minBtn = new QPushButton(QStringLiteral("–"), header_);
    minBtn->setObjectName(QStringLiteral("winClose"));
    minBtn->setToolTip(QStringLiteral("Свернуть"));
    minBtn->setCursor(Qt::PointingHandCursor);
    minBtn->setFixedSize(30, 28);
    hl->addWidget(minBtn, 0);
    connect(minBtn, &QPushButton::clicked, this, &HubWindow::showMinimized);

    auto *closeX = new QPushButton(QStringLiteral("✕"), header_);
    closeX->setObjectName(QStringLiteral("winClose"));
    closeX->setToolTip(QStringLiteral("Свернуть в трей (выход — через меню трея)"));
    closeX->setCursor(Qt::PointingHandCursor);
    closeX->setFixedSize(30, 28);
    hl->addWidget(closeX, 0);
    connect(closeX, &QPushButton::clicked, this, &QWidget::close);   // → closeEvent → в трей

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
    // ⚠️ Пункты и страницы (stack_ ниже) добавляются СИНХРОННО и в ТОМ ЖЕ порядке под ТЕМИ ЖЕ
    // гейтами: row навигации ОБЯЗАН совпадать с index страницы. navRow*_ = индекс пункта на момент
    // добавления (count() до addItem = индекс следующего) — для ленивой инициализации без хардкода.
    navList_->addItem(QStringLiteral("🏠   Главная"));
#ifdef RPOVERLAY_SEARCH
    // Публичный QA — БЕЗ поиска (RPOVERLAY_SEARCH не определён): «Поиск»/«Избранное» вырезаны.
    navRowSearch_ = navList_->count();  navList_->addItem(QStringLiteral("⚖️   Поиск по законке"));
    navRowFav_    = navList_->count();  navList_->addItem(QStringLiteral("⭐   Избранное"));
#endif
#ifndef RPOVERLAY_LAWS
    // laws-билд: только Главная/Поиск/Настройки — Памятки и Новости не для этого продукта.
    navList_->addItem(QStringLiteral("📋   Памятки"));
    navRowNews_ = navList_->count();    navList_->addItem(QStringLiteral("📰   Новости"));
#endif
    navList_->addItem(QStringLiteral("⚙️   Настройки"));
    sl->addWidget(navList_, 1);

    auto *ver = new QLabel(QStringLiteral("RP-ассистент · v%1").arg(qApp->applicationVersion()), side);
    ver->setObjectName(QStringLiteral("dlgVer"));
    sl->addWidget(ver, 0);

    bl->addWidget(side, 0);

    stack_ = new QStackedWidget(body);
    bl->addWidget(stack_, 1);

    cl->addWidget(body, 1);

    // Страницы (порядок = порядок пунктов навигации; гейты те же, что у addItem выше).
    buildHomePage();
#ifdef RPOVERLAY_SEARCH
    buildLawSearchPage();
    buildFavoritesPage();
#endif
#ifndef RPOVERLAY_LAWS
    buildNotesPage();
    buildNewsPage();
#endif
    buildSettingsPage();

    connect(navList_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
    // Ленивая инициализация разделов при первом заходе. Сверяемся с navRow*_ (а не с хардкод-row),
    // т.к. индексы пунктов зависят от варианта билда. navRow*_ = -1 → пункта нет → ветка не сработает.
    connect(navList_, &QListWidget::currentRowChanged, this, [this](int row) {
#ifdef RPOVERLAY_SEARCH
        if (row == navRowSearch_) {   // «Поиск по законке»: история/избранное + гейт беты
            ensureLawStoreLoaded();
            ensureLawServersLoaded();
            updateLawAvailability();
            if (lawArticle_.isEmpty() && lawResults_.isEmpty() && lawQuery_.isEmpty())
                lawRenderHome();
            return;
        }
        if (row == navRowFav_) {   // «Избранное»: список текущего сервера + автообновление снимков
            ensureLawStoreLoaded();
            ensureLawServersLoaded();
            favArticleMode_ = false;   // вход на вкладку — всегда показываем список, не статью
            startFavoritesRefresh();   // сначала запуск (favRefreshing_) — потом рендер с индикатором
            lawRenderFavorites();
            return;
        }
#endif
#ifndef RPOVERLAY_LAWS
        if (row == navRowNews_)   // «Новости»: news.json тянется один раз на сеанс
            ensureNewsLoaded();
#endif
    });
    navList_->setCurrentRow(0);

    // --- Футер: пилюля «Сервер · канал» + «Свернуть в трей» ---
    auto *foot = new QWidget(contentRoot_);
    foot->setObjectName(QStringLiteral("dlgFoot"));
    foot->setFixedHeight(54);
    auto *fl = new QHBoxLayout(foot);
    fl->setContentsMargins(16, 0, 16, 0);
    fl->setSpacing(10);

    footPill_ = new QLabel(foot);
    footPill_->setObjectName(QStringLiteral("dlgServerPill"));
    fl->addWidget(footPill_, 0);
    fl->addStretch(1);

    auto *trayBtn = new QPushButton(QStringLiteral("Свернуть в трей"), foot);
    trayBtn->setObjectName(QStringLiteral("secondary"));
    connect(trayBtn, &QPushButton::clicked, this, &QWidget::close);   // → closeEvent → в трей
    fl->addWidget(trayBtn, 0);

    cl->addWidget(foot);

    setMinimumSize(860, 560);
    resize(880, 580);
}

QWidget *HubWindow::makePage(const QString &sectionTitle, QVBoxLayout *&outLayout)
{
    auto *page = new QWidget(stack_);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(20, 18, 20, 18);
    lay->setSpacing(8);

    auto *sec = new QLabel(sectionTitle, page);
    sec->setObjectName(QStringLiteral("dlgSection"));
    lay->addWidget(sec);

    stack_->addWidget(page);
    outLayout = lay;
    return page;
}

// ---------------------------------------------------------------------------
//  Главная: статус-панель + крупная кнопка запуска бара.
// ---------------------------------------------------------------------------
void HubWindow::buildHomePage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("ГЛАВНАЯ"), lay);

    // Карточка статуса: ключ слева, значение справа (значения заполняет refreshStatus()).
    auto *card = new QWidget(page);
    card->setObjectName(QStringLiteral("hubCard"));
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(16, 14, 16, 14);
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);

    int row = 0;
    auto addStat = [&](const QString &key, QLabel *&val) {
        auto *k = new QLabel(key, card);
        k->setObjectName(QStringLiteral("hubStatKey"));
        val = new QLabel(QStringLiteral("—"), card);
        val->setObjectName(QStringLiteral("hubStatVal"));
        grid->addWidget(k,   row, 0);
        grid->addWidget(val, row, 1);
        ++row;
    };
    addStat(QStringLiteral("Версия"),         verVal_);
    addStat(QStringLiteral("Лицензия"),       tierVal_);
    addStat(QStringLiteral("Сервер законов"), serverVal_);
    addStat(QStringLiteral("Канал связи"),    channelVal_);
    addStat(QStringLiteral("Оверлей"),        overlayVal_);

    lay->addSpacing(4);
    lay->addWidget(card);

    // Крупная кнопка запуска: надпись меняет refreshStatus() по фактическому состоянию бара
    // (не запущен → «Запустить», скрыт → «Показать», видим → «Скрыть»).
    launchBtn_ = new QPushButton(QStringLiteral("▶  Запустить оверлей"), page);
    launchBtn_->setObjectName(QStringLiteral("hubLaunch"));
    launchBtn_->setCursor(Qt::PointingHandCursor);
    launchBtn_->setMinimumHeight(48);
    connect(launchBtn_, &QPushButton::clicked, this, &HubWindow::onLaunchClicked);
    lay->addSpacing(14);
    lay->addWidget(launchBtn_);

    launchHint_ = hintLabel(page, QString());   // текст (с актуальным хоткеем) ставит refreshStatus()
    lay->addSpacing(2);
    lay->addWidget(launchHint_);

    lay->addStretch(1);
}

// ---------------------------------------------------------------------------
//  Поиск по законке (бета): поле + кнопка, выдача/статья/история/избранное — HTML
//  в QTextBrowser (клики якорями). Голосовой поиск — в БАРЕ (режим «Поиск»), тут клавиатура.
// ---------------------------------------------------------------------------
void HubWindow::buildLawSearchPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("ПОИСК ПО ЗАКОНКЕ"), lay);

    lay->addWidget(fieldLabel(page, QStringLiteral("Запрос")));

    auto *row = new QHBoxLayout();
    row->setSpacing(8);
    searchEdit_ = new QLineEdit(page);
    searchEdit_->setPlaceholderText(
        QStringLiteral("Статья, срок или ситуация — например: наказание за угон транспорта"));
    row->addWidget(searchEdit_, 1);
    searchBtn_ = new QPushButton(QStringLiteral("Найти"), page);
    searchBtn_->setCursor(Qt::PointingHandCursor);
    row->addWidget(searchBtn_, 0);
    lay->addLayout(row);
    connect(searchEdit_, &QLineEdit::returnPressed, this, &HubWindow::onLawSubmit);
    connect(searchBtn_, &QPushButton::clicked, this, &HubWindow::onLawSubmit);

    // Плашка бета-гейта: поиск существует только на серверах с search=true (/servers).
    lawGateLbl_ = hintLabel(page,
        QStringLiteral("🔒 Поиск недоступен на этом сервере — бета открывается по мере "
                       "готовности каталогов законов."));
    lawGateLbl_->setVisible(false);
    lay->addWidget(lawGateLbl_);

    lawView_ = new QTextBrowser(page);
    lawView_->setObjectName(QStringLiteral("hubLawView"));
    lawView_->setOpenLinks(false);           // навигацию обрабатываем сами (якоря art:/q:/fav:/…)
    lawView_->setOpenExternalLinks(false);
    connect(lawView_, &QTextBrowser::anchorClicked, this, &HubWindow::onLawAnchor);
    lay->addSpacing(4);
    lay->addWidget(lawView_, 1);
}

// ---------------------------------------------------------------------------
//  Избранное: отдельная вкладка. Список закреплённых статей ТОЛЬКО выбранного сервера
//  (per-server фильтр — без «каши» из 4×17.1 с разных серверов) + автообновление снимков
//  при открытии (см. startFavoritesRefresh). Клики — свои (fav: снять / art: открыть в «Поиске»).
// ---------------------------------------------------------------------------
void HubWindow::buildFavoritesPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("ИЗБРАННОЕ"), lay);

    lawFavView_ = new QTextBrowser(page);
    lawFavView_->setObjectName(QStringLiteral("hubLawView"));
    lawFavView_->setOpenLinks(false);
    lawFavView_->setOpenExternalLinks(false);
    connect(lawFavView_, &QTextBrowser::anchorClicked, this, &HubWindow::onLawFavAnchor);
    lay->addSpacing(4);
    lay->addWidget(lawFavView_, 1);
}

// ---------------------------------------------------------------------------
//  Памятки: заглушка «в разработке».
// ---------------------------------------------------------------------------
void HubWindow::buildNotesPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("ПАМЯТКИ"), lay);

    // TODO(notes): наполнение раздела (краткие шпаргалки по RP-процедурам: задержание,
    // обыск, штрафы, права) — источник контента пока не определён.
    lay->addStretch(2);
    auto *stub = new QLabel(QStringLiteral("🚧  Раздел в разработке"), page);
    stub->setObjectName(QStringLiteral("hubStub"));
    stub->setAlignment(Qt::AlignCenter);
    lay->addWidget(stub);

    auto *hint = hintLabel(page,
        QStringLiteral("Здесь появятся памятки: короткие шпаргалки по игровым процедурам."));
    hint->setAlignment(Qt::AlignCenter);
    lay->addSpacing(4);
    lay->addWidget(hint);
    lay->addStretch(3);
}

// ---------------------------------------------------------------------------
//  Новости: {apiBaseUrl}/updates/news.json — [{date,title,text}]. Кэш на сеанс;
//  файла может не быть (появится позже) → тихо «новостей пока нет».
// ---------------------------------------------------------------------------
void HubWindow::buildNewsPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("НОВОСТИ"), lay);

    newsView_ = new QTextBrowser(page);
    newsView_->setObjectName(QStringLiteral("hubNewsView"));
    newsView_->setOpenLinks(false);
    lay->addSpacing(4);
    lay->addWidget(newsView_, 1);
    renderNews();   // до загрузки — «новостей пока нет» (реальная загрузка при заходе в раздел)
}

void HubWindow::ensureNewsLoaded()
{
    if (newsAsked_)
        return;   // кэш на сеанс: повторные заходы в раздел файл не перетягивают
    newsAsked_ = true;
    if (newsView_) {
        const HubColors &c = hubColors();
        newsView_->setHtml(QStringLiteral("<div style='color:%1'>Загружаю новости…</div>")
                               .arg(QLatin1String(c.muted)));
    }

    QString base = cfg().apiBaseUrl;   // активный канал (main/relay) — как манифесты обновлений
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    // Новости СВОИ на каждый вариант билда: публичный QA и тестерский full анонсируют РАЗНОЕ
    // (напр. поиск/избранное есть только у тестеров — в QA о них писать нельзя). laws-билд вкладки
    // «Новости» не имеет (nav под #ifndef RPOVERLAY_LAWS), сюда не попадает. Файла может не быть →
    // тихо «новостей пока нет». Старые клиенты (≤0.4.6) читают общий news.json — его держим full-контентом.
#if defined(RPOVERLAY_QA)
    const QString newsFile = QStringLiteral("/updates/news-qa.json");
#else
    const QString newsFile = QStringLiteral("/updates/news.json");
#endif
    QNetworkRequest req{QUrl(base + newsFile)};
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(12000);   // короткая статика — не висим

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        newsItems_.clear();
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (doc.isArray())
                newsItems_ = doc.array().toVariantList();
        }
        // 404 (файла ещё нет) / сеть / битый JSON → просто пустой список: страница живёт тихо.
        renderNews();
    });
}

void HubWindow::renderNews()
{
    if (!newsView_)
        return;
    const HubColors &c = hubColors();
    if (newsItems_.isEmpty()) {
        newsView_->setHtml(QStringLiteral(
            "<div style='color:%1'>Новостей пока нет — здесь появятся анонсы обновлений и "
            "изменений.</div>").arg(QLatin1String(c.muted)));
        return;
    }
    QString h;
    for (const QVariant &v : newsItems_) {
        const QVariantMap m = v.toMap();
        h += QStringLiteral(
            "<div style='margin-bottom:12px'>"
            "<span style='color:%1;font-size:11px'>%2</span><br>"
            "<span style='color:%3;font-weight:700'>%4</span><br>"
            "<span style='color:%5'>%6</span></div>")
                 .arg(QLatin1String(c.dim),
                      esc(m.value(QStringLiteral("date")).toString()),
                      QLatin1String(c.text),
                      esc(m.value(QStringLiteral("title")).toString()),
                      QLatin1String(c.muted),
                      esc(m.value(QStringLiteral("text")).toString())
                          .replace(QStringLiteral("\n"), QStringLiteral("<br>")));
    }
    newsView_->setHtml(h);
    update();   // слоёное окно: применить отрисованные новости без ожидания move
}

// ---------------------------------------------------------------------------
//  Настройки: переход в СУЩЕСТВУЮЩЕЕ окно настроек (SettingsDialog открывает main через бар).
// ---------------------------------------------------------------------------
void HubWindow::buildSettingsPage()
{
    QVBoxLayout *lay = nullptr;
    QWidget *page = makePage(QStringLiteral("НАСТРОЙКИ"), lay);

    lay->addWidget(hintLabel(page,
        QStringLiteral("Все настройки — звук, хоткеи, окно бара, соединение, сервер законов, "
                       "аккаунт — живут в отдельном окне (то же, что открывается с бара по ⚙). "
                       "Тумблер «Запускать оверлей сразу при старте» — в разделе «Оверлей».")));

    auto *openBtn = new QPushButton(QStringLiteral("Открыть настройки…"), page);
    openBtn->setCursor(Qt::PointingHandCursor);
    connect(openBtn, &QPushButton::clicked, this, [this] { emit settingsRequested(); });
    lay->addSpacing(8);
    lay->addWidget(openBtn, 0, Qt::AlignLeft);
    lay->addStretch(1);
}

// ---------------------------------------------------------------------------
//  Поиск по законке: данные (история/избранное/гейт) и сетевые обработчики.
// ---------------------------------------------------------------------------
QString HubWindow::currentServerId() const
{
    const ServerSettings &srv = settings::server();
    return srv.serverId.isEmpty() ? cfg().serverId : srv.serverId;
}

void HubWindow::ensureLawStoreLoaded()
{
    if (lawStoreLoaded_)
        return;
    lawStoreLoaded_ = true;
    QFile f(lawStorePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return;   // первого запуска файла нет — норм
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    lawHistory_   = o.value(QStringLiteral("history")).toArray().toVariantList();
    lawFavorites_ = o.value(QStringLiteral("favorites")).toArray().toVariantList();
}

void HubWindow::saveLawStore() const
{
    QJsonObject o;
    o.insert(QStringLiteral("history"), QJsonArray::fromVariantList(lawHistory_));
    o.insert(QStringLiteral("favorites"), QJsonArray::fromVariantList(lawFavorites_));
    QFile f(lawStorePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

void HubWindow::addLawHistory(const QString &q, const QString &serverId)
{
    ensureLawStoreLoaded();
    // Дедуп «тот же запрос на том же сервере» — переносим наверх, не плодим копии.
    for (int i = 0; i < lawHistory_.size(); ++i) {
        const QVariantMap m = lawHistory_.at(i).toMap();
        if (m.value(QStringLiteral("q")).toString() == q
            && m.value(QStringLiteral("server")).toString() == serverId) {
            lawHistory_.removeAt(i);
            break;
        }
    }
    QVariantMap e;
    e.insert(QStringLiteral("q"), q);
    e.insert(QStringLiteral("server"), serverId);
    lawHistory_.prepend(e);
    while (lawHistory_.size() > 20)   // последние ~20 запросов
        lawHistory_.removeLast();
    saveLawStore();
}

bool HubWindow::isLawFavorite(int id) const
{
    for (const QVariant &v : lawFavorites_)
        if (v.toMap().value(QStringLiteral("id")).toInt() == id)
            return true;
    return false;
}

// Карточка по id — для добавления в избранное (код/название). Ищем во всех живых данных.
QVariantMap HubWindow::lawFindCard(int id) const
{
    auto scan = [id](const QVariantList &list) -> QVariantMap {
        for (const QVariant &v : list) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("id")).toInt() == id)
                return m;
        }
        return {};
    };
    QVariantMap m = scan(lawResults_);
    if (m.isEmpty()) m = scan(lawRelated_);
    if (m.isEmpty() && lawArticle_.value(QStringLiteral("id")).toInt() == id)
        m = lawArticle_;
    if (m.isEmpty()) m = scan(lawArticle_.value(QStringLiteral("related")).toList());
    if (m.isEmpty()) m = scan(lawFavorites_);
    return m;
}

void HubWindow::toggleLawFavorite(int id)
{
    ensureLawStoreLoaded();
    for (int i = 0; i < lawFavorites_.size(); ++i) {
        if (lawFavorites_.at(i).toMap().value(QStringLiteral("id")).toInt() == id) {
            lawFavorites_.removeAt(i);   // уже в избранном → снимаем звёздочку
            saveLawStore();
            return;
        }
    }
    const QVariantMap card = lawFindCard(id);
    if (card.isEmpty())
        return;   // нечего сохранять (карточка ушла из выдачи) — молча
    QVariantMap fav;
    fav.insert(QStringLiteral("id"), id);
    fav.insert(QStringLiteral("code"), card.value(QStringLiteral("code")).toString());
    QString title = card.value(QStringLiteral("title")).toString();
    if (title.isEmpty()) title = card.value(QStringLiteral("article_ref")).toString();
    if (title.isEmpty()) title = card.value(QStringLiteral("doc_title")).toString();
    fav.insert(QStringLiteral("title"), title);
    fav.insert(QStringLiteral("server"),
               card.value(QStringLiteral("server_id")).toString().isEmpty()
                   ? currentServerId()
                   : card.value(QStringLiteral("server_id")).toString());
    lawFavorites_.prepend(fav);
    saveLawStore();
}

void HubWindow::ensureLawServersLoaded()
{
    if (lawServersAsked_)
        return;
    if (!auth_ || !auth_->isAuthorized())
        return;   // без Bearer смысла нет — спросим при следующем заходе в раздел
    lawServersAsked_ = true;
    laws_->requestServers();
}

void HubWindow::onLawServersReady(const QVariantList &projects)
{
    lawServersLoaded_ = true;
    lawSearchFlags_.clear();
    for (const QVariant &p : projects) {
        const QVariantList servers = p.toMap().value(QStringLiteral("servers")).toList();
        for (const QVariant &sv : servers) {
            const QVariantMap sm = sv.toMap();
            lawSearchFlags_.insert(sm.value(QStringLiteral("server_id")).toString(),
                                   sm.value(QStringLiteral("search")).toBool());
        }
    }
    updateLawAvailability();
}

void HubWindow::updateLawAvailability()
{
    if (!searchEdit_ || !searchBtn_ || !lawGateLbl_)
        return;
    // До получения флагов — оптимистично разрешаем (закрытая бета сама ответит 403,
    // onLawSearchError закроет гейт); после — строго по флагу search выбранного сервера.
    bool avail = true;
    if (lawServersLoaded_)
        avail = lawSearchFlags_.value(currentServerId(), false);
    searchEdit_->setEnabled(avail);
    searchBtn_->setEnabled(avail);
    lawGateLbl_->setVisible(!avail);
}

void HubWindow::onLawSubmit()
{
    if (searchEdit_)
        runLawQuery(searchEdit_->text());
}

void HubWindow::runLawQuery(const QString &q)
{
    const QString query = q.trimmed();
    if (query.isEmpty() || !lawView_)
        return;
    const HubColors &c = hubColors();
    if (auth_ && !auth_->isAuthorized()) {
        lawView_->setHtml(QStringLiteral("<div style='color:%1'>Сначала активируйте ключ.</div>")
                              .arg(QLatin1String(c.muted)));
        return;
    }
    ensureLawStoreLoaded();
    if (searchEdit_ && searchEdit_->text().trimmed() != query)
        searchEdit_->setText(query);   // повтор из истории — показываем запрос в поле
    // Летящая статья (клик art: → «Загружаю…») не должна пережить новый поиск: её поздний
    // ответ отрендерился бы ПОВЕРХ свежей выдачи (у поиска guard по lawQuery_, у статьи нет).
    laws_->cancelArticle();
    lawArticle_.clear();
    lawCopiedMark_ = false;
    lawQuery_ = query;
    lawView_->setHtml(QStringLiteral("<div style='color:%1'>🔎 Ищу «%2»…</div>")
                          .arg(QLatin1String(c.muted), esc(query)));
    laws_->requestSearch(query, currentServerId(), /*limit*/8);
}

void HubWindow::onLawSearchReady(const QString &query, const QVariantList &results,
                                 const QVariantList &related, bool degraded)
{
    // Бэкстоп: LawsClient отменяет предыдущий reply, но выдача устаревшего запроса
    // (успевшая встать в очередь) не должна перетереть актуальную.
    if (query != lawQuery_)
        return;
    lawResults_  = results;
    lawRelated_  = related;
    lawDegraded_ = degraded;
    lawArticle_.clear();
    addLawHistory(query, currentServerId());
    lawRenderResults();
}

void HubWindow::onLawSearchError(const QString &code, const QString &message)
{
    if (!lawView_)
        return;
    // Закрытая бета: сервер знает точнее флага — фиксируем гейт для этого сервера.
    if (code == QLatin1String("beta_closed")) {
        lawServersLoaded_ = true;
        lawSearchFlags_.insert(currentServerId(), false);
        updateLawAvailability();
    }
    const HubColors &c = hubColors();
    lawView_->setHtml(QStringLiteral(
        "<div style='color:%1'>⚠ %2</div>"
        "<div style='margin-top:8px'><a href='home:' style='color:%3;text-decoration:none'>"
        "← Недавние запросы</a></div>")
            .arg(QLatin1String(c.muted), esc(message), QLatin1String(c.accent)));
}

void HubWindow::onLawArticleReady(const QVariantMap &article)
{
    lawArticle_ = article;
    lawCopiedMark_ = false;
    if (favArticleMode_ && lawFavView_)
        lawFavView_->setHtml(buildLawArticleHtml());   // открыта из «Избранного» — рендер в той вкладке
    else
        lawRenderArticle();
}

void HubWindow::onLawArticleError(const QString &code, const QString &message)
{
    Q_UNUSED(code);
    QTextBrowser *view = (favArticleMode_ && lawFavView_) ? lawFavView_ : lawView_;
    if (!view)
        return;
    const HubColors &c = hubColors();
    view->setHtml(QStringLiteral(
        "<div style='color:%1'>⚠ Статья не загрузилась: %2</div>"
        "<div style='margin-top:8px'><a href='back:' style='color:%3;text-decoration:none'>"
        "← Назад</a></div>")
            .arg(QLatin1String(c.muted), esc(message), QLatin1String(c.accent)));
}

// Клики по якорям выдачи: q:<индекс истории> / art:<id> / fav:<id> / back: / home: / copy:.
void HubWindow::onLawAnchor(const QUrl &url)
{
    const QString s = url.toString();
    const HubColors &c = hubColors();
    if (s.startsWith(QLatin1String("q:"))) {
        const int idx = s.mid(2).toInt();
        if (idx >= 0 && idx < lawHistory_.size())
            runLawQuery(lawHistory_.at(idx).toMap().value(QStringLiteral("q")).toString());
    } else if (s.startsWith(QLatin1String("art:"))) {
        const int id = s.mid(4).toInt();
        favArticleMode_ = false;   // статья открыта из «Поиска» — рендер в lawView_
        if (lawView_)
            lawView_->setHtml(QStringLiteral("<div style='color:%1'>Загружаю статью…</div>")
                                  .arg(QLatin1String(c.muted)));
        laws_->requestArticle(id);
    } else if (s.startsWith(QLatin1String("fav:"))) {
        toggleLawFavorite(s.mid(4).toInt());
        // Перерисовать текущий вид — звёздочка меняет состояние на месте.
        if (!lawArticle_.isEmpty())      lawRenderArticle();
        else if (!lawQuery_.isEmpty())   lawRenderResults();
        else                             lawRenderHome();
    } else if (s == QLatin1String("back:")) {
        laws_->cancelArticle();   // ушли со «Загружаю статью…» — её поздний ответ не нужен
        lawArticle_.clear();
        if (!lawQuery_.isEmpty()) lawRenderResults();
        else                      lawRenderHome();
    } else if (s == QLatin1String("home:")) {
        laws_->cancelArticle();   // симметрично back: — устаревшая статья не перетрёт home
        lawArticle_.clear();
        lawResults_.clear();
        lawRelated_.clear();
        lawQuery_.clear();
        lawRenderHome();
    } else if (s == QLatin1String("copy:")) {
        // Копируем то, что нужно в рапорт: сырой заголовок статьи + полный текст.
        const QString ref  = lawArticle_.value(QStringLiteral("article_ref")).toString();
        const QString text = lawArticle_.value(QStringLiteral("full_text")).toString();
        QString clip = ref;
        if (!text.isEmpty())
            clip += (clip.isEmpty() ? QString() : QStringLiteral("\n\n")) + text;
        if (!clip.isEmpty()) {
            QApplication::clipboard()->setText(clip);
            lawCopiedMark_ = true;
            lawRenderArticle();   // ссылка сменится на «✓ Скопировано»
        }
    }
}

// ---------------------------------------------------------------------------
//  Поиск по законке: HTML-рендер (стартовый вид / выдача / статья).
// ---------------------------------------------------------------------------
void HubWindow::lawRenderHome()
{
    if (!lawView_)
        return;
    ensureLawStoreLoaded();
    const HubColors &c = hubColors();
    QString h = QStringLiteral(
        "<div style='color:%1'>Введите запрос: номер статьи, ключевые слова или ситуацию — "
        "найду подходящие статьи в законке выбранного сервера.</div>")
                    .arg(QLatin1String(c.muted));

    if (!lawHistory_.isEmpty()) {
        h += QStringLiteral("<div style='color:%1;font-size:11px;font-weight:800;"
                            "letter-spacing:1px;margin-top:14px'>НЕДАВНИЕ ЗАПРОСЫ</div>")
                 .arg(QLatin1String(c.dim));
        for (int i = 0; i < lawHistory_.size(); ++i) {
            const QVariantMap m = lawHistory_.at(i).toMap();
            h += QStringLiteral(
                "<div style='margin-top:4px'><a href='q:%1' style='color:%2;"
                "text-decoration:none'>%3</a> <span style='color:%4;font-size:11px'>· %5</span></div>")
                     .arg(QString::number(i), QLatin1String(c.accent),
                          esc(m.value(QStringLiteral("q")).toString()),
                          QLatin1String(c.dim),
                          esc(m.value(QStringLiteral("server")).toString()));
        }
    }

    // Избранное — в ОТДЕЛЬНОЙ вкладке «⭐ Избранное» (per-server + автообновление), не здесь.
    if (lawHistory_.isEmpty())
        h += QStringLiteral("<div style='color:%1;margin-top:12px'>Здесь появятся недавние "
                            "запросы. Избранные статьи — во вкладке «⭐ Избранное».</div>")
                 .arg(QLatin1String(c.dim));
    lawView_->setHtml(h);
    update();   // слоёное окно: применить домашний экран поиска без ожидания move
}

// ---------------------------------------------------------------------------
//  Избранное: рендер (ТОЛЬКО текущий сервер), свои клики, автообновление снимков.
// ---------------------------------------------------------------------------
void HubWindow::lawRenderFavorites()
{
    if (!lawFavView_)
        return;
    ensureLawStoreLoaded();
    const HubColors &c = hubColors();
    const QString sid = currentServerId();

    QString h = QStringLiteral(
        "<div style='color:%1'>Закреплённые статьи сервера "
        "<span style='color:%2'>%3</span> — обновляются автоматически при открытии вкладки.%4</div>")
            .arg(QLatin1String(c.muted), QLatin1String(c.dim), esc(sid),
                 favRefreshing_
                     ? QStringLiteral(" <span style='color:%1'>· обновляю…</span>").arg(QLatin1String(c.dim))
                     : QString());

    int shown = 0;
    for (const QVariant &v : lawFavorites_) {
        const QVariantMap m = v.toMap();
        const QString fs = m.value(QStringLiteral("server")).toString();
        if (!fs.isEmpty() && fs != sid)
            continue;   // не выбранный сервер — прячем (без «каши» одинаковых кодов с разных серверов)
        ++shown;
        const int id = m.value(QStringLiteral("id")).toInt();
        const QString code = m.value(QStringLiteral("code")).toString();
        QString caption = m.value(QStringLiteral("title")).toString();
        if (!code.isEmpty())
            caption = QStringLiteral("[%1] %2").arg(code, caption);
        h += QStringLiteral(
            "<div style='margin-top:8px'>"
            "<a href='fav:%1' style='color:%2;text-decoration:none'>★</a> "
            "<a href='art:%1' style='color:%3;text-decoration:none'>%4</a></div>")
                 .arg(QString::number(id), QLatin1String(c.accent), QLatin1String(c.text), esc(caption));
    }
    if (shown == 0)
        h += QStringLiteral("<div style='color:%1;margin-top:14px'>Пусто. Помечайте статьи "
                            "звёздочкой ★ в разделе «Поиск» — они появятся здесь.</div>")
                 .arg(QLatin1String(c.dim));

    lawFavView_->setHtml(h);
    update();
}

// Клики во вкладке «Избранное»: fav:<id> снять звезду; art:<id> открыть статью в разделе «Поиск».
// Клики во вкладке «Избранное» — всё В ЭТОЙ ЖЕ вкладке (не прыгаем в «Поиск»):
//   art:<id> открыть статью здесь · back: назад к списку · fav:<id> звезда · copy: копировать.
void HubWindow::onLawFavAnchor(const QUrl &url)
{
    const QString s = url.toString();
    const HubColors &c = hubColors();
    if (s.startsWith(QLatin1String("art:"))) {
        const int id = s.mid(4).toInt();
        favArticleMode_ = true;   // статью рендерим в lawFavView_ (см. onLawArticleReady)
        if (lawFavView_)
            lawFavView_->setHtml(QStringLiteral("<div style='color:%1'>Загружаю статью…</div>")
                                     .arg(QLatin1String(c.muted)));
        if (laws_) laws_->requestArticle(id);
    } else if (s == QLatin1String("back:")) {
        if (laws_) laws_->cancelArticle();   // ушли со статьи — поздний ответ не нужен
        favArticleMode_ = false;
        lawArticle_.clear();
        lawRenderFavorites();                // назад — к списку избранного (в этой же вкладке)
    } else if (s.startsWith(QLatin1String("fav:"))) {
        toggleLawFavorite(s.mid(4).toInt());
        if (favArticleMode_ && !lawArticle_.isEmpty() && lawFavView_)
            lawFavView_->setHtml(buildLawArticleHtml());   // открыта статья — перерисовать (звезда сменилась)
        else
            lawRenderFavorites();                          // список — строка ушла
    } else if (s == QLatin1String("copy:")) {
        const QString ref  = lawArticle_.value(QStringLiteral("article_ref")).toString();
        const QString text = lawArticle_.value(QStringLiteral("full_text")).toString();
        QString clip = ref;
        if (!text.isEmpty())
            clip += (clip.isEmpty() ? QString() : QStringLiteral("\n\n")) + text;
        if (!clip.isEmpty()) {
            QApplication::clipboard()->setText(clip);
            lawCopiedMark_ = true;
            if (lawFavView_) lawFavView_->setHtml(buildLawArticleHtml());   // ссылка → «✓ Скопировано»
        }
    }
}

// ---------------------------------------------------------------------------
//  Автообновление избранного: докачиваем снимки закреплённых статей ТЕКУЩЕГО сервера по одной
//  (свой reply в LawsClient — не мешает интерактивной статье). Обновилась → снимок; удалили → снять.
// ---------------------------------------------------------------------------
void HubWindow::startFavoritesRefresh()
{
    ensureLawStoreLoaded();
    if (favRefreshing_)
        return;   // уже идёт (повторный заход на вкладку)
    if (!auth_ || !auth_->isAuthorized())
        return;   // без Bearer докачать нельзя — показываем локальные снимки как есть
    const QString sid = currentServerId();
    favRefreshQueue_.clear();
    for (const QVariant &v : lawFavorites_) {
        const QVariantMap m = v.toMap();
        const QString fs = m.value(QStringLiteral("server")).toString();
        if (!fs.isEmpty() && fs != sid)
            continue;
        favRefreshQueue_.append(m.value(QStringLiteral("id")).toInt());
    }
    if (favRefreshQueue_.isEmpty())
        return;
    favRefreshing_ = true;
    favoritesRefreshNext();
}

void HubWindow::favoritesRefreshNext()
{
    if (favRefreshQueue_.isEmpty()) {
        favRefreshing_ = false;
        saveLawStore();          // снимки обновлены / удалённые сняты → сохранить
        lawRenderFavorites();    // финальная перерисовка (снять «обновляю…»)
        return;
    }
    if (laws_)
        laws_->refreshFavorite(favRefreshQueue_.takeFirst());
    else {
        favRefreshQueue_.clear();
        favRefreshing_ = false;
    }
}

void HubWindow::onLawFavoriteRefreshed(int id, const QVariantMap &article)
{
    for (int i = 0; i < lawFavorites_.size(); ++i) {
        QVariantMap m = lawFavorites_.at(i).toMap();
        if (m.value(QStringLiteral("id")).toInt() != id)
            continue;
        m.insert(QStringLiteral("code"), article.value(QStringLiteral("code")).toString());
        QString title = article.value(QStringLiteral("title")).toString();
        if (title.isEmpty()) title = article.value(QStringLiteral("article_ref")).toString();
        if (title.isEmpty()) title = article.value(QStringLiteral("doc_title")).toString();
        m.insert(QStringLiteral("title"), title);
        if (m.value(QStringLiteral("server")).toString().isEmpty())
            m.insert(QStringLiteral("server"), article.value(QStringLiteral("server_id")).toString());
        lawFavorites_[i] = m;   // обновлённый снимок
        break;
    }
    favoritesRefreshNext();
}

void HubWindow::onLawFavoriteMissing(int id)
{
    for (int i = 0; i < lawFavorites_.size(); ++i) {
        if (lawFavorites_.at(i).toMap().value(QStringLiteral("id")).toInt() == id) {
            lawFavorites_.removeAt(i);   // статья удалена на сервере → снять из избранного
            break;
        }
    }
    favoritesRefreshNext();
}

void HubWindow::onLawFavoriteRefreshFailed(int id)
{
    Q_UNUSED(id);
    favoritesRefreshNext();   // транзиентный сбой — снимок оставляем, двигаем очередь
}

void HubWindow::lawRenderResults()
{
    if (!lawView_)
        return;
    const HubColors &c = hubColors();
    QString h = QStringLiteral(
        "<div><a href='home:' style='color:%1;text-decoration:none;font-size:11px'>"
        "← Недавние запросы</a></div>").arg(QLatin1String(c.accent));

    h += QStringLiteral("<div style='margin-top:6px;color:%1'>По запросу "
                        "<span style='color:%2;font-weight:700'>«%3»</span>")
             .arg(QLatin1String(c.muted), QLatin1String(c.text), esc(lawQuery_));
    if (lawDegraded_)
        // Векторное плечо недоступно — маленький бейдж, НЕ ошибка (результаты валидны).
        h += QStringLiteral(" <span style='color:%1;font-size:11px'>· упрощённый поиск</span>")
                 .arg(QLatin1String(c.dim));
    h += QStringLiteral("</div>");

    if (lawResults_.isEmpty()) {
        h += QStringLiteral("<div style='color:%1;margin-top:10px'>Ничего не найдено — "
                            "попробуйте другие слова (номер статьи, синонимы).</div>")
                 .arg(QLatin1String(c.muted));
    } else {
        for (const QVariant &v : lawResults_) {
            const QVariantMap m = v.toMap();
            const int id = m.value(QStringLiteral("id")).toInt();
            const QString star = isLawFavorite(id) ? QStringLiteral("★") : QStringLiteral("☆");
            const QString penalty = m.value(QStringLiteral("penalty_summary")).toString();
            const QString doc     = m.value(QStringLiteral("doc_title")).toString();
            const QString snippet = m.value(QStringLiteral("snippet")).toString();
            h += QStringLiteral("<div style='margin-top:12px'>"
                                "<a href='fav:%1' style='color:%2;text-decoration:none'>%3</a> "
                                "<a href='art:%1' style='color:%2;text-decoration:none;"
                                "font-weight:700'>%4</a>")
                     .arg(QString::number(id), QLatin1String(c.accent), star,
                          esc(cardCaption(m)));
            if (!penalty.isEmpty())
                h += QStringLiteral(" <span style='color:%1'>— %2</span>")
                         .arg(QLatin1String(c.accent), esc(penalty));
            if (!doc.isEmpty())
                h += QStringLiteral("<br><span style='color:%1;font-size:11px'>%2</span>")
                         .arg(QLatin1String(c.dim), esc(doc));
            if (!snippet.isEmpty())
                h += QStringLiteral("<br><span style='color:%1'>%2</span>")
                         .arg(QLatin1String(c.muted), esc(snippet));
            h += QStringLiteral("</div>");
        }
    }

    if (!lawRelated_.isEmpty()) {
        h += QStringLiteral("<div style='color:%1;font-size:11px;font-weight:800;"
                            "letter-spacing:1px;margin-top:16px'>ЕЩЁ РЕЛЕВАНТНОЕ</div>")
                 .arg(QLatin1String(c.dim));
        for (const QVariant &v : lawRelated_) {
            const QVariantMap m = v.toMap();
            h += QStringLiteral("<div style='margin-top:4px'><a href='art:%1' style='color:%2;"
                                "text-decoration:none'>%3</a></div>")
                     .arg(QString::number(m.value(QStringLiteral("id")).toInt()),
                          QLatin1String(c.accent), esc(cardCaption(m)));
        }
    }
    lawView_->setHtml(h);
    update();   // слоёное окно: применить выдачу поиска без ожидания move
}

void HubWindow::lawRenderArticle()
{
    if (!lawView_ || lawArticle_.isEmpty())
        return;
    lawView_->setHtml(buildLawArticleHtml());
    update();   // слоёное окно: применить карточку статьи без ожидания move
}

// Единый HTML-билдер карточки статьи (из lawArticle_/lawCopiedMark_) — общий для вкладок «Поиск» и
// «Избранное». Якоря back:/fav:/copy:/art: обрабатывает тот обработчик, чей QTextBrowser показал
// статью: onLawAnchor («Поиск» → назад к выдаче) либо onLawFavAnchor («Избранное» → назад к списку).
QString HubWindow::buildLawArticleHtml()
{
    const HubColors &c = hubColors();
    const QVariantMap &m = lawArticle_;
    const int id = m.value(QStringLiteral("id")).toInt();
    const QString star = isLawFavorite(id) ? QStringLiteral("★") : QStringLiteral("☆");

    QString h = QStringLiteral(
        "<div><a href='back:' style='color:%1;text-decoration:none;font-size:11px'>← Назад</a></div>")
                    .arg(QLatin1String(c.accent));
    h += QStringLiteral("<div style='margin-top:8px'>"
                        "<a href='fav:%1' style='color:%2;text-decoration:none'>%3</a> "
                        "<span style='color:%4;font-weight:700;font-size:15px'>%5</span></div>")
             .arg(QString::number(id), QLatin1String(c.accent), star, QLatin1String(c.text),
                  esc(cardCaption(m)));

    const QString doc = m.value(QStringLiteral("doc_title")).toString();
    if (!doc.isEmpty())
        h += QStringLiteral("<div style='color:%1;font-size:11px'>%2</div>")
                 .arg(QLatin1String(c.dim), esc(doc));

    const QString penalty = m.value(QStringLiteral("penalty_summary")).toString();
    if (!penalty.isEmpty())
        h += QStringLiteral("<div style='margin-top:6px;color:%1;font-weight:700'>"
                            "Наказание: %2</div>").arg(QLatin1String(c.accent), esc(penalty));

    // Детализация санкции (penalty_json), если сервер её разобрал: строки «Штраф: …» и т.п.
    const QVariantMap pj = m.value(QStringLiteral("penalty_json")).toMap();
    if (!pj.isEmpty()) {
        static const struct { const char *key; const char *label; } kPen[] = {
            {"fine",         "Штраф"},
            {"prison",       "Тюрьма"},
            {"arrest",       "Арест"},
            {"wanted_level", "Розыск"},
            {"other",        "Прочее"},
        };
        for (const auto &p : kPen) {
            const QString val = pj.value(QLatin1String(p.key)).toString();
            if (!val.isEmpty())
                // p.label — узкий const char* с UTF-8 кириллицей; QLatin1String трактует байты
                // как Latin-1 → мойибейк («Ð Ð¾Ð·ÑÑÐº»). Декодируем как UTF-8.
                h += QStringLiteral("<div style='color:%1;font-size:12px'>%2: %3</div>")
                         .arg(QLatin1String(c.muted), QString::fromUtf8(p.label), esc(val));
        }
    }

    // Копирование в рапорт: сырой article_ref + полный текст (см. onLawAnchor "copy:").
    h += QStringLiteral("<div style='margin-top:8px'><a href='copy:' style='color:%1;"
                        "text-decoration:none'>%2</a></div>")
             .arg(QLatin1String(c.accent),
                  lawCopiedMark_ ? QStringLiteral("✓ Скопировано")
                                 : QStringLiteral("⧉ Копировать статью"));

    const QString fullText = m.value(QStringLiteral("full_text")).toString();
    if (!fullText.isEmpty())
        h += QStringLiteral("<div style='margin-top:8px;color:%1'>%2</div>")
                 .arg(QLatin1String(c.text),
                      esc(fullText).replace(QStringLiteral("\n"), QStringLiteral("<br>")));

    const QVariantList related = m.value(QStringLiteral("related")).toList();
    if (!related.isEmpty()) {
        h += QStringLiteral("<div style='color:%1;font-size:11px;font-weight:800;"
                            "letter-spacing:1px;margin-top:16px'>ЕЩЁ РЕЛЕВАНТНОЕ</div>")
                 .arg(QLatin1String(c.dim));
        for (const QVariant &v : related) {
            const QVariantMap rm = v.toMap();
            h += QStringLiteral("<div style='margin-top:4px'><a href='art:%1' style='color:%2;"
                                "text-decoration:none'>%3</a></div>")
                     .arg(QString::number(rm.value(QStringLiteral("id")).toInt()),
                          QLatin1String(c.accent), esc(cardCaption(rm)));
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
//  Трей: единственная иконка приложения. Меню обслуживает и хаб, и бар (бар получает
//  этот tray как sharedTray — только для balloon-уведомлений).
// ---------------------------------------------------------------------------
void HubWindow::setupTray()
{
    if (tray_ || !QSystemTrayIcon::isSystemTrayAvailable())
        return;

    QIcon ic = qApp->windowIcon();
    if (ic.isNull())
        ic = QIcon(QStringLiteral(":/assets/app.ico"));

    tray_ = new QSystemTrayIcon(ic, this);
    tray_->setToolTip(QStringLiteral("RP-ассистент"));

    auto *menu = new QMenu(this);
    menu->addAction(QStringLiteral("Открыть хаб"), this, &HubWindow::showFromTray);
    menu->addAction(QStringLiteral("Показать оверлей"), this,
                    [this] { emit launchOverlayRequested(); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("Настройки…"), this, [this] { emit settingsRequested(); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("Выход"), qApp, &QApplication::quit);
    tray_->setContextMenu(menu);

    // ЛКМ/двойной клик по иконке — открыть хаб (главное окно приложения).
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) {
                if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
                    showFromTray();
            });
    applyStealthTray();   // показать иконку, если режим невидимка выключен; иначе не показывать
}

// «Режим невидимка»: прячем иконку трея — приложение остаётся только процессом в Диспетчере
// задач. Вернуть хаб можно повторным запуском ярлыка (single-instance IPC → showFromTray),
// оттуда же — настройки/выключение режима. Полный выход при скрытом трее — через Диспетчер задач.
void HubWindow::applyStealthTray()
{
    if (!tray_)
        return;
    if (stealth::masterOn())
        tray_->hide();
    else
        tray_->show();
}

void HubWindow::showFromTray()
{
    if (isMinimized())
        showNormal();
    show();
    raise();
    activateWindow();
    update();   // форс флаша: восстановление frameless+translucent окна из трея/таскбара без repaint = пустой слой
}

// ---------------------------------------------------------------------------
//  Статус-панель + кнопка запуска.
// ---------------------------------------------------------------------------
void HubWindow::refreshStatus()
{
    if (verVal_)
        verVal_->setText(QStringLiteral("v%1").arg(qApp->applicationVersion()));

    if (tierVal_) {
        if (auth_ && auth_->isAuthorized()) {
            const QString t = auth_->tier();
            // Тир известен только из ответа активации/refresh — новых сетевых запросов не делаем.
            tierVal_->setText(t.isEmpty() ? QStringLiteral("активна") : humanTier(t));
        } else {
            tierVal_->setText(QStringLiteral("не авторизован"));
        }
    }

    const QString serverId = currentServerId();
    if (serverVal_)
        serverVal_->setText(serverId);

    const bool relay = (activeChannel() == QLatin1String("relay"));
    if (channelVal_)
        channelVal_->setText(relay ? QStringLiteral("резервный (РФ)") : QStringLiteral("основной"));

    // Состояние бара: не создан → «Запустить», создан-но-скрыт → «Показать», видим → «Скрыть».
    QWidget *bar = overlayProvider_ ? overlayProvider_() : nullptr;
    if (overlayVal_ && launchBtn_) {
        if (!bar) {
            overlayVal_->setText(QStringLiteral("не запущен"));
            launchBtn_->setText(QStringLiteral("▶  Запустить оверлей"));
        } else if (bar->isVisible()) {
            overlayVal_->setText(QStringLiteral("работает"));
            launchBtn_->setText(QStringLiteral("⏸  Скрыть оверлей"));
        } else {
            overlayVal_->setText(QStringLiteral("скрыт"));
            launchBtn_->setText(QStringLiteral("▶  Показать оверлей"));
        }
    }
    if (launchHint_)
        launchHint_->setText(
            QStringLiteral("Бар появится поверх игры. Скрыть/показать его можно хоткеем %1 или отсюда.")
                .arg(settings::window().hkToggle));

    if (footPill_)
        footPill_->setText(QStringLiteral("🟢  Сервер: %1 · канал: %2")
                               .arg(serverId,
                                    relay ? QStringLiteral("резервный") : QStringLiteral("основной")));

    // Гейт беты поиска следит за сменой сервера (пикер в настройках) — дёшево, по тем же тикам.
    updateLawAvailability();
}

void HubWindow::onLaunchClicked()
{
    QWidget *bar = overlayProvider_ ? overlayProvider_() : nullptr;
    if (bar && bar->isVisible())
        emit hideOverlayRequested();
    else
        emit launchOverlayRequested();
    // Сигналы соединены directly (main в том же потоке) — состояние бара уже новое.
    refreshStatus();
}

// ---------------------------------------------------------------------------
//  Frameless: перетаскивание за шапку + рамка Win11 + жизненный цикл (трей/таймер).
// ---------------------------------------------------------------------------
void HubWindow::mousePressEvent(QMouseEvent *e)
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
    QWidget::mousePressEvent(e);
}

void HubWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (dragging_ && (e->buttons() & Qt::LeftButton)) {
        move(e->globalPosition().toPoint() - dragPos_);
        update();   // слоёное окно: очистить «хвост» прозрачной рамки при перетаскивании хаба
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void HubWindow::mouseReleaseEvent(QMouseEvent *e)
{
    dragging_ = false;
    QWidget::mouseReleaseEvent(e);
}

void HubWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
#ifdef Q_OS_WIN
    // Снять светлую системную рамку Win11 (как у оверлея/настроек) — иначе hairline вокруг карточки.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    COLORREF noBorder = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
#endif
    refreshStatus();
    if (statusTimer_)
        statusTimer_->start();
    update();   // форс первого флаша слоёного окна хаба (без него — пустой прозрачный прямоугольник)
}

void HubWindow::hideEvent(QHideEvent *e)
{
    if (statusTimer_)
        statusTimer_->stop();
    QWidget::hideEvent(e);
}

void HubWindow::closeEvent(QCloseEvent *e)
{
    // ✕ = свернуть в трей, приложение живёт (setQuitOnLastWindowClosed(false) в main) —
    // то же поведение, что у бара. Полный выход — «Выход» в меню трея.
    e->ignore();
    hide();
    if (tray_ && !shownTrayHint_) {
        tray_->showMessage(QStringLiteral("RP-ассистент"),
                           QStringLiteral("Хаб свёрнут в трей. Клик по иконке — открыть снова."),
                           QSystemTrayIcon::Information, 2500);
        shownTrayHint_ = true;
    }
}
