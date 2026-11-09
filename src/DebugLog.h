#pragma once

#include <QString>

// Режим отладки для тестеров: при включении открывается КОНСОЛЬ (терминал) с живыми
// подробными логами + те же строки пишутся в ФАЙЛ (%APPDATA%/.../logs/overlay-debug.log),
// чтобы тестер мог прислать лог. Управляется тумблером в настройках; персист в settings.json
// (settings::debugMode). На старте main.cpp применяет сохранённое значение.
namespace debuglog {

void setEnabled(bool on);   // вкл: AllocConsole + qInstallMessageHandler + лог-файл + verbose; выкл: всё снять
bool isEnabled();

QString logDir();           // папка логов
QString logFilePath();      // путь к overlay-debug.log
void openLogDir();          // открыть папку логов в проводнике

// Записать строку в лог-файл НЕЗАВИСИМО от режима отладки (файл создаётся при необходимости).
// Обычный qInfo() пишется в файл ТОЛЬКО при включённом режиме — а диагностику тестер запускает
// кнопкой, не включая его, и тогда запись молча уходила в никуда.
void appendAlways(const QString &line);

} // namespace debuglog
