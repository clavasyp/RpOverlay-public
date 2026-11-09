// RpUpdater.exe — крошечный хелпер бесшовного автообновления. БЕЗ Qt и любых не-системных DLL
// (иначе держал бы их открытыми и не смог бы подменить при апдейте). Запуск из приложения:
//   RpUpdater.exe --apply <путь к apply.txt>
// apply.txt (UTF-8, key=value): installDir, stagedDir, mainExe, mainPid, relaunch.
// Алгоритм: ждём выхода RpOverlay → атомарная подмена staged-файлов (rename target→.bak,
// затем staged→target; rename переживает локи лучше delete+write) → при сбое откат всех .bak →
// перезапуск приложения → чистка. Никогда не оставляет полу-обновлённую установку.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <cstdlib>
#include <cwctype>

static std::wstring utf8ToW(const std::string &s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string readFileBytes(const std::wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string out;
    char buf[4096];
    DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd)
        out.append(buf, rd);
    CloseHandle(h);
    return out;
}

static std::map<std::wstring, std::wstring> parseKv(const std::string &u8)
{
    std::map<std::wstring, std::wstring> m;
    size_t i = 0;
    while (i < u8.size()) {
        size_t nl = u8.find('\n', i);
        std::string line = u8.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? u8.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos)
            m[utf8ToW(line.substr(0, eq))] = utf8ToW(line.substr(eq + 1));
    }
    return m;
}

// Рекурсивно собрать относительные пути всех файлов под root.
static void collect(const std::wstring &root, const std::wstring &rel, std::vector<std::wstring> &out)
{
    std::wstring pattern = root + (rel.empty() ? L"" : L"\\" + rel) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring childRel = rel.empty() ? name : rel + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            collect(root, childRel, out);
        else
            out.push_back(childRel);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::wstring dirOf(const std::wstring &p)
{
    size_t s = p.find_last_of(L'\\');
    return (s == std::wstring::npos) ? L"" : p.substr(0, s);
}

static void ensureDir(const std::wstring &path)
{
    for (size_t i = 3; i < path.size(); ++i)
        if (path[i] == L'\\')
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

static bool moveRetry(const std::wstring &from, const std::wstring &to, int tries, int delayMs)
{
    // AV (особенно облачный) держит хэндл на только что записанном Themida-НЕподписанном exe без
    // FILE_SHARE_DELETE десятки секунд → MoveFileEx временно падает. Ретраим. Длинный бюджет (60×1000мс)
    // применяем ТОЛЬКО к ожиданию отпускания staged-источника (target при этом ещё цел); сам своп —
    // короткими ретраями, чтобы окно без установленного exe было миллисекундами (инцидент 2026-07-19).
    for (int i = 0; i < tries; ++i) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
            return true;
        Sleep(delayMs);
    }
    return false;
}

static void rmTree(const std::wstring &dir)
{
    // Defense-in-depth: рекурсивное тихое удаление (FOF_NOCONFIRMATION|FOF_SILENT) — жёсткий backstop
    // НЕЗАВИСИМО от вызывающего. Сносим ТОЛЬКО app-owned папки (лист = RpOverlay / RP Overlay).
    if (dir.size() < 8) return;                            // слишком коротко (корень диска и т.п.)
    if (dir.find(L"..") != std::wstring::npos) return;     // traversal
    if (dir.rfind(L"\\\\", 0) == 0) return;                // UNC/сеть
    if (dir.find(L':') == std::wstring::npos) return;      // не абсолютный
    int seps = 0;
    for (wchar_t c : dir) if (c == L'\\') ++seps;
    if (seps < 2) return;                                  // не корень/не верхний уровень
    {
        std::wstring leaf = dir;
        while (!leaf.empty() && leaf.back() == L'\\') leaf.pop_back();
        const size_t s = leaf.find_last_of(L'\\');
        std::wstring base = (s == std::wstring::npos) ? leaf : leaf.substr(s + 1);
        std::wstring lb;
        for (wchar_t c : base) lb.push_back((wchar_t)towlower(c));
        if (lb != L"rpoverlay" && lb != L"rp overlay") return;  // не наша папка → отказ
    }
    std::wstring from = dir;
    from.push_back(L'\0');
    from.push_back(L'\0');  // двойной NUL для SHFileOperation
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

// ====================== Кил-свитч: режим --reap (Слой F) ======================
// Чистит ТОЛЬКО свой след по ЛОКАЛЬНО вычисленному allowlist'у. Путь по сети/из команды НЕ принимает
// (кроме installDir, который строго валидируется). reap.txt несёт лишь mainPid + magic + installDir.
static std::wstring getEnvW(const wchar_t *name)
{
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return (n > 0 && n < 32768) ? std::wstring(buf, n) : std::wstring();
}

static std::wstring lowerW(std::wstring s)
{
    for (auto &c : s) c = (wchar_t)towlower(c);
    return s;
}

// Валидируем install dir СТРОГИМ равенством канонической per-user локации (инсталлятор всегда lowest:
// {autopf}=%LOCALAPPDATA%\Programs). НЕ подстрочный матч (исключает обход вида ...\Program Files Backup\)
// и НЕ берём путь из команды — вычисляем из env (см. runReap). Плюс существование + наличие RpOverlay.exe.
static bool isAllowedInstallDir(const std::wstring &dir)
{
    if (dir.empty())
        return false;
    const std::wstring localapp = getEnvW(L"LOCALAPPDATA");
    if (localapp.empty())
        return false;
    const std::wstring canon = localapp + L"\\Programs\\RpOverlay";
    if (lowerW(dir) != lowerW(canon))                       // строго каноничная локация, без подстрок
        return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    if (GetFileAttributesW((dir + L"\\RpOverlay.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    return true;
}

static int runReap(const std::wstring &reapPath)
{
    auto kv = parseKv(readFileBytes(reapPath));
    if (kv[L"reap"] != L"RPOVERLAY_REAP_V1")   // magic — не реагируем на чужой/случайный --reap
        return 1;
    const DWORD mainPid = (DWORD)wcstoul(kv[L"mainPid"].c_str(), nullptr, 10);

    // 1) Ждём реального выхода главного процесса.
    if (mainPid) {
        HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, mainPid);
        if (hp) { WaitForSingleObject(hp, 30000); CloseHandle(hp); }
    }
    Sleep(900);

    // 2) Allowlist — ВСЕ пути вычисляем ЛОКАЛЬНО из env (НИЧЕГО из reap.txt): org=ProdBot / app=RP Overlay,
    //    install = каноническая per-user локация. Так подделанный reap.txt не может задать чужой путь.
    std::vector<std::wstring> targets;
    const std::wstring appdata  = getEnvW(L"APPDATA");        // %APPDATA%  (роуминг)
    const std::wstring localapp = getEnvW(L"LOCALAPPDATA");   // %LOCALAPPDATA% (локал)
    const std::wstring userprof = getEnvW(L"USERPROFILE");
    if (!appdata.empty())  targets.push_back(appdata  + L"\\ProdBot\\RP Overlay");   // settings/tokens/hwid_key/device_id/log
    if (!localapp.empty()) targets.push_back(localapp + L"\\ProdBot\\RP Overlay");   // модели, стейдж апдейта
    // install dir — вычислен из env (НЕ из команды), валидируется строгим равенством канонической локации.
    const std::wstring installDir = localapp.empty() ? std::wstring() : (localapp + L"\\Programs\\RpOverlay");
    if (isAllowedInstallDir(installDir)) targets.push_back(installDir);
    if (!appdata.empty())
        targets.push_back(appdata + L"\\Microsoft\\Windows\\Start Menu\\Programs\\RpOverlay"); // ярлык меню

    // 3) Удаляем (best-effort). Точечно: только \RP Overlay внутри \ProdBot, не весь org-каталог.
    for (const std::wstring &t : targets)
        rmTree(t);
    if (!userprof.empty())
        DeleteFileW((userprof + L"\\Desktop\\RpOverlay.lnk").c_str());              // ярлык рабочего стола

    // 4) Self-delete: рипер запущен из %TEMP% (копия), оригинал в installDir уже снесён. Свою копию
    //    помечаем на удаление при перезагрузке + сносим reap.txt.
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH))
        MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    DeleteFileW(reapPath.c_str());
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring applyPath, reapPath;
    for (int i = 0; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--apply" && i + 1 < argc)
            applyPath = argv[i + 1];
        else if (std::wstring(argv[i]) == L"--reap" && i + 1 < argc)
            reapPath = argv[i + 1];
    }
    if (argv) LocalFree(argv);
    if (!reapPath.empty())
        return runReap(reapPath);
    if (applyPath.empty())
        return 1;

    auto kv = parseKv(readFileBytes(applyPath));
    const std::wstring installDir = kv[L"installDir"];
    const std::wstring stagedDir  = kv[L"stagedDir"];
    const std::wstring mainExe    = kv[L"mainExe"];
    const DWORD mainPid = (DWORD)wcstoul(kv[L"mainPid"].c_str(), nullptr, 10);
    const bool relaunch = (kv[L"relaunch"] == L"1");
    if (installDir.empty() || stagedDir.empty())
        return 1;

    // 1) Ждём реального выхода главного процесса (мьютекс отпускается раньше handles DLL).
    if (mainPid) {
        HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, mainPid);
        if (hp) { WaitForSingleObject(hp, 30000); CloseHandle(hp); }
    }
    Sleep(700);  // дать загрузчику отпустить DLL

    // 2) Подмена: rename target→.bak, затем staged→target; откат при сбое.
    std::vector<std::wstring> rels;
    collect(stagedDir, L"", rels);

    std::vector<std::pair<std::wstring, std::wstring>> backups;  // (bak, orig)
    bool ok = true;
    for (const std::wstring &rel : rels) {
        const std::wstring target = installDir + L"\\" + rel;
        const std::wstring src    = stagedDir + L"\\" + rel;
        // Осиротевший .rdy от прошлого СОРВАННОГО свопа: это НЕ файл релиза (collect() подобрал его,
        // т.к. резюм-логика больше не чистит stage). Его исходника нет → moveRetry молотил бы 60с
        // впустую и откатил бы уже успешно применённые файлы. Сносим и пропускаем.
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, L".rdy") == 0) {
            DeleteFileW(src.c_str());
            continue;
        }
        ensureDir(dirOf(target));
        const std::wstring bak   = target + L".bak";
        const std::wstring ready = src + L".rdy";

        // ⚠️ ПОРЯДОК КРИТИЧЕН (инцидент 2026-07-19): сначала ждём, пока AV ОТПУСТИТ свежескачанный
        // staged-источник — ДО того как трогаем target. Пробуем перевести src → src.rdy (rename
        // проходит только когда файл не залочен): длинный 60с-бюджет на AV-скан. Пока ждём —
        // installDir\target ЦЕЛ (нет окна «нет exe» → выключение ПК/смерть апдейтера тут безопасны).
        DeleteFileW(ready.c_str());
        if (!moveRetry(src, ready, 60, 1000)) { ok = false; break; }

        // Источник releasable → своп парой БЫСТРЫХ rename'ов (target давно отсканирован AV; ready
        // только что успешно переименован). Окно, когда target отсутствует, — миллисекунды, не 60с.
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(bak.c_str());
            if (!moveRetry(target, bak, 12, 250)) { ok = false; break; }
            backups.push_back({bak, target});
        }
        if (!moveRetry(ready, target, 12, 250)) { ok = false; break; }
    }

    if (!ok) {
        for (auto it = backups.rbegin(); it != backups.rend(); ++it)
            MoveFileExW(it->first.c_str(), it->second.c_str(), MOVEFILE_REPLACE_EXISTING);  // откат
    } else {
        for (auto &b : backups)
            DeleteFileW(b.first.c_str());
    }

    // 3) Чистка staged/state/apply (best-effort).
    const std::wstring stageRoot = dirOf(dirOf(stagedDir));  // .../update
    rmTree(stagedDir);
    DeleteFileW((stageRoot + L"\\state.json").c_str());
    DeleteFileW(applyPath.c_str());

    // 4) Перезапуск приложения (даже при откате — поднимаем старую/восстановленную версию).
    if (relaunch && !mainExe.empty())
        ShellExecuteW(nullptr, L"open", mainExe.c_str(), nullptr, installDir.c_str(), SW_SHOWNORMAL);

    return ok ? 0 : 2;
}
