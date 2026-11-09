#pragma once

#include <QWidget>
#include <QPoint>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QHash>

#include <functional>

class AuthClient;
class LawsClient;

class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QStackedWidget;
class QSystemTrayIcon;
class QTextBrowser;
class QNetworkAccessManager;
class QTimer;
class QUrl;
class QVBoxLayout;
class QMouseEvent;
class QShowEvent;
class QHideEvent;
class QCloseEvent;

// HubWindow — окно-хаб (лаунчер), ГЛАВНОЕ десктопное окно приложения с 0.4.0.
//
// Открывается первым после авторизации; игровой бар (OverlayWindow) запускается отсюда
// кнопкой «Запустить оверлей» (или сразу при старте — тумблер settings::overlayAutostart,
// старое поведение ≤0.3.x; хаб при этом тоже создаётся). Разделы (сайдбар + QStackedWidget):
//   • Главная — статус-панель (версия / лицензия / сервер / канал / состояние бара)
//               + крупная кнопка запуска/скрытия бара;
//   • Поиск по законке — БЕТА: поиск по каталогу законки сервера (LawsClient →
//     /v1/overlay/laws/*), карточки → полная статья, история + избранное (локально,
//     law_search.json рядом с settings.json); доступность гейтится флагом search
//     у выбранного сервера (/v1/overlay/servers);
//   • Памятки — заглушка «в разработке»;
//   • Новости — {apiBaseUrl}/updates/news.json (кэш на сеанс; нет файла → «новостей пока нет»);
//   • Настройки — кнопка, открывающая СУЩЕСТВУЮЩИЙ SettingsDialog (через main → OverlayWindow).
//
// Хаб владеет ЕДИНСТВЕННОЙ иконкой трея приложения (main передаёт её в OverlayWindow как
// sharedTray — бару она нужна только для balloon-уведомлений). ✕ хаба = свернуть в трей
// (приложение живёт: setQuitOnLastWindowClosed(false) в main); полный выход — из меню трея.
//
// Вёрстка/стиль — как у SettingsDialog (плавающая frameless-карточка активной темы):
// переиспользуем theme::dialogStyle() через ТЕ ЖЕ objectName (#dlgContent/#dlgHead/#dlgNav/…),
// хаб-специфика (карточка статуса, крупная кнопка, поле поиска) — небольшой доп-QSS.
class HubWindow : public QWidget {
    Q_OBJECT
public:
    explicit HubWindow(AuthClient *auth, QWidget *parent = nullptr);

    // Единственная иконка трея приложения (живёт у хаба; nullptr, если системный трей недоступен).
    QSystemTrayIcon *tray() const { return tray_; }

    // main отдаёт доступ к бару (создаётся лениво в main): хаб по нему показывает состояние
    // «не запущен / работает / скрыт» и выбирает надпись на кнопке запуска.
    void setOverlayProvider(std::function<QWidget *()> provider);

    void showFromTray();    // показать/поднять (клик по трею, IPC повторного запуска exe)

signals:
    void launchOverlayRequested();  // «Запустить/Показать оверлей» (кнопка хаба / меню трея)
    void hideOverlayRequested();    // «Скрыть оверлей» (бар видим → спрятать)
    void settingsRequested();       // открыть СУЩЕСТВУЮЩЕЕ окно настроек (SettingsDialog)

public slots:
    void refreshStatus();           // перечитать статус-панель (таймер + после действий)
    void applyStealthTray();        // «режим невидимка»: скрыть/показать иконку трея (живьём из StealthGuard)

protected:
    void mousePressEvent(QMouseEvent *e) override;   // перетаскивание frameless-окна за шапку
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void showEvent(QShowEvent *e) override;          // снять рамку Win11 + старт таймера статуса
    void hideEvent(QHideEvent *e) override;          // стоп таймера статуса
    void closeEvent(QCloseEvent *e) override;        // ✕ = свернуть в трей (выход — из меню трея)

private slots:
    void onLaunchClicked();         // кнопка запуска: запустить/показать ↔ скрыть бар

    // --- Поиск по законке (LawsClient) ---
    void onLawSubmit();             // Enter в поле / кнопка «Найти»
    void onLawAnchor(const QUrl &url);  // клики по ссылкам выдачи (art:/q:/fav:/back:/copy:)
    void onLawSearchReady(const QString &query, const QVariantList &results,
                          const QVariantList &related, bool degraded);
    void onLawSearchError(const QString &code, const QString &message);
    void onLawArticleReady(const QVariantMap &article);
    void onLawArticleError(const QString &code, const QString &message);
    void onLawServersReady(const QVariantList &projects);   // флаги search per server
    // Автообновление избранного (фоновая докачка снимков закреплённых статей текущего сервера):
    void onLawFavoriteRefreshed(int id, const QVariantMap &article);  // обновить снимок code/title
    void onLawFavoriteMissing(int id);                                // статья удалена → снять из избранного
    void onLawFavoriteRefreshFailed(int id);                          // транзиент → оставить как есть
    void onLawFavAnchor(const QUrl &url);   // клики во вкладке «Избранное» (fav: снять / art: открыть в «Поиске»)

private:
    void buildUi();
    void setupTray();               // единственная трей-иконка приложения + меню
    // Каркас одной страницы (контейнер + layout + crimson-заголовок секции) — как в SettingsDialog.
    QWidget *makePage(const QString &sectionTitle, QVBoxLayout *&outLayout);
    void buildHomePage();           // Главная: статус-панель + крупная кнопка запуска бара
    void buildLawSearchPage();      // Поиск по законке (бета): поле + выдача + история/избранное
    void buildFavoritesPage();      // Избранное: отдельная вкладка (per-server + автообновление)
    void buildNotesPage();          // Памятки: заглушка «в разработке»
    void buildNewsPage();           // Новости: /updates/news.json (кэш на сеанс)
    void buildSettingsPage();       // Настройки: переход в существующий SettingsDialog

    // --- Поиск по законке: данные/рендер ---
    QString currentServerId() const;        // как на статус-панели: settings → фолбэк cfg()
    void ensureLawStoreLoaded();            // история/избранное из law_search.json (лениво)
    void saveLawStore() const;              // ... и обратно на диск
    void addLawHistory(const QString &q, const QString &serverId);   // dedupe + cap 20
    bool isLawFavorite(int id) const;
    void toggleLawFavorite(int id);         // ищет карточку в выдаче/статье → add/remove + save
    void ensureLawServersLoaded();          // флаги search: один запрос /servers на сеанс
    void updateLawAvailability();           // enable/disable поля + плашка «недоступен» по флагу
    void runLawQuery(const QString &q);     // общий вход поиска (поле, история)
    void lawRenderHome();                   // история + избранное (стартовый вид)
    void lawRenderResults();                // карточки выдачи (+degraded, related)
    void lawRenderArticle();                // полная статья (+related, копирование) в разделе «Поиск»
    QString buildLawArticleHtml();          // HTML карточки статьи — общий для вкладок «Поиск» и «Избранное»
    void lawRenderFavorites();              // вкладка «Избранное» ТЕКУЩЕГО сервера (per-server фильтр)
    void startFavoritesRefresh();           // автообновление: собрать очередь id текущего сервера + старт
    void favoritesRefreshNext();            // ... докачать следующий id (по одному, свой reply в LawsClient)
    // Найти карточку по id среди выдачи/related/статьи/избранного (для добавления в избранное).
    QVariantMap lawFindCard(int id) const;

    // --- Новости ---
    void ensureNewsLoaded();                // GET /updates/news.json (кэш на сеанс)
    void renderNews();

    // Зависимости (не владеем auth_; laws_/nam_ — наши, parented).
    AuthClient *auth_ = nullptr;                     // статус авторизации / тир лицензии
    LawsClient *laws_ = nullptr;                     // поиск по законке (свой инстанс хаба)
    QNetworkAccessManager *nam_ = nullptr;           // новости (без Bearer)
    std::function<QWidget *()> overlayProvider_;     // доступ к бару (nullptr = ещё не создан)

    // --- Каркас (objectName совпадают с SettingsDialog → общий QSS активной темы) ---
    QWidget        *contentRoot_ = nullptr;  // #dlgContent — плавающая карточка
    QWidget        *header_      = nullptr;  // #dlgHead — шапка (drag-зона)
    QListWidget    *navList_     = nullptr;  // #dlgNav — сайдбар-разделы
    QStackedWidget *stack_       = nullptr;  // страницы разделов
    // Индексы динамических пунктов навигации (= индекс страницы в stack_): состав пунктов
    // зависит от варианта билда (Поиск/Избранное — только с RPOVERLAY_SEARCH; Памятки/Новости —
    // без RPOVERLAY_LAWS), поэтому ленивая инициализация сверяется с этими членами, а НЕ с
    // хардкодом row==N (иначе в чистом QA «Новости» съехали бы на место вырезанного «Поиска»).
    int navRowSearch_ = -1;   // «Поиск по законке» (-1 = пункта нет в этом билде)
    int navRowFav_    = -1;   // «Избранное»
    int navRowNews_   = -1;   // «Новости»
    QLabel         *footPill_    = nullptr;  // #dlgServerPill — «Сервер: X · канал: Y»

    // --- Главная: статус-панель ---
    QLabel      *verVal_     = nullptr;  // версия приложения
    QLabel      *tierVal_    = nullptr;  // тир лицензии / статус авторизации
    QLabel      *serverVal_  = nullptr;  // выбранный сервер законов
    QLabel      *channelVal_ = nullptr;  // активный канал связи (main/relay)
    QLabel      *overlayVal_ = nullptr;  // состояние бара: не запущен / работает / скрыт
    QPushButton *launchBtn_  = nullptr;  // #hubLaunch — крупная кнопка запуска/скрытия
    QLabel      *launchHint_ = nullptr;  // подсказка под кнопкой (хоткей показа/скрытия)

    // --- Поиск по законке: UI + состояние ---
    QLineEdit    *searchEdit_ = nullptr;  // строка запроса
    QPushButton  *searchBtn_  = nullptr;  // «Найти»
    QLabel       *lawGateLbl_ = nullptr;  // плашка «поиск недоступен на этом сервере» (бета-гейт)
    QTextBrowser *lawView_    = nullptr;  // #hubLawView — выдача/статья/история (HTML)
    QTextBrowser *lawFavView_ = nullptr;  // #hubLawView — вкладка «Избранное» (per-server список)
    QVariantList  lawResults_;            // последняя выдача (Card'ы)
    QVariantList  lawRelated_;            // related последней выдачи
    QVariantMap   lawArticle_;            // открытая статья (или пусто)
    QString       lawQuery_;              // запрос последней выдачи
    bool          lawDegraded_   = false; // выдача без векторного плеча → бейдж «упрощённый поиск»
    bool          lawCopiedMark_ = false; // «✓ скопировано» у ссылки копирования статьи
    QVariantList  lawHistory_;            // [{q, server}] — последние ~20, локально
    QVariantList  lawFavorites_;          // [{id, code, title, server}] — локально
    QList<int>    favRefreshQueue_;       // id для автообновления избранного (докачка по одному)
    bool          favRefreshing_ = false; // идёт автообновление (защита от повторного захода на вкладку)
    bool          favArticleMode_ = false; // во вкладке «Избранное» открыта СТАТЬЯ (не список)
    bool          lawStoreLoaded_   = false;  // law_search.json уже прочитан
    bool          lawServersAsked_  = false;  // /servers уже запрошен (защёлка на сеанс)
    bool          lawServersLoaded_ = false;  // флаги search получены
    QHash<QString, bool> lawSearchFlags_;     // server_id → search (гейт беты)

    // --- Новости ---
    QTextBrowser *newsView_   = nullptr;  // #hubLawView-стиль; рендер списка новостей
    bool          newsAsked_  = false;    // запрос news.json уже ушёл (кэш на сеанс)
    QVariantList  newsItems_;             // [{date,title,text}] (пусто = «новостей пока нет»)

    // --- Трей / сервисное ---
    QSystemTrayIcon *tray_ = nullptr;
    QTimer *statusTimer_   = nullptr;    // обновление статус-панели, пока хаб виден
    bool    shownTrayHint_ = false;      // одноразовый balloon «свёрнут в трей»
    bool    dragging_      = false;      // идёт перетаскивание за шапку
    QPoint  dragPos_;
};
