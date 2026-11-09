#pragma once

// Утилиты для гибкого биндинга горячих клавиш: токен <-> (модификаторы, Win32 VK).
// Поддерживает одиночные клавиши, numpad, именованные клавиши и КНОПКИ МЫШИ
// (у боковых кнопок мыши есть свои VK-коды, GetAsyncKeyState их видит).
//
// Формат токена (строка в settings.json, человекочитаемая, обратносовместимая со старыми
// «Ctrl+Shift+O»): [Ctrl+][Shift+][Alt+][Win+]<KEY>.
//   KEY: A..Z, 0..9, F1..F24, Space, Tab, Esc, Enter, Backspace, Insert, Delete,
//        Home, End, PageUp, PageDown, Left, Right, Up, Down, CapsLock, PrintScreen, Pause, Apps,
//        Num0..Num9, NumMul, NumAdd, NumSub, NumDec, NumDiv,
//        Mouse3 (средняя), Mouse4 (X1, «назад»), Mouse5 (X2, «вперёд»).
//
// Биты модификаторов (НЕ Win32 MOD_*, а наши): Ctrl=1, Shift=2, Alt=4, Win=8.

#include <QString>
#include <QStringList>
#include <QChar>

namespace hk {

enum { Ctrl = 1, Shift = 2, Alt = 4, Win = 8 };

// токен ключа -> Win32 VK (0 = неизвестно). Без модификаторов.
inline unsigned tokenToVk(const QString &tokRaw)
{
    const QString t = tokRaw.trimmed();
    if (t.isEmpty()) return 0;
    const QString u = t.toUpper();

    if (u.size() == 1) {
        const ushort c = u.at(0).unicode();
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; // VK == ASCII заглавной
    }
    if (u.size() >= 2 && u.at(0) == QLatin1Char('F')) {           // F1..F24
        bool ok = false; const int n = u.mid(1).toInt(&ok);
        if (ok && n >= 1 && n <= 24) return 0x70 + (n - 1);
    }
    if (u.startsWith(QLatin1String("NUM"))) {                     // numpad
        const QString r = u.mid(3);
        if (r.size() == 1 && r.at(0).isDigit()) return 0x60 + (r.at(0).unicode() - '0');
        if (r == QLatin1String("MUL")) return 0x6A;
        if (r == QLatin1String("ADD")) return 0x6B;
        if (r == QLatin1String("SUB")) return 0x6D;
        if (r == QLatin1String("DEC")) return 0x6E;
        if (r == QLatin1String("DIV")) return 0x6F;
    }
    if (u == QLatin1String("MOUSE3")) return 0x04; // VK_MBUTTON
    if (u == QLatin1String("MOUSE4")) return 0x05; // VK_XBUTTON1
    if (u == QLatin1String("MOUSE5")) return 0x06; // VK_XBUTTON2

    if (u == QLatin1String("SPACE"))      return 0x20;
    if (u == QLatin1String("TAB"))        return 0x09;
    if (u == QLatin1String("ESC") || u == QLatin1String("ESCAPE")) return 0x1B;
    if (u == QLatin1String("ENTER") || u == QLatin1String("RETURN")) return 0x0D;
    if (u == QLatin1String("BACKSPACE"))  return 0x08;
    if (u == QLatin1String("INSERT") || u == QLatin1String("INS")) return 0x2D;
    if (u == QLatin1String("DELETE") || u == QLatin1String("DEL")) return 0x2E;
    if (u == QLatin1String("HOME"))       return 0x24;
    if (u == QLatin1String("END"))        return 0x23;
    if (u == QLatin1String("PAGEUP") || u == QLatin1String("PGUP")) return 0x21;
    if (u == QLatin1String("PAGEDOWN") || u == QLatin1String("PGDN")) return 0x22;
    if (u == QLatin1String("LEFT"))       return 0x25;
    if (u == QLatin1String("UP"))         return 0x26;
    if (u == QLatin1String("RIGHT"))      return 0x27;
    if (u == QLatin1String("DOWN"))       return 0x28;
    if (u == QLatin1String("CAPSLOCK"))   return 0x14;
    if (u == QLatin1String("PRINTSCREEN") || u == QLatin1String("PRTSC")) return 0x2C;
    if (u == QLatin1String("PAUSE"))      return 0x13;
    if (u == QLatin1String("APPS") || u == QLatin1String("MENU")) return 0x5D;
    return 0;
}

// Win32 VK -> человекочитаемый токен ("" если неизвестно).
inline QString vkToToken(unsigned vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return QString(QChar(vk));
    if (vk >= 0x70 && vk <= 0x87) return QStringLiteral("F%1").arg(vk - 0x70 + 1);
    if (vk >= 0x60 && vk <= 0x69) return QStringLiteral("Num%1").arg(vk - 0x60);
    switch (vk) {
        case 0x6A: return QStringLiteral("NumMul");
        case 0x6B: return QStringLiteral("NumAdd");
        case 0x6D: return QStringLiteral("NumSub");
        case 0x6E: return QStringLiteral("NumDec");
        case 0x6F: return QStringLiteral("NumDiv");
        case 0x04: return QStringLiteral("Mouse3");
        case 0x05: return QStringLiteral("Mouse4");
        case 0x06: return QStringLiteral("Mouse5");
        case 0x20: return QStringLiteral("Space");
        case 0x09: return QStringLiteral("Tab");
        case 0x1B: return QStringLiteral("Esc");
        case 0x0D: return QStringLiteral("Enter");
        case 0x08: return QStringLiteral("Backspace");
        case 0x2D: return QStringLiteral("Insert");
        case 0x2E: return QStringLiteral("Delete");
        case 0x24: return QStringLiteral("Home");
        case 0x23: return QStringLiteral("End");
        case 0x21: return QStringLiteral("PageUp");
        case 0x22: return QStringLiteral("PageDown");
        case 0x25: return QStringLiteral("Left");
        case 0x26: return QStringLiteral("Up");
        case 0x27: return QStringLiteral("Right");
        case 0x28: return QStringLiteral("Down");
        case 0x14: return QStringLiteral("CapsLock");
        case 0x2C: return QStringLiteral("PrintScreen");
        case 0x13: return QStringLiteral("Pause");
        case 0x5D: return QStringLiteral("Apps");
        default: return QString();
    }
}

// токен-строка -> (mods, vk). true если распознан key.
inline bool parse(const QString &s, unsigned &mods, unsigned &vk)
{
    mods = 0; vk = 0;
    const QStringList parts = s.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString u = raw.trimmed().toUpper();
        if (u == QLatin1String("CTRL") || u == QLatin1String("CONTROL")) mods |= Ctrl;
        else if (u == QLatin1String("SHIFT")) mods |= Shift;
        else if (u == QLatin1String("ALT")) mods |= Alt;
        else if (u == QLatin1String("WIN") || u == QLatin1String("META")) mods |= Win;
        else {
            const unsigned v = tokenToVk(raw.trimmed());
            if (!v) return false;
            vk = v;
        }
    }
    return vk != 0;
}

// (mods, vk) -> токен-строка для хранения/отображения.
inline QString format(unsigned mods, unsigned vk)
{
    QString s;
    if (mods & Ctrl)  s += QStringLiteral("Ctrl+");
    if (mods & Shift) s += QStringLiteral("Shift+");
    if (mods & Alt)   s += QStringLiteral("Alt+");
    if (mods & Win)   s += QStringLiteral("Win+");
    s += vkToToken(vk);
    return s;
}

} // namespace hk
