// CrashHandler.cpp — самодиагностика крашей релизного клиента.
//
// Цель: при падении писать ПРИЧИНУ прямо в наш лог (сбойный модуль+смещение, код исключения,
// стек) + минидамп — чтобы не просить юзера лезть в «Журнал событий Windows».
//
// Тулчейн — MinGW/ucrt64 (GCC): __try/__except НЕ доступны, поэтому используем чистый Win32
// SetUnhandledExceptionFilter (это обычная функция-фильтр, GCC её поддерживает) + dbghelp
// (StackWalk64/MiniDumpWriteDump, линкуется -ldbghelp). Никакого Qt — на момент краша он мог
// быть в нерабочем состоянии; путь к logs/ берём из %APPDATA% и кэшируем при install().

#include "CrashHandler.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>

namespace {

wchar_t       g_logDir[MAX_PATH] = L"";    // %APPDATA%\ProdBot\RP Overlay\logs
char          g_appVer[64]       = "?";
volatile LONG g_inHandler        = 0;      // анти-рекурсия

// Точная версия Windows (GetVersionEx искажается манифестом совместимости).
void osVersionStr(char *out, size_t n)
{
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi;
    ZeroMemory(&vi, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (HMODULE h = GetModuleHandleW(L"ntdll.dll"))
        if (auto p = (RtlGetVersionPtr)GetProcAddress(h, "RtlGetVersion"))
            p(&vi);
    snprintf(out, n, "Windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
}

const char *excName(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:    return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:      return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:    return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:       return "IN_PAGE_ERROR";
        case 0xE06D7363:                    return "C++ exception";
        default:                            return "UNKNOWN";
    }
}

// «module.dll+0x1234» по абсолютному адресу.
void moduleOffset(const void *addr, char *out, size_t n)
{
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod) {
        wchar_t pathW[MAX_PATH] = L"";
        GetModuleFileNameW(mod, pathW, MAX_PATH);
        const wchar_t *base = wcsrchr(pathW, L'\\');
        base = base ? base + 1 : pathW;
        unsigned long long off = (unsigned long long)((const char *)addr - (const char *)mod);
        snprintf(out, n, "%ls+0x%llx", base, off);
    } else {
        snprintf(out, n, "0x%p", addr);
    }
}

void writeLine(HANDLE f, const char *s)
{
    if (f == INVALID_HANDLE_VALUE || !s) return;
    DWORD wr = 0;
    WriteFile(f, s, (DWORD)strlen(s), &wr, nullptr);
}

void writeMiniDump(EXCEPTION_POINTERS *ep, const wchar_t *path)
{
    HANDLE hf = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    ZeroMemory(&mei, sizeof(mei));
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                                               MiniDumpWithUnloadedModules |
                                               MiniDumpWithIndirectlyReferencedMemory);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hf, type, &mei, nullptr, nullptr);
    CloseHandle(hf);
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS *ep)
{
    if (InterlockedExchange(&g_inHandler, 1))
        return EXCEPTION_EXECUTE_HANDLER;   // повторный краш внутри обработчика — просто выходим

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t logPath[MAX_PATH], dmpPath[MAX_PATH];
    swprintf(logPath, MAX_PATH, L"%ls\\overlay-crash.log", g_logDir);
    swprintf(dmpPath, MAX_PATH, L"%ls\\overlay-crash-%04u%02u%02u-%02u%02u%02u.dmp",
             g_logDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE f = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    char buf[1200], osb[160], modoff[MAX_PATH + 48];
    osVersionStr(osb, sizeof osb);
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    moduleOffset(er->ExceptionAddress, modoff, sizeof modoff);

    snprintf(buf, sizeof buf,
             "\r\n======== RP Overlay CRASH ========\r\n"
             "time=%04u-%02u-%02u %02u:%02u:%02u\r\nversion=%s\r\nos=%s\r\n"
             "exception=0x%08lx (%s)\r\nfault_module=%s\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             g_appVer, osb, (unsigned long)er->ExceptionCode, excName(er->ExceptionCode), modoff);
    writeLine(f, buf);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        snprintf(buf, sizeof buf, "access=%s addr=0x%llx\r\n",
                 er->ExceptionInformation[0] ? "write" : "read",
                 (unsigned long long)er->ExceptionInformation[1]);
        writeLine(f, buf);
    }

    // Стек по контексту краша (best-effort; символы Qt/exe stripped → выводим module+offset).
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf;
    ZeroMemory(&sf, sizeof(sf));
    DWORD mach;
#if defined(_M_X64) || defined(__x86_64__)
    mach                = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset    = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
#else
    mach                = IMAGE_FILE_MACHINE_I386;
    sf.AddrPC.Offset    = ctx.Eip;
    sf.AddrFrame.Offset = ctx.Ebp;
    sf.AddrStack.Offset = ctx.Esp;
#endif
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;

    writeLine(f, "stack:\r\n");
    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(mach, proc, GetCurrentThread(), &sf, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0)
            break;
        char mo[MAX_PATH + 48];
        moduleOffset((const void *)(uintptr_t)sf.AddrPC.Offset, mo, sizeof mo);
        char symbuf[sizeof(SYMBOL_INFO) + 256];
        ZeroMemory(symbuf, sizeof symbuf);
        auto *si = (SYMBOL_INFO *)symbuf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen   = 255;
        DWORD64 disp     = 0;
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, si))
            snprintf(buf, sizeof buf, "  #%02d %s  (%s+0x%llx)\r\n", i, mo, si->Name, (unsigned long long)disp);
        else
            snprintf(buf, sizeof buf, "  #%02d %s\r\n", i, mo);
        writeLine(f, buf);
    }
    writeLine(f, "==================================\r\n");

    if (f != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(f);
        CloseHandle(f);
    }

    writeMiniDump(ep, dmpPath);
    return EXCEPTION_EXECUTE_HANDLER;   // отчёт записан → завершаем процесс
}

}  // namespace

namespace crashhandler {

void install(const char *appVersion)
{
    if (appVersion) {
        strncpy(g_appVer, appVersion, sizeof(g_appVer) - 1);
        g_appVer[sizeof(g_appVer) - 1] = '\0';
    }

    // Каталог logs из %APPDATA% — без Qt (он может быть мёртв на момент краша). Создаём дерево.
    wchar_t appdata[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        wchar_t tmp[MAX_PATH];
        swprintf(tmp, MAX_PATH, L"%ls\\ProdBot", appdata);
        CreateDirectoryW(tmp, nullptr);
        swprintf(tmp, MAX_PATH, L"%ls\\ProdBot\\RP Overlay", appdata);
        CreateDirectoryW(tmp, nullptr);
        swprintf(g_logDir, MAX_PATH, L"%ls\\ProdBot\\RP Overlay\\logs", appdata);
        CreateDirectoryW(g_logDir, nullptr);
    }

    SetErrorMode(SEM_FAILCRITICALERRORS);          // не блокировать на критичных диалогах
    SetUnhandledExceptionFilter(crashFilter);      // обычный Win32-фильтр (работает и на MinGW)
}

}  // namespace crashhandler

#else  // !_WIN32
namespace crashhandler { void install(const char *) {} }
#endif
