#include "DebugLog.h"

#include <QtGlobal>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QLoggingCategory>
#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <cstdio>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace {

bool             g_enabled = false;
QString          g_logPath;      // путь к лог-файлу (открываем на КАЖДУЮ запись и закрываем —
bool             g_fileOk  = false; //   файл не залочен, тестер может скопировать/прислать его в любой момент)
QtMessageHandler g_prev    = nullptr;
QMutex           g_mutex;        // логи приходят из разных потоков (аудио/сеть) — сериализуем запись
#ifdef Q_OS_WIN
bool             g_console  = false;
#endif

const char *levelStr(QtMsgType t)
{
    switch (t) {
        case QtDebugMsg:    return "DBG";
        case QtInfoMsg:     return "INF";
        case QtWarningMsg:  return "WRN";
        case QtCriticalMsg: return "ERR";
        case QtFatalMsg:    return "FATAL";
    }
    return "?";
}

void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    const QString ts  = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString cat = ctx.category ? QString::fromLatin1(ctx.category) : QStringLiteral("app");
    QString line = QStringLiteral("[%1] %2 %3 | %4").arg(ts, QString::fromLatin1(levelStr(type)), cat, msg);

    const QByteArray b = (line + QLatin1Char('\n')).toUtf8();
    QMutexLocker lock(&g_mutex);
    // Консоль (stderr перенаправлен на CONOUT$ в setEnabled).
    fputs(b.constData(), stderr);
    fflush(stderr);
    // Файл: открываем/дописываем/закрываем на каждую строку → файл НЕ залочен (тестер
    // может скопировать его в любой момент, не закрывая оверлей).
    if (g_fileOk) {
        QFile f(g_logPath);
        if (f.open(QIODevice::Append | QIODevice::Text))
            f.write(b);   // закроется в деструкторе QFile
    }
    if (type == QtFatalMsg)
        abort();
}

} // namespace

namespace debuglog {

QString logDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/RpOverlay");
    return base + QStringLiteral("/logs");
}

QString logFilePath() { return logDir() + QStringLiteral("/overlay-debug.log"); }

bool isEnabled() { return g_enabled; }

void setEnabled(bool on)
{
    if (on == g_enabled)
        return;

    if (on) {
#ifdef Q_OS_WIN
        // Своё консольное окно (GUI-приложение без консоли по умолчанию).
        if (AllocConsole()) {
            g_console = true;
            FILE *f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleTitleW(L"RP Overlay — отладочный лог (НЕ закрывайте это окно)");
            // Запрещаем закрытие консоли крестиком — иначе тестер закрыл бы и УБИЛ оверлей.
            if (HWND cw = GetConsoleWindow()) {
                if (HMENU m = GetSystemMenu(cw, FALSE))
                    DeleteMenu(m, SC_CLOSE, MF_BYCOMMAND);
            }
        }
#endif
        // Лог-файл (свежий на сессию): один раз очищаем, дальше дописываем построчно.
        QDir().mkpath(logDir());
        g_logPath = logFilePath();
        {
            QFile f(g_logPath);
            g_fileOk = f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        }   // закрыли — не держим хэндл
        // Подробно по логам ПРИЛОЖЕНИЯ (default-категория), но глушим внутренний спам Qt
        // (qt.qpa.fonts/qt.text.font.db и пр. — тысячи строк про шрифты). Qt-warning остаются.
        QLoggingCategory::setFilterRules(QStringLiteral(
            "*.debug=true\n"
            "*.info=true\n"
            "qt.*.debug=false\n"
            "qt.*.info=false"));
        g_prev = qInstallMessageHandler(handler);
        g_enabled = true;

        qInfo().noquote() << "================ RP Overlay — отладочный лог ================";
        qInfo().noquote() << "Версия:" << QCoreApplication::applicationVersion();
        qInfo().noquote() << "Время:" << QDateTime::currentDateTime().toString(Qt::ISODate);
        qInfo().noquote() << "Файл лога:" << logFilePath();
        qInfo().noquote() << "Пришлите этот файл разработчику, если что-то не работает.";
        qInfo().noquote() << "============================================================";
    } else {
        qInfo().noquote() << "[debug] режим отладки выключен";
        qInstallMessageHandler(g_prev);
        g_prev = nullptr;
        QLoggingCategory::setFilterRules(QString());
        {
            QMutexLocker lock(&g_mutex);
            g_fileOk = false;
        }
#ifdef Q_OS_WIN
        if (g_console) {
            FreeConsole();
            g_console = false;
        }
#endif
        g_enabled = false;
    }
}

void openLogDir()
{
    QDir().mkpath(logDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(logDir()));
}

void appendAlways(const QString &line)
{
    // Намеренно НЕ через qInfo(): тот пишет в файл только при включённом режиме отладки.
    // Здесь открываем файл сами — диагностика по кнопке обязана попадать в файл всегда.
    // Мьютекс тот же, что у обработчика сообщений: записи из разных потоков не перемешаются.
    QDir().mkpath(logDir());
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QByteArray b = (QStringLiteral("[%1] INF dump | %2").arg(ts, line)
                          + QLatin1Char('\n')).toUtf8();
    QMutexLocker lock(&g_mutex);
    QFile f(logFilePath());
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write(b);   // закроется в деструкторе QFile — файл не залочен
}

} // namespace debuglog
