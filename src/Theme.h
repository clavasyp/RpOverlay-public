#pragma once

#include <QString>
#include <QColor>

// Тема оформления оверлея. Сейчас две:
//   • "graphite" — историческая «Графит + индиго» (дефолт; вид НЕ меняется ни на пиксель);
//   • "majestic" — фирменный стиль проекта Majestic RP (near-black + crimson-акцент,
//                  бренд-логотип в шапке). Опт-ин, переключается в Настройках.
//
// Весь QSS панели/диалога раньше жил в двух статических строках (kPanelStyle/kDialogStyle).
// Теперь их отдаёт этот модуль в зависимости от активной темы. Цвета, которые задаются не
// через QSS, а кодом (чек-лист, цвет текста транскрипта), берутся из palette().
namespace theme {

// Цвета для код-рендеринга (не покрываются QSS).
struct Palette {
    QColor checklistOk;        // статус «ok» в чек-листе
    QColor checklistErr;       // статус «violation»
    QColor checklistUnknown;   // статус «unclear» / пусто
    QColor transcriptFinal;    // финальная строка транскрипта
    QColor transcriptPartial;  // «живой» partial (приглушённый)
};

void setActive(const QString &name);   // "graphite" | "majestic" (неизвестное → "graphite")
QString active();                       // текущее имя темы
bool isMajestic();                      // active() == "majestic"

const Palette &palette();               // палитра активной темы

QString panelStyle();                   // QSS для главного окна (#panel и потомки)
QString dialogStyle();                  // QSS для SettingsDialog

} // namespace theme
