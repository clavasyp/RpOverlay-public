// main.cpp — точка входа C++/Qt оверлея для RP (релизный клиент).
// Создаёт модули (auth/audio/voice/advice) и открывает окно-ХАБ (лаунчер, с 0.4.0 —
// главное окно приложения); компактный игровой Q&A-бар (вопрос голосом по PTT или
// текстом → ответ по законам сервера) запускается из хаба кнопкой «Запустить оверлей»
// либо сразу при старте (тумблер overlayAutostart — старое поведение ≤0.3.x).
//
// Перед окном — тихая проверка авто-обновления (UpdateClient): если вышла новая версия,
// ставим её до показа панели; иначе (или при ошибке/оффлайне/таймауте) стартуем как есть.
// Плюс single-instance мьютекс (то же имя, что AppMutex в RpOverlay.iss).

#include <QApplication>
#include <QLabel>
#include <QGuiApplication>
#include <QScreen>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QSysInfo>

#include "Config.h"
#include "Settings.h"
#include "StealthGuard.h"
#include "LayerRepaint.h"
#include "HubWindow.h"
#include "OverlayWindow.h"
#include "AuthClient.h"
#include "LicenseClient.h"
#include "TelemetryClient.h"
#include "KillSwitch.h"
#include "ActivationWindow.h"
#include "AudioCapture.h"
#include "VoiceClient.h"
#include "AdviceClient.h"
#include "Updater.h"
#include "Hardening.h"
#include "ModelManager.h"
#include "Connectivity.h"
#include "relay_cert.h"
#include "Theme.h"
#include "DebugLog.h"
#include "CrashHandler.h"

#include <QSslConfiguration>
#include <QSslCertificate>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"   // реально задаётся в CMake из project(... VERSION ...)
#endif

namespace {
// Имя ДОЛЖНО совпадать с AppMutex в RpOverlay.iss — тогда установщик корректно
// детектит запущенное приложение (CloseApplications), а мы получаем single-instance.
// Без префикса Global\ — это Local-namespace (сессия пользователя): per-user приложению
// (PrivilegesRequired=lowest) НЕ дают создавать Global\-объекты, а для single-instance
// и детекта установщиком (тот же пользователь/сессия) сессионного мьютекса достаточно.
// laws-билд (отдельный продукт «Поиск по законке») получает СВОИ имена мьютекса/IPC/папки
// настроек, чтобы мирно сосуществовать с основным оверлеем на одной машине (иначе делили бы
// single-instance-мьютекс, %APPDATA%-настройки, tokens.dat и конфликтовали бы установщики).
#ifdef Q_OS_WIN
#ifdef RPOVERLAY_LAWS
const wchar_t *kSingleInstanceMutex = L"RpOverlayLawsSingleInstance";
#else
const wchar_t *kSingleInstanceMutex = L"RpOverlaySingleInstance";
#endif
#endif

// Минималистичный сплэш на время проверки/установки апдейта.
QLabel *makeSplash()
{
    auto *s = new QLabel(QStringLiteral("Проверка обновлений…"));
    s->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    s->setAttribute(Qt::WA_TranslucentBackground, false);
    s->setAlignment(Qt::AlignCenter);
    s->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(18,18,24,0.97); color: #e8e8ec;"
        "         font-family: 'Segoe UI', sans-serif; font-size: 13px;"
        "         border: 1px solid rgba(255,255,255,0.10); border-radius: 8px;"
        "         padding: 18px 26px; }"));
    s->setFixedWidth(280);
    s->adjustSize();
    if (QScreen *scr = QGuiApplication::primaryScreen())
        s->move(scr->geometry().center() - s->rect().center());
    return s;
}
} // namespace

int main(int argc, char **argv)
{
    // ПЕРВЫМ делом — обработчик крашей (всегда вкл, не зависит от режима отладки): при падении
    // пишет причину (сбойный модуль+смещение, код, стек) и минидамп в logs/. Чтобы диагностировать
    // полевые краши без «Журнала событий Windows».
    crashhandler::install(APP_VERSION);

    QApplication app(argc, argv);
    // laws-билд — отдельная папка настроек (%APPDATA%\ProdBot\RP Overlay Laws): свой tokens.dat/
    // settings.json/law_search.json, не пересекается с основным оверлеем на той же машине.
#ifdef RPOVERLAY_LAWS
    app.setApplicationName("RP Overlay Laws");
#else
    app.setApplicationName("RP Overlay");
#endif
    app.setOrganizationName("ProdBot");
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
#ifdef HAVE_APP_ICON
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/app.ico")));  // окно/таскбар
#endif

    // --- ПРОКСИ ВЫКЛ для всего приложения (фикс «device/start: Proxy connection refused») ---
    // По умолчанию Qt подхватывает СИСТЕМНЫЙ прокси Windows. У части юзеров прописан прокси/VPN,
    // который сейчас не слушает → ВСЕ запросы (auth/advice/update/voice) падают с ProxyConnectionRefused.
    // Наш обход блокировок — это резервный КАНАЛ (relay-реверс-прокси), а не клиентский прокси,
    // поэтому системный прокси оверлею не нужен никогда. Жёстко отключаем его глобально:
    // покрывает и QNetworkAccessManager, и QWebSocket (берут applicationProxy).
    QNetworkProxyFactory::setUseSystemConfiguration(false);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    // Окно живёт до явного выхода (крестик ✕ зовёт qApp->quit()); не закрываемся по
    // последнему окну — на стыке сплэш↔панель это иначе могло бы погасить приложение.
    app.setQuitOnLastWindowClosed(false);

    // «Режим невидимка» (тестер-билд): глобальный фильтр вешает WDA_EXCLUDEFROMCAPTURE на каждое
    // новое top-level окно (в т.ч. попапы), пока режим включён. Ставим ДО показа любых окон
    // (сплэш/активация/хаб). В QA-сборке masterOn()==false → фильтр по факту бездействует.
    stealth::install();

    // Страховка перерисовки слоёных окон: закрытая выпадашка/меню оставляла под собой
    // прозрачную дыру («фантомное окно с рамкой»), потому что система не шлёт таким окнам
    // WM_PAINT на освободившийся регион. Фильтр ставим тоже ДО показа любых окон.
    layerfix::install();

#ifdef Q_OS_WIN
    // Мьютекс оставляем для детекта запущенного приложения установщиком (CloseApplications в .iss).
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    const bool alreadyRunning = (hMutex && GetLastError() == ERROR_ALREADY_EXISTS);
#else
    const bool alreadyRunning = false;
#endif
    // Single-instance + АКТИВАЦИЯ: повторный клик по ярлыку/exe не плодит окно, а просит уже
    // запущенный экземпляр показаться (локальный сокет/именованный канал). Без этого второй
    // запуск раньше просто молча выходил (return 0) — «ничего не происходило».
#ifdef RPOVERLAY_LAWS
    const QString kIpcName = QStringLiteral("RpOverlayLawsSingleInstanceIpc");
#else
    const QString kIpcName = QStringLiteral("RpOverlaySingleInstanceIpc");
#endif
    if (alreadyRunning) {
        QLocalSocket sock;
        sock.connectToServer(kIpcName);
        if (sock.waitForConnected(400)) {
            sock.write("SHOW");
            sock.flush();
            sock.waitForBytesWritten(400);
        }
        return 0;   // первый экземпляр покажет себя сам
    }
    // Мы — первый экземпляр: слушаем запросы активации от будущих запусков.
    QLocalServer::removeServer(kIpcName);          // снять возможный stale-сокет от прошлого падения
    auto *ipcServer = new QLocalServer(&app);
    ipcServer->listen(kIpcName);
    // Указатели на создаваемые окна (появятся позже — после проверки апдейта/авторизации).
    // IPC-обработчик обращается к ним по этим слотам.
    OverlayWindow **winBox = new OverlayWindow *{nullptr};
    ActivationWindow **actBox = new ActivationWindow *{nullptr};
    HubWindow **hubBox = new HubWindow *{nullptr};
    QObject::connect(ipcServer, &QLocalServer::newConnection, &app, [ipcServer, winBox, actBox, hubBox]() {
        while (QLocalSocket *c = ipcServer->nextPendingConnection()) {
            c->disconnectFromServer();
            c->deleteLater();
        }
        // Поднять то окно, что сейчас актуально: окно активации (если оно видимо — нет авторизации)
        // приоритетнее; затем хаб (главное окно с 0.4.0); фолбэки — бар / окно активации.
        if (*actBox && (*actBox)->isVisible())
            (*actBox)->showCentered();
        else if (*hubBox)
            (*hubBox)->showFromTray();
        else if (*winBox)
            (*winBox)->showFromExternal();
        else if (*actBox)
            (*actBox)->showCentered();
    });

    (void)cfg();              // прелоад config.json
    (void)settings::audio();  // прелоад settings.json (mic/game device + autoUpdate + theme + connection)

    // Активная тема оформления — до построения окон, чтобы стартовый QSS был уже её.
    theme::setActive(settings::uiTheme());

    // Режим отладки (тестеры): если включён — открыть консоль + лог-файл ДО сетевых шагов,
    // чтобы в лог попали проба канала, авторизация, апдейт и т.д.
    debuglog::setEnabled(settings::debugMode());
    if (debuglog::isEnabled()) {
        qInfo().noquote() << "[main] старт, версия" << QStringLiteral(APP_VERSION);
        // Системный контекст — помогает диагностировать полевые краши (особенно графику Qt6Gui).
        qInfo().noquote() << "[env] ОС:" << QSysInfo::prettyProductName()
                          << "| kernel" << QSysInfo::kernelVersion()
                          << "| arch" << QSysInfo::currentCpuArchitecture();
        qInfo().noquote() << "[env] Qt runtime:" << qVersion() << "| сборка: Q&A-бар (тестерский канал)";
        if (auto *pscr = QGuiApplication::primaryScreen())
            qInfo().noquote() << "[env] экран:" << pscr->geometry().width() << "x" << pscr->geometry().height()
                              << "| DPR" << pscr->devicePixelRatio()
                              << "| экранов:" << QGuiApplication::screens().size();
    }

    // Сплэш показываем СРАЗУ: под ним идут синхронные проверки (канал связи, затем апдейт).
    QLabel *splash = makeSplash();
    splash->show();

    // Доверяем self-signed серту RU-сервера-прокладки (резервный канал): добавляем его
    // в дефолтный SSL-конфиг, чтобы TLS к relay (relay.invalid:8443) валидировался.
    // Покрывает и QNetworkAccessManager, и QWebSocket (оба берут default-конфиг). Серт имеет
    // SAN IP:relay.invalid → проверка имени проходит. Trust ограничен ровно этим сертом.
    {
        const QList<QSslCertificate> relay =
            QSslCertificate::fromData(QByteArray(RPOVERLAY_RELAY_CERT_PEM), QSsl::Pem);
        if (!relay.isEmpty()) {
            QSslConfiguration sc = QSslConfiguration::defaultConfiguration();
            QList<QSslCertificate> cas = sc.caCertificates();
            cas += relay;
            sc.setCaCertificates(cas);
            QSslConfiguration::setDefaultConfiguration(sc);
        }
    }

    // --- Выбор канала связи (ДО любых сетевых запросов и update-check) ---
    // Основной: напрямую на overlay.turgovbot.ru. Если недоступен (DPI режет Cloudflare) —
    // резервный: через RU-сервер-прокладку. Без этого заблокированный юзер не скачал бы
    // даже манифест обновления.
    splash->setText(QStringLiteral("Проверка соединения…"));
    splash->repaint();
    qApp->processEvents();
    setActiveChannel(conn::chooseChannel(settings::connMode()));
    qInfo().noquote() << "[main] активный канал:" << cfg().apiBaseUrl
                      << "(connMode=" << settings::connMode() << ", last=" << settings::connLast() << ")";

    auto *auth    = new AuthClient(&app);
    auto *license = new LicenseClient(&app);   // активация по ключу (challenge→подпись HWID→activate)
    // Дуал-сенд детектов харденинга на ОБА канала (main+relay) + досылка офлайн-очереди (Слой D).
    auto *telemetry = new TelemetryClient(&app);
    telemetry->flushQueued();                  // дошлём то, что не ушло в прошлый сеанс (оба канала лежали)
    // Удалённый кил-свитч (Слой F): обрабатывает pending-команду из ответа verify (сам верифицирует
    // подпись токена вшитым публичным ключом релиза + target_pubkey == свой, иначе ничего не делает).
    auto *killSwitch = new KillSwitch(&app);
    QObject::connect(license, &LicenseClient::commandReceived, killSwitch, &KillSwitch::handle);
    auto *audio  = new AudioCapture(&app);
    auto *voice  = new VoiceClient(auth, &app);
    auto *advice = new AdviceClient(auth, &app);

    // Применяем сохранённые аудио-настройки (применятся при следующем start()).
    // Звук игры нужен сценовым режимам (Адвокат/Прокурор/Общий); Q&A слушает только микрофон.
    audio->setMicDeviceId(settings::audio().micDeviceId);
    audio->setGameProcessExe(settings::audio().gameProcessExe);

    // Аудио-кадры из захвата (мик/loopback) → в голосовой WS-клиент.
    QObject::connect(audio, &AudioCapture::frame, voice, &VoiceClient::onFrame);

    // --- Бесшовное автообновление (фоновое). Создаём ДО окон, чтобы OverlayWindow при создании
    //     сразу получил updater (уведомления «обновление готово»). ---
    auto *updater = new Updater(&app);
    bool *applyingLaunch = new bool(false);
    QObject::connect(updater, &Updater::aboutToApply, &app, [=]() {
        *applyingLaunch = true;
        qApp->quit();   // выходим → RpUpdater подменит файлы и перезапустит приложение
    });

    // --- ОКНО АКТИВАЦИИ (отдельное) показывается ПЕРВЫМ, пока нет валидной авторизации по ключу.
    //     Сам оверлей создаётся/показывается ТОЛЬКО после успешной активации/входа. ---
    auto *actwin = new ActivationWindow(license);
    *actBox = actwin;
    // Успешная активация ключа → токены в AuthClient (→ authorized). Ошибка → текст в окне активации.
    QObject::connect(license, &LicenseClient::activated, &app,
                     [=](QString a, QString r, int ttl, QString t) { auth->acceptTokens(a, r, ttl, t); });
    QObject::connect(license, &LicenseClient::activateError, actwin, &ActivationWindow::showError);

    // Сплэш гасим РОВНО когда определилось, что показывать. Объявлен ДО ensureOverlay (модель-гейт зовёт).
    bool *splashClosed = new bool(false);
    auto closeSplash = [=]() {
        if (*splashClosed) return;
        *splashClosed = true;
        // hide(), НЕ deleteLater: периодическая проверка апдейта (каждые 4ч) с mandatoryUpdate зовёт
        // splash->setText/show — при deleteLater это был use-after-free (краш/порча памяти после логина).
        // splash — одна маленькая QLabel, живёт до конца процесса.
        if (splash) splash->hide();
    };

    // Игровой бар создаётся ОДИН раз (лениво): кнопкой «Запустить оверлей» из хаба, переходом
    // в настройки (создаётся СКРЫТЫМ — диалог и его виринг живут в OverlayWindow) либо сразу
    // после авторизации (тумблер overlayAutostart — старое поведение ≤0.3.x).
    bool *started = new bool(false);
    auto ensureOverlay = [=, &app]() {
        if (*started)
            return;
        *started = true;
        // Трей общий: иконкой владеет хаб (создаётся раньше бара во всех путях), бар получает
        // её только для balloon-уведомлений и свою не создаёт.
        auto *w = new OverlayWindow(auth, audio, voice, advice,
                                    (*hubBox) ? (*hubBox)->tray() : nullptr);
        w->setUpdater(updater);   // уведомление «обновление готово» в трей
        *winBox = w;              // создано → IPC-активация (повторный запуск) поднимет его
    };

    // prepareForLaunch (проверка/докачка STT-модели) уже вызван в ЭТОМ сеансе авторизации.
    // Отдельный флаг, а НЕ «бар только что создан»: бар мог быть создан переходом в настройки
    // (settingsRequested → ensureOverlay) ДО authorized-вызова гейта — по «firstCreate» кнопка
    // запуска тогда не взводила бы докачку вовсе, и у свежего юзера PTT молча упирался бы
    // в отсутствие модели до следующего token-refresh.
    bool *modelsPrepared = new bool(false);

    // --- ХАБ (лаунчер) — главное окно приложения с 0.4.0. Создаётся один раз по authorized;
    //     владеет треем; бар запускается его кнопкой/меню трея. ---
    auto ensureHub = [=, &app]() {
        if (*hubBox)
            return;
        auto *h = new HubWindow(auth);
        // Хаб читает состояние бара (не запущен/работает/скрыт) через провайдер — сам бар
        // он не создаёт и им не владеет (создание — только через сигналы ниже).
        h->setOverlayProvider([winBox]() -> QWidget * { return *winBox; });
        // «Запустить/Показать оверлей» (кнопка хаба или меню трея).
        QObject::connect(h, &HubWindow::launchOverlayRequested, &app, [=]() {
            if (!auth->isAuthorized())
                return;   // без авторизации бар не поднимаем (хаб в этом состоянии и так скрыт)
            ensureOverlay();
            (*winBox)->showFromExternal();
            // Модель локального STT — НЕ-блокирующий гейт: бар показывается сразу (текстовый
            // ввод работает без модели), проверка/докачка идёт в фоне, статус виден в баре.
            // Один раз за сеанс (modelsPrepared): обычно это делает authorized-хендлер, но при
            // запуске без автостарта бара он гейт пропустил — взводим при первом запуске бара.
            if (!*modelsPrepared) {
                if (ModelManager *mm = (*winBox)->models()) {
                    *modelsPrepared = true;
                    mm->prepareForLaunch();
                }
            }
            h->refreshStatus();
        });
        // «Скрыть оверлей» — бар прячется (хоткеи/трей продолжают работать, как minimizeToTray).
        QObject::connect(h, &HubWindow::hideOverlayRequested, &app, [=]() {
            if (*winBox) (*winBox)->hide();
            h->refreshStatus();
        });
        // «Настройки» из хаба/трея: окно настроек ЕДИНОЕ с баром (SettingsDialog + всё
        // применение «на лету» живут в OverlayWindow) — создаём бар скрытым, если его ещё
        // нет, и открываем его диалог. Показ бара это не вызывает.
        QObject::connect(h, &HubWindow::settingsRequested, &app, [=]() {
            if (!auth->isAuthorized())
                return;
            ensureOverlay();
            // Гард от второго вложенного диалога живёт в самом onOpenSettings (единая точка: покрывает
            // и этот путь, и прямой вызов шестерёнкой бара). Трей-попап «Настройки…» доступен поверх
            // модалки, но повторный onOpenSettings коротко выйдет по inSettingsDlg_.
            if (*winBox) (*winBox)->onOpenSettings();   // модальный exec() — блокирует до закрытия
        });
        *hubBox = h;
    };

    // Показ окон по входу — только на ПЕРВОМ authorized за сеанс: сигнал эмитится и на каждый
    // тихий refresh токена, а окна не должны выпрыгивать, если пользователь их свернул.
    bool *sessionUp = new bool(false);

    // Нет валидного токена / потеряли сессию (logout, бан, смена железа) → окно активации поверх,
    // хаб и оверлей прячем. Авторизованы → прячем активацию, поднимаем хаб.
    QObject::connect(auth, &AuthClient::needKey, &app, [=]() {
        *sessionUp = false;       // следующий вход = новый сеанс → хаб снова покажем
        *modelsPrepared = false;  // ... и модель-гейт после релогина отработает заново
        if (*winBox) (*winBox)->hide();
        if (*hubBox) (*hubBox)->hide();
        actwin->resetForInput();
        actwin->showCentered();
        closeSplash();
    });
    QObject::connect(auth, &AuthClient::authorized, &app, [=](QString) {
        ensureHub();
        actwin->hide();
        if (!*sessionUp) {
            *sessionUp = true;
            // Старое поведение по тумблеру: бар сразу при старте (хаб при этом тоже создан —
            // его можно свернуть в трей). Дефолт: только хаб, бар — кнопкой из него.
            if (settings::overlayAutostart()) {
                ensureOverlay();
                (*winBox)->show();
            }
            (*hubBox)->showFromTray();
        }
        closeSplash();
        // Перепроверка железа/ключа на КАЖДОМ запуске (даже после silent-refresh). Ловит копирование
        // токенов на другой ПК / смену ядра железа: эти проверки НЕ делает refresh (он верит claim'у).
        // laws-билд ПРОПУСКАЕТ: универсальный ключ не привязан к железу (hwid_pubkey нет) → verify
        // вернул бы not_activated и ложно выкинул юзера в активацию. Отзыв ключа здесь ловит refresh
        // (AuthClient: key_revoked → needKey → окно активации).
#ifndef RPOVERLAY_LAWS
        license->verifyHardware();
#endif
        // Модель локального STT (гейт остаётся за баром): если бар ещё не создан (запуск без
        // автостарта), проверку/докачку взведёт кнопка «Запустить оверлей» в хабе (по флагу
        // modelsPrepared — даже если бар до этого создали скрытым переходом в настройки).
        if (ModelManager *mm = (*winBox) ? (*winBox)->models() : nullptr) {
            *modelsPrepared = true;
            mm->prepareForLaunch();
        }
        (*hubBox)->refreshStatus();
    });
    QObject::connect(auth, &AuthClient::authError, &app, [=](QString m) {
        actwin->showError(QStringLiteral("auth"), m);
        closeSplash();
    });
    // Результат перепроверки железа. Фатальные коды (машина больше не привязана / доступ снят) →
    // сбрасываем ключ-авторизацию и возвращаем к окну активации. Сетевые/временные — игнор (повторим).
    QObject::connect(license, &LicenseClient::hardwareError, &app, [=](QString code, QString human) {
        const bool fatal = (code == QStringLiteral("hwid_mismatch") || code == QStringLiteral("not_activated")
                            || code == QStringLiteral("hwid_banned") || code == QStringLiteral("tier_lost")
                            || code == QStringLiteral("tester_only"));
        if (fatal) {
            qWarning().noquote() << "[main] hardware-verify ФАТАЛЬНО:" << code << "→ окно активации";
            if (*winBox) (*winBox)->hide();
            auth->logout();                 // обнулить ключ-авторизацию (не та машина / доступ снят)
            actwin->resetForInput();
            actwin->showError(code, human);
            actwin->showCentered();
        } else {
            qWarning().noquote() << "[main] hardware-verify нефатально:" << code << human;
        }
    });

    // Если с прошлого сеанса есть проверенное staged-обновление — применяем молча на старте.
    updater->maybeApplyOnLaunch();

    if (!*applyingLaunch) {
        // --- Харденинг (Пиллар C): report-only самоконтроль (анти-дебаг + integrity self-check) ---
        auto *hardening = new Hardening(&app);
        QObject::connect(hardening, &Hardening::anomaly, &app, [telemetry](const QString &kind, const QString &detail) {
            qWarning().noquote() << "[main] харденинг:" << kind << "→" << detail;
            // Слой D: довести сигнал до сервера по ОБОИМ каналам (раньше умирал в qWarning).
            telemetry->report(kind, detail);
        });
        hardening->startPeriodic(30);          // первый прогон ~через 12с, далее каждые 30 мин

        // --- ФОРС-обновление: старую версию НЕ пускаем, если на сервере есть ОБЯЗАТЕЛЬНОЕ обновление
        //     (mandatory или текущая < min_supported). Делаем РАННЮЮ проверку манифеста под сплэшем и
        //     пускаем пользователя дальше ТОЛЬКО по checkConcluded. На mandatoryUpdate — блокирующий
        //     экран, качаем+применяем (рестарт уже на новой версии). ---
        bool *gateOpen = new bool(false);
        bool *forcedNow = new bool(false);
        auto openGate = [=]() {
            if (*gateOpen || *forcedNow) return;     // форс уже идёт / уже пустили — не дублируем
            *gateOpen = true;
            updater->startPeriodic(4);               // дальше — фоновые проверки каждые 4ч
            auth->begin();                           // → activation/overlay
        };
        QObject::connect(updater, &Updater::checkConcluded, &app, openGate);
        // Обновление НЕ встаёт (своп падал kMaxApplyAttempts раз — напр. AV держит свежий бинарь): НЕ
        // циклим. Updater уже прислал checkConcluded (openGate пустил на текущей версии) — здесь лишь
        // разово уведомляем юзера в трее. Хаб/трей появляется по авторизации (позже раннего чека) —
        // показываем с задержкой, если трей уже виден.
        QObject::connect(updater, &Updater::updateStuck, &app, [splash, hubBox, forcedNow, gateOpen, openGate](const QString &ver) {
            qWarning().noquote() << "[main] обновление" << ver
                                 << "не устанавливается (своп падает) → fail-open, работаем на текущей";
            // Снять блок форс-сплэша: mandatory-путь мог взвести forcedNow в ЭТОЙ же сессии (RpUpdater.exe
            // закарантинен AV → applyStaged фейлится без выхода) → без сброса openGate заблокирован и юзер
            // вис бы на сплэше «Повтор…» навсегда. Пускаем в приложение на текущей версии.
            *forcedNow = false;
            // ФОНОВАЯ 4ч-проверка (gate УЖЕ открыт с запуска): mandatoryUpdate спрятал хаб/бар и поднял
            // топовый сплэш ПОВЕРХ игры. openGate() тут no-op (gateOpen==true), а closeSplash одноразовый
            // (отработал на логине) → без явного hide сплэш висел бы по центру экрана до перезапуска
            // процесса (ровно инцидент 2026-07-19). Гасим сплэш и возвращаем хаб из трея сами.
            if (*gateOpen) {
                splash->hide();
                if (*hubBox) (*hubBox)->showFromTray();
            }
            openGate();   // запусковый mandatory-stuck (gate ещё закрыт): пустит через auth->begin → закроет сплэш
            QTimer::singleShot(4000, qApp, [hubBox]() {
                QSystemTrayIcon *tray = (*hubBox) ? (*hubBox)->tray() : nullptr;
                if (tray && tray->isVisible())
                    tray->showMessage(
                        QStringLiteral("Обновление не установилось"),
                        QStringLiteral("Антивирус мог заблокировать файлы обновления. Добавьте папку "
                                       "оверлея в исключения антивируса или переустановите последнюю "
                                       "версию. Приложение продолжает работать."),
                        QSystemTrayIcon::Warning, 12000);
            });
        });
        // Любое обновление при ЗАПУСКЕ — блокирующее: качаем с прогрессом → применяем → стартует
        // уже НОВАЯ версия. Серверный форс (mandatory/min_supported) дополнительно «жёсткий»:
        // при сбое скачивания ретраит и НЕ пускает в старую; обычный апдейт при сбое — fail-open.
        QObject::connect(updater, &Updater::mandatoryUpdate, &app, [=](const QString &ver, const QString &) {
            *forcedNow = true;                       // блокируем вход до рестарта на новую версию
            if (*winBox) (*winBox)->hide();
            if (*hubBox) (*hubBox)->hide();
            actwin->hide();
            splash->setText(updater->serverForced()
                ? QStringLiteral("Обязательное обновление %1…\nСтарая версия отключена.").arg(ver)
                : QStringLiteral("Обновление до %1…").arg(ver));
            splash->show(); splash->raise();
        });
        QObject::connect(updater, &Updater::progress, &app, [=](qint64 rcv, qint64 total, int idx, int cnt) {
            if (!*forcedNow) return;                 // прогресс рисуем только для блокирующего апдейта
            const int pct = total > 0 ? int(rcv * 100 / total) : 0;
            splash->setText(updater->serverForced()
                ? QStringLiteral("Обязательное обновление: файл %1/%2 (%3%)…\nСтарая версия отключена.")
                      .arg(idx + 1).arg(cnt).arg(pct)
                : QStringLiteral("Скачиваю обновление: файл %1/%2 (%3%)…").arg(idx + 1).arg(cnt).arg(pct));
        });
        QObject::connect(updater, &Updater::errorOccurred, &app, [=](const QString &) {
            if (!*forcedNow) return;
            if (updater->serverForced()) {
                splash->setText(QStringLiteral("Не удалось скачать обязательное обновление. Повтор…"));
                QTimer::singleShot(5000, updater, &Updater::checkNow);   // ретрай до победного
            } else {
                // Обычное обновление при запуске не должно запирать юзера из-за сети:
                // пускаем на текущей версии, докачается фоновой проверкой → применится на след. запуске.
                qWarning().noquote() << "[main] launch-обновление сорвалось (сеть) → fail-open, работаем на текущей";
                if (*gateOpen) splash->hide();   // симметрия с updateStuck: если gate уже открыт — не оставляем сплэш
                *forcedNow = false;
                openGate();
            }
        });
        // Подстраховка: если ранняя проверка зависла (ни checkConcluded, ни mandatory) — через 10с
        // пускаем (fail-open: сетевой сбой не должен запирать клиент намертво).
        QTimer::singleShot(10000, &app, openGate);

        splash->setText(QStringLiteral("Проверка обновлений…"));
        updater->checkNow();                          // ранняя проверка обязательного обновления
    } else {
        // Применяется staged-обновление на старте: RpUpdater УЖЕ запущен (applyStaged) и ждёт нашего
        // выхода. Выходим СРАЗУ, НЕ входя в event loop: иначе qApp->quit() был вызван ДО app.exec()
        // (в aboutToApply-слоте) и мог оказаться no-op → невидимый зомби-процесс держал бы
        // single-instance мьютекс, и новые запуски молча выходили бы («не запускается» до ребута).
        closeSplash();   // применяется апдейт → окон не показываем
        return 0;
    }

    return app.exec();
}
