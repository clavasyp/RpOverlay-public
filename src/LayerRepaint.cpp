#include "LayerRepaint.h"
#include "DebugLog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QDebug>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QToolTip>
#include <QWidget>
#include <QWindow>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace layerfix {
namespace {

// Слоёное окно = то самое, у которого пропадает автоперерисовка: полупрозрачный фон.
bool isTranslucentWindow(const QWidget *w)
{
    return w && w->isWindow() && w->testAttribute(Qt::WA_TranslucentBackground);
}

bool isPopupWindow(const QWidget *w)
{
    if (!w || !w->isWindow()) return false;
    const Qt::WindowType t = w->windowType();
    return t == Qt::Popup || t == Qt::ToolTip;
}

class Repainter : public QObject {
public:
    explicit Repainter(QObject *parent) : QObject(parent)
    {
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(0);   // схлопывание: пачка событий → один update() на итерацию цикла
        connect(timer_, &QTimer::timeout, this, &Repainter::flush);

        sweep_ = new QTimer(this);
        sweep_->setInterval(1000);
        connect(sweep_, &QTimer::timeout, this, &Repainter::sweepOrphanPopups);
        sweep_->start();
    }

    void schedule(QWidget *w)
    {
        if (!isTranslucentWindow(w)) return;
        scheduleWidget(w);
    }

    // Без проверки на прозрачность — для самой выпадашки: она бывает показана, но ни разу
    // не отрисована (пустой прямоугольник с рамкой).
    void scheduleWidget(QWidget *w)
    {
        if (!w) return;
        for (const QPointer<QWidget> &p : pending_)
            if (p == w) return;                  // уже запланировано (список — единицы окон)
        pending_.append(QPointer<QWidget>(w));
        if (!timer_->isActive())
            timer_->start();
    }

    void scheduleAll()
    {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->isVisible())
                schedule(w);
    }

    // Выпадашка/меню — ОТДЕЛЬНОЕ top-level окно, а не часть карточки. Если окно-хозяин
    // спрячется, пока список открыт (свернули в трей, хоткей скрытия бара, закрыли настройки),
    // список остаётся висеть сам по себе — прямоугольник с рамкой поверх игры, который уже
    // некому ни перерисовать, ни закрыть. Уводим попапы вместе с их окном.
    static void closeOwnedPopups(const QWidget *host)
    {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w == host || !w->isVisible() || !isPopupWindow(w))
                continue;
            const QWidget *owner = w->parentWidget();
            if (owner && owner->window() == host)
                w->hide();
        }
    }

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        // Быстрый выход: подавляющее большинство событий сюда не относится.
        switch (ev->type()) {
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::LayoutRequest:
            break;
        default:
            return false;
        }
        if (!obj->isWidgetType()) return false;
        auto *w = static_cast<QWidget *>(obj);

        if (isPopupWindow(w)) {
            // Выпадашка/меню/подсказка. При ЗАКРЫТИИ под ней остаётся прозрачная дыра, при
            // ПОКАЗЕ она сама может встать неотрисованной — в обоих случаях перекрашиваем
            // все слоёные окна: попап мог перекрыть любое из них (бар, хаб, настройки).
            scheduleAll();
            if (ev->type() == QEvent::Show)
                scheduleWidget(w);   // форс первой отрисовки самого списка
            return false;
        }

        if (ev->type() == QEvent::Hide && w->isWindow())
            closeOwnedPopups(w);   // окно уходит — его выпадашки не должны его пережить

        // Показ/скрытие обычного виджета внутри слоёного окна (смена страницы, появление
        // секции, перезаполнение списка) — регион под ним тоже надо перерисовать.
        schedule(w->window());
        return false;
    }

private:
    // Подметание осиротевших выпадашек.
    //
    // Симптом с прода: в окне настроек накапливались ПУСТЫЕ прямоугольники размером со
    // список — по одному на каждое открытие выпадашки, и они переживали закрытие окна,
    // оставаясь висеть поверх игры. То есть Qt-попап логически закрыт (grab снят), а его
    // окно осталось показанным.
    //
    // Критерий сироты предельно узкий: активного попапа в приложении НЕТ вообще
    // (QApplication::activePopupWidget() == nullptr), а окно-попап всё ещё видимо. Тогда
    // закрывать нечему — это остаток. Подтверждения ждём ДВА тика подряд, чтобы не убить
    // список, который только что создан и ещё не успел стать активным. QToolTip намеренно
    // не трогаем: подсказки активным попапом не считаются и гасят себя сами.
    // Вторая, более грубая разновидность фантома: Qt СЧИТАЕТ окно скрытым, а нативное окно
    // осталось на экране. Тогда сторож выше его не видит (isVisible()==false), и пустой
    // прямоугольник висит, пока не перезапустишь приложение.
    //
    // Сверка безопасна по построению: трогаем ровно те окна, которые сам Qt уже считает
    // скрытыми, — показать что-то живое она не может в принципе. windowHandle() (а НЕ winId())
    // намеренно: winId() создал бы нативное окно там, где его ещё нет.
    static void reconcileNativeVisibility()
    {
#ifdef Q_OS_WIN
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w->isVisible()) continue;
            QWindow *h = w->windowHandle();
            if (!h) continue;
            HWND hwnd = reinterpret_cast<HWND>(h->winId());
            if (hwnd && IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_HIDE);
        }
#endif
    }

    // Третья, самая живучая разновидность: нативного окна НЕТ ни у одного живого QWidget —
    // Qt-объект уже умер, а окно осталось на экране. Такое не видит ни один сторож выше,
    // потому что все они ходят по виджетам Qt. Здесь идём от системы: перебираем окна
    // НАШЕГО процесса и ищем видимые, которым не соответствует ни один живой QWidget.
    //
    // Гарды, чтобы случайно не погасить что-то живое: только наш процесс, только класс окна
    // Qt («Qt…»), только видимые, и обязательно ДВА тика подряд в этом состоянии.
    void sweepOrphanNativeWindows()
    {
#ifdef Q_OS_WIN
        QSet<HWND> alive;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (QWindow *h = w->windowHandle())
                alive.insert(reinterpret_cast<HWND>(h->winId()));

        struct Ctx { QSet<HWND> *alive; QList<HWND> *found; DWORD pid; };
        QList<HWND> found;
        Ctx ctx{ &alive, &found, GetCurrentProcessId() };

        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto *c = reinterpret_cast<Ctx *>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != c->pid)          return TRUE;   // чужой процесс
            if (!IsWindowVisible(hwnd)) return TRUE;
            if (c->alive->contains(hwnd)) return TRUE; // окно живого виджета Qt
            wchar_t cls[128] = {0};
            GetClassNameW(hwnd, cls, 127);
            if (wcsncmp(cls, L"Qt", 2) != 0) return TRUE;   // не окно Qt — не наше дело
            c->found->append(hwnd);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        QList<HWND> stillOrphan;
        for (HWND hwnd : found) {
            if (nativeSuspects_.contains(hwnd)) {
                RECT r{};
                GetWindowRect(hwnd, &r);
                wchar_t cls[128] = {0};
                GetClassNameW(hwnd, cls, 127);
                qWarning().noquote()
                    << "[layerfix] осиротевшее нативное окно — гашу:"
                    << QString::fromWCharArray(cls)
                    << QStringLiteral("%1x%2 @%3,%4").arg(r.right - r.left).arg(r.bottom - r.top)
                           .arg(r.left).arg(r.top);
                ShowWindow(hwnd, SW_HIDE);
            } else {
                stillOrphan.append(hwnd);
            }
        }
        nativeSuspects_ = QSet<HWND>(stillOrphan.begin(), stillOrphan.end());
#endif
    }

    // Залипшая ВСПЛЫВАЮЩАЯ ПОДСКАЗКА — то, чем фантом и оказался по описи окон с прода:
    //   #4 class=Qt6110QWindowToolSaveBits 280x55 владелец=QLabel
    // (QToolTip внутри — это QLabel в отдельном окне класса Tool). Размер совпал со скриншотом,
    // позиция — вплотную к бару: это подсказка микрофона, показанная и не отрисованная.
    //
    // Раньше подсказки были ЯВНО исключены из сторожа («гаснут сами»). Обычно да — у Qt свой
    // таймер, около 10-12 секунд. Но если подсказка залипла, гасить её больше некому: активным
    // попапом она не считается, и ни один из прежних сторожей её не видел.
    //
    // Два критерия, оба консервативные:
    //   • Qt считает, что подсказки нет (QToolTip::isVisible()==false), а окно висит — 2 тика;
    //   • окно висит дольше kTipMaxTicks секунд — это уже за пределами любого штатного таймера.
    void sweepStuckTooltips()
    {
        constexpr int kTipMaxTicks = 20;   // 20 с — заведомо больше собственного таймаута Qt
        QHash<QWidget *, int> next;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w->windowType() != Qt::ToolTip || !w->isVisible())
                continue;   // пропал/скрылся — счётчик обнулится сам, его просто нет в next
            const int ticks = tipTicks_.value(w, 0) + 1;
            const bool qtSaysNoTip = !QToolTip::isVisible();
            if ((qtSaysNoTip && ticks >= 2) || ticks >= kTipMaxTicks) {
                qWarning().noquote()
                    << "[layerfix] залипшая подсказка — гашу:" << w->metaObject()->className()
                    << QStringLiteral("%1x%2, висела %3 с")
                           .arg(w->width()).arg(w->height()).arg(ticks);
                w->hide();
                continue;   // в next не кладём
            }
            next.insert(w, ticks);
        }
        tipTicks_ = next;
    }

    void sweepOrphanPopups()
    {
        reconcileNativeVisibility();
        sweepOrphanNativeWindows();
        sweepStuckTooltips();
        if (QApplication::activePopupWidget()) {
            suspects_.clear();
            return;
        }
        QList<QPointer<QWidget>> stillOpen;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (!w->isVisible() || w->windowType() != Qt::Popup)
                continue;
            bool seenBefore = false;
            for (const QPointer<QWidget> &p : suspects_)
                if (p == w) { seenBefore = true; break; }
            if (seenBefore)
                w->hide();          // второй тик подряд без активного попапа — точно остаток
            else
                stillOpen.append(QPointer<QWidget>(w));
        }
        suspects_ = stillOpen;
    }

    void flush()
    {
        // QPointer, а не сырой указатель: окно настроек живёт на стеке вызывающего и может
        // быть разрушено между планированием и срабатыванием таймера.
        const auto batch = pending_;
        pending_.clear();
        for (const QPointer<QWidget> &p : batch) {
            if (!p || !p->isVisible())
                continue;
            p->update();
#ifdef Q_OS_WIN
            // Для самой выпадашки одного update() бывает мало: окно уже показано, а первого
            // флаша слоя так и не случилось — остаётся пустой прямоугольник с рамкой. Здесь
            // мы уже вне обработки показа, поэтому можно потребовать отрисовку немедленно.
            if (isPopupWindow(p) && p->windowHandle()) {
                HWND hwnd = reinterpret_cast<HWND>(p->windowHandle()->winId());
                if (hwnd)
                    RedrawWindow(hwnd, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
#endif
        }
    }

    QList<QPointer<QWidget>> pending_;
    QHash<QWidget *, int> tipTicks_;      // сколько секунд подсказка висит (ключ — только для сравнения)
    QList<QPointer<QWidget>> suspects_;   // попапы, видимые без активного попапа (кандидаты в сироты)
#ifdef Q_OS_WIN
    QSet<HWND> nativeSuspects_;           // нативные окна без живого виджета (кандидаты, ждём 2-й тик)
#endif
    QTimer *timer_ = nullptr;
    QTimer *sweep_ = nullptr;
};

Repainter *g_repainter = nullptr;

} // namespace

void install()
{
    if (g_repainter) return;
    g_repainter = new Repainter(qApp);
    qApp->installEventFilter(g_repainter);
}

void scheduleAllTranslucent()
{
    if (g_repainter)
        g_repainter->scheduleAll();
}

void dumpWindowInventory()
{
#ifdef Q_OS_WIN
    // Сопоставление hwnd → живой виджет Qt: по нему видно, «своё» окно или бесхозное.
    QHash<HWND, QWidget *> byHwnd;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (QWindow *h = w->windowHandle())
            byHwnd.insert(reinterpret_cast<HWND>(h->winId()), w);

    debuglog::appendAlways(QStringLiteral("[layerfix] ===== ОПИСЬ ОКОН ПРОЦЕССА ====="));
    debuglog::appendAlways(QStringLiteral("[layerfix] версия %1, время %2")
                               .arg(QCoreApplication::applicationVersion(),
                                    QDateTime::currentDateTime().toString(Qt::ISODate)));
    debuglog::appendAlways(QStringLiteral("[layerfix] активный попап: %1, модалка: %2")
                             .arg(QApplication::activePopupWidget()
                                      ? QString::fromLatin1(QApplication::activePopupWidget()->metaObject()->className())
                                      : QStringLiteral("нет"))
                             .arg(QApplication::activeModalWidget()
                                      ? QString::fromLatin1(QApplication::activeModalWidget()->metaObject()->className())
                                      : QStringLiteral("нет")));

    struct Ctx { QHash<HWND, QWidget *> *byHwnd; DWORD pid; int n; };
    Ctx ctx{ &byHwnd, GetCurrentProcessId(), 0 };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid) return TRUE;

        wchar_t cls[128] = {0}, title[128] = {0};
        GetClassNameW(hwnd, cls, 127);
        GetWindowTextW(hwnd, title, 127);
        RECT r{};
        GetWindowRect(hwnd, &r);
        const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

        QStringList flags;
        if (ex & WS_EX_LAYERED)     flags << QStringLiteral("LAYERED");
        if (ex & WS_EX_TRANSPARENT) flags << QStringLiteral("TRANSPARENT");
        if (ex & WS_EX_TOOLWINDOW)  flags << QStringLiteral("TOOL");
        if (ex & WS_EX_TOPMOST)     flags << QStringLiteral("TOPMOST");
        if (ex & WS_EX_NOACTIVATE)  flags << QStringLiteral("NOACTIVATE");

        QWidget *w = c->byHwnd->value(hwnd, nullptr);
        const QString owner = w
            ? QStringLiteral("%1 (objectName=%2, видим по Qt=%3)")
                  .arg(QString::fromLatin1(w->metaObject()->className()),
                       w->objectName().isEmpty() ? QStringLiteral("—") : w->objectName(),
                       w->isVisible() ? QStringLiteral("да") : QStringLiteral("НЕТ"))
            : QStringLiteral("*** живого виджета Qt НЕТ ***");

        debuglog::appendAlways(QStringLiteral("[layerfix] #%1 class=%2 title=\"%3\" %4x%5 @%6,%7 "
                                            "видимо=%8 стили=[%9] владелец=%10")
                                 .arg(++c->n)
                                 .arg(QString::fromWCharArray(cls),
                                      QString::fromWCharArray(title))
                                 .arg(r.right - r.left).arg(r.bottom - r.top)
                                 .arg(r.left).arg(r.top)
                                 .arg(IsWindowVisible(hwnd) ? QStringLiteral("да") : QStringLiteral("нет"),
                                      flags.join(QLatin1Char('|')),
                                      owner));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    debuglog::appendAlways(QStringLiteral("[layerfix] ===== конец описи, окон: %1 =====").arg(ctx.n));
#endif
}

} // namespace layerfix
