; ============================================================================
;  RpOverlay — Inno Setup script
;  Сборка установщика:  "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" RpOverlay.iss
;
;  ПЕРЕД СБОРКОЙ:
;   1) Подготовить папку dist\ исправленной командой windeployqt6 + ручные DLL
;      (libssl-3-x64.dll, libcrypto-3-x64.dll, mingw runtime, ICU/pcre2/... — 23 шт.,
;       плагин tls\qopensslbackend.dll через --force-openssl). См. build-dist в README.
;
;  Установка per-user (без прав администратора): PrivilegesRequired=lowest.
;  config.json РЯДОМ с exe НЕ кладётся: деплой-параметры (домен/сервер/бот) зашиты в коде
;  (Config.cpp). Юзеру там нечего настраивать; его настройки — в %APPDATA%\RpOverlay
;  (settings.json, device_id.txt, tokens.dat). [InstallDelete] подчищает старый config.json
;  от прежних версий. (config.json остаётся НЕОБЯЗАТЕЛЬНЫМ override для дева — если положить вручную.)
; ============================================================================

; Версию можно переопределить из командной строки: ISCC.exe /DMyAppVersion=0.1.1 RpOverlay.iss
; (release.sh берёт её из CMake project(VERSION), чтобы exe/manifest/installer совпадали).
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "RpOverlay"
#define MyAppExeName "RpOverlay.exe"
; Вариант сборки: release.sh передаёт /DVariant=qa (публичная Q&A) или /DVariant=laws (билд
; «поиск по законке» с универсальным ключом). Без define — full-вариант (тестерский канал).
#ifndef Variant
  #define Variant "full"
#endif
; laws — ОТДЕЛЬНЫЙ продукт: своё имя приложения (и папка {autopf}\RpOverlay Laws, ярлыки,
; деинсталлятор) — ставится РЯДОМ с основным оверлеем, не перезаписывая его.
#if Variant == "laws"
  #define MyAppName "RpOverlay Laws"
#else
  #define MyAppName "RpOverlay"
#endif
; Папка с разложенным дистрибутивом (windeployqt6 + OpenSSL + mingw runtime).
; ⚠️ СТРОГО относительно .iss (SourcePath): абсолютный путь однажды заставил сборку из
; worktree молча упаковать ЧУЖОЙ dist основного чекаута (full-бинарь под QA-этикеткой).
#define DistDir SourcePath + "dist"

[Setup]
; laws-вариант — СВОЙ AppId: иначе установщики laws и основного оверлея считались бы одним
; приложением и перезаписывали бы друг друга в одной папке.
#if Variant == "laws"
AppId={{0EC5E65A-F5F4-462C-9FAA-E49E4778C9CE}
#else
AppId={{B7B6F2C1-9C2D-4E8A-9F1A-7A2D6E3F4B10}
#endif
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; В свойствах exe-установщика (для антивирусов/SmartScreen) и для сравнения версий.
VersionInfoVersion={#MyAppVersion}
; --- ТОЛЬКО первичная установка ---
; Авто-обновления теперь бесшовные (per-file delta + RpUpdater.exe подменяет файлы на месте,
; БЕЗ Inno). Поэтому директивы авто-апдейта (AppMutex/CloseApplications/RestartApplications) УБРАНЫ:
; этот установщик нужен лишь для ПЕРВОЙ установки (ярлыки, per-user каталог, деинсталлятор).
; --- ВСЕГДА per-user установка без прав администратора ---
; PrivilegesRequiredOverridesAllowed НЕ задаём (пусто = запрет override) — иначе юзер мог выбрать
; «для всех» (admin → Program Files), а тихий авто-апдейт всегда идёт lowest (→ %LocalAppData%\Programs),
; и обновление уезжало в ДРУГУЮ папку: старый ярлык запускал старую версию («апдейт не сработал»).
; Теперь и первая установка, и апдейты — строго per-user в одну папку.
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#SourcePath}installer
; qa → RpOverlay-QA-Setup-<ver>.exe; laws → RpOverlay-Laws-Setup-<ver>.exe;
; иначе (full) → RpOverlay-Setup-<ver>.exe (основной канал).
#if Variant == "qa"
  OutputBaseFilename=RpOverlay-QA-Setup-{#MyAppVersion}
#elif Variant == "laws"
  OutputBaseFilename=RpOverlay-Laws-Setup-{#MyAppVersion}
#else
  OutputBaseFilename=RpOverlay-Setup-{#MyAppVersion}
#endif
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
SetupLogging=yes
; SetupIconFile НЕ задаём намеренно → у самого установщика стандартная иконка Inno.
; А вот установленное приложение и его ярлыки берут иконку из RpOverlay.exe, куда она
; встроена через app.rc (см. CMakeLists), и деинсталлятор — из {app}\RpOverlay.exe выше.

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; --- Весь подготовленный дистрибутив (exe + Qt DLL + плагины tls\,platforms\,...
;     + libssl/libcrypto + mingw runtime + не-Qt DLL), рекурсивно ---
; Excludes *.preprotect — это ЧИСТЫЕ (НЕзащищённые) оригиналы-бэкапы protect-dist.sh; в установщик
; их класть НЕЛЬЗЯ (утечка незащищённого бинаря + лишний вес). Под защищённой сборкой они лежат в dist/.
Source: "{#DistDir}\*"; DestDir: "{app}"; Excludes: "*.preprotect"; Flags: ignoreversion recursesubdirs createallsubdirs
; config.json НЕ кладём — деплой-параметры зашиты в Config.cpp (см. шапку файла).

[InstallDelete]
; Подчистить config.json от прежних версий (раньше он клался установщиком). Деплой-значения
; теперь в коде; файл больше не нужен. Юзерские настройки (settings.json) лежат в %APPDATA% — не трогаем.
Type: files; Name: "{app}\config.json"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; nowait + БЕЗ skipifsilent/postinstall → клиент перезапускается и при тихой авто-установке
; (/VERYSILENT), и после обычной установки. Это и есть «само-перезапуск» после апдейта.
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait

; Деинсталлятор Inno генерирует автоматически. Per-user данные (device_id.txt,
; tokens.dat в %APPDATA%\RpOverlay) при удалении НЕ трогаются намеренно.

[Code]
// --- HWID-гейт инсталлятора (Слой G плана усиления защиты) ---
// До установки спрашиваем сервер, не забанено ли это железо. ДЕТЕРРЕНТ, не стена: упорный распатчит
// инсталлятор/заблокит запрос. РЕАЛЬНЫЙ барьер — серверный лок при первом запуске (verify→hwid_banned).
// Шлём ТОЛЬКО MachineGuid (реестр, надёжно, без WMI/COM) — сервер сверяет по пересечению компонентов.
// Целостность ответа — по TLS (overlay.turgovbot.ru). Любая сетевая ошибка → fail-open (не кирпичим легит).
const
  HWID_STATUS_URL = 'https://overlay.turgovbot.ru/v1/license/hwid-status';

function JsonEsc(const S: String): String;
begin
  Result := S;
  StringChangeEx(Result, '\', '\\', True);
  StringChangeEx(Result, '"', '\"', True);
end;

function RegMachineGuid(): String;
var v: String;
begin
  Result := '';
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Microsoft\Cryptography', 'MachineGuid', v) then
    Result := v;
end;

// Возврат: 1 = забанено, 0 = не забанено, -1 = неизвестно/ошибка (→ fail-open).
function QueryBanned(const Json: String): Integer;
var
  WinHttp: Variant;
  Resp: String;
begin
  Result := -1;
  try
    WinHttp := CreateOleObject('WinHttp.WinHttpRequest.5.1');
    WinHttp.Open('POST', HWID_STATUS_URL, False);
    WinHttp.SetRequestHeader('Content-Type', 'application/json');
    WinHttp.SetTimeouts(4000, 4000, 4000, 6000);
    WinHttp.Send(Json);
    if WinHttp.Status = 200 then
    begin
      Resp := WinHttp.ResponseText;   // Variant→String присваиванием (LowerCase не ест Variant напрямую)
      Resp := LowerCase(Resp);
      StringChangeEx(Resp, ' ', '', True);
      if Pos('"banned":true', Resp) > 0 then
        Result := 1
      else
        Result := 0;
    end;
  except
    Result := -1;  // нет сети / Cloudflare лёг / DPI-блок → fail-open
  end;
end;

function InitializeSetup(): Boolean;
var
  mg, json: String;
begin
  Result := True;  // по умолчанию пускаем (fail-open)
  try
    mg := RegMachineGuid();
    if mg <> '' then
    begin
      json := '{"machine_guid":"' + JsonEsc(mg) + '"}';
      if QueryBanned(json) = 1 then
      begin
        MsgBox('Это устройство заблокировано и не может установить RP Overlay.' + #13#10 +
               'Если считаешь это ошибкой — обратись к администратору.', mbCriticalError, MB_OK);
        Result := False;
      end;
    end;
  except
    Result := True;  // любая ошибка кода гейта НЕ должна мешать легиту
  end;
end;
