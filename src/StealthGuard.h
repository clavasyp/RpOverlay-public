#pragma once

#include <QObject>
#include <qwindowdefs.h>   // WId

class QEvent;

// «Режим невидимка» (stealth) — фича ТЕСТЕРСКОГО full-билда. Централизованно прячет ВСЕ окна
// приложения из захвата экрана/OBS через SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE),
// включая транзиентные попапы (меню режима/трея, выпадашки QComboBox, QToolTip) — их ловит
// глобальный event-фильтр на qApp по QEvent::Show. Скрытие Hub/Activation из панели задач и
// Alt-Tab делается их флагами окна (Qt::Tool при masterOn) в их конструкторах.
//
// В публичной QA-сборке (RPOVERLAY_QA) режим вкомпилен в OFF — не активируется даже ручной
// правкой settings.json. Существующий «лёгкий» тумблер бара (captureInvisible) не затрагивается:
// он остаётся во всех сборках и работает как раньше.
namespace stealth {

// Мастер-режим включён? settings::window().stealthMode; в RPOVERLAY_QA — всегда false.
bool masterOn();

// Бар должен быть скрыт из захвата: masterOn ИЛИ старый тумблер captureInvisible.
bool barHidden();

// Низкоуровневая установка WDA-аффинити на native-окно (no-op вне Windows / при нулевом hwnd).
// hide=true → WDA_EXCLUDEFROMCAPTURE, false → WDA_NONE.
void applyAffinity(WId wid, bool hide);

// Переустановить аффинити на ВСЕ открытые сейчас top-level окна по текущему состоянию режима.
// Бар пропускается (держит своё аффинити сам, с учётом captureInvisible). Зовётся на живом
// переключении тумблера, чтобы уже открытые окна (Hub/настройки) сменили видимость без рестарта.
void sweepAllTopLevel();

// Установить глобальный event-фильтр на qApp (один раз из main, ДО показа любых окон): каждое
// НОВОЕ top-level окно при masterOn получает WDA_EXCLUDEFROMCAPTURE (в т.ч. транзиентные попапы).
void install();

} // namespace stealth
