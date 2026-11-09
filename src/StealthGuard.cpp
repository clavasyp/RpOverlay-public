#include "StealthGuard.h"
#include "Settings.h"

#include <QApplication>
#include <QWidget>
#include <QWindow>
#include <QEvent>
#include <QMetaObject>
#include <QDebug>

#ifdef Q_OS_WIN
#  include <windows.h>
// WDA_EXCLUDEFROMCAPTURE появился в Win10 2004; на старых SDK/системах определяем сами и
// молча деградируем (окно останется видимым в записи — не фатально).
#  ifndef WDA_NONE
#    define WDA_NONE 0x00
#  endif
#  ifndef WDA_EXCLUDEFROMCAPTURE
#    define WDA_EXCLUDEFROMCAPTURE 0x11
#  endif
#endif

namespace stealth {
namespace {

// Глобальный фильтр приложения: при показе любого нового top-level окна вешает аффинити,
// пока активен мастер-режим. Ловит и попапы, которые per-widget nativeEvent пропустил бы.
class Guard : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() != QEvent::Show) return false;   // O(1) быстрый выход для всех прочих событий
        if (!masterOn())                return false;   // режим выкл — окна не трогаем (бар держит своё)
        if (!obj->isWidgetType())       return false;
        auto *w = static_cast<QWidget *>(obj);
        if (w->isWindow())
            applyAffinity(w->winId(), /*hide*/true);
        return false;   // событие не поглощаем
    }
};

Guard *g_guard = nullptr;

} // namespace

bool masterOn()
{
#ifdef RPOVERLAY_QA
    return false;   // публичный QA-билд: мастер-невидимка недоступна (только тестерский full-билд)
#else
    return settings::window().stealthMode;
#endif
}

bool barHidden()
{
    return masterOn() || settings::window().captureInvisible;
}

void applyAffinity(WId wid, bool hide)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(wid);
    if (!hwnd) return;
    const DWORD aff = hide ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (!SetWindowDisplayAffinity(hwnd, aff))
        qWarning().noquote() << "[stealth] SetWindowDisplayAffinity не применился (Win10 <2004?), err="
                             << static_cast<qulonglong>(GetLastError());
#else
    Q_UNUSED(wid);
    Q_UNUSED(hide);
#endif
}

void sweepAllTopLevel()
{
    const bool master = masterOn();
    const auto tops = QApplication::topLevelWidgets();
    for (QWidget *w : tops) {
        if (!w->isWindow())     continue;
        if (!w->windowHandle()) continue;                    // ещё нет native-хэндла — пропускаем
        if (w->property("stealthIsBar").toBool()) continue;  // бар держит своё аффинити сам
        applyAffinity(w->winId(), master);
        // Владелец трея (хаб) — на живом переключении режима скрыть/показать иконку трея.
        if (w->property("stealthTrayOwner").toBool())
            QMetaObject::invokeMethod(w, "applyStealthTray");
    }
}

void install()
{
    if (g_guard) return;
    g_guard = new Guard(qApp);
    qApp->installEventFilter(g_guard);
}

} // namespace stealth
