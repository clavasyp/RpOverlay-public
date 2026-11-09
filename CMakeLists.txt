cmake_minimum_required(VERSION 3.21)
project(RpOverlay VERSION 0.5.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# QA_ONLY — УПРАЗДНЁН с 0.3.0: единственная сборка = компактный Q&A-бар (тестерский канал
# manifest-v2). Публичный Q&A-канал (manifest-v2-qa) ЗАМОРОЖЕН на 0.2.20 — для экстренного
# хотфикса того варианта собирайте из тега v0.2.20. Option оставлен защёлкой, чтобы старые
# кэши build-qa/ (QA_ONLY=ON) падали громко, а не собирали молча не тот бинарь.
option(QA_ONLY "FROZEN: QA-вариант заморожен на 0.2.20 (см. тег v0.2.20)" OFF)
if (QA_ONLY)
    message(FATAL_ERROR "QA_ONLY-вариант заморожен: с 0.3.0 один билд (Q&A-бар). Хотфикс QA-канала — из тега v0.2.20.")
endif()

# RPOVERLAY_QA — публичный Q&A-вариант 0.3.x (НЕ старый QA_ONLY): тот же единый билд, но
# РАНТАЙМ-ЗАМОК на режим Q&A (пилюля режимов скрыта, роли/сцена/«Обзвон» недоступны) + клиент
# шлёт variant=qa (сервер пускает по обычному ключу — подписчики+, без тестер-гейта). Для QA-канала
# (manifest-v2-qa). Дефолт OFF = тестерский full-билд со всеми режимами.
option(RPOVERLAY_QA "Публичный Q&A-вариант: замок на Q&A-режим + variant=qa (QA-канал)" OFF)
if (RPOVERLAY_QA)
    message(STATUS "RpOverlay: ПУБЛИЧНЫЙ Q&A-вариант (замок на Q&A, variant=qa) — релиз на QA-канал")
endif()

# RPOVERLAY_LAWS — отдельный laws-билд «только поиск по законке» (раздаётся с УНИВЕРСАЛЬНЫМ
# ключом без HWID-привязки; отзыв ключа гасит все копии). Надкласс QA-замка: define включает
# И RPOVERLAY_QA (наследует все QA-вырезания: роли/сцена/невидимка скрыты), поверх этого:
# единственный режим бара = «Поиск», хаб только Главная/Поиск/Настройки, variant=laws
# (сервер: тир laws → только /laws/*), свой канал manifest-v2-laws.json, отдельные
# мьютекс/IPC/папка настроек/AppId — НЕ конфликтует с основным оверлеем на той же машине.
option(RPOVERLAY_LAWS "Laws-билд: только поиск по законке + универсальный ключ (variant=laws)" OFF)
if (RPOVERLAY_LAWS)
    message(STATUS "RpOverlay: LAWS-БИЛД (только поиск по законке, variant=laws, канал manifest-v2-laws)")
endif()

# Qt6 (MinGW-сборка ставится через aqtinstall в C:\Qt; путь задаётся через CMAKE_PREFIX_PATH,
# напр. -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64). Модули: Network (https/SSE), WebSockets (wss).
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)

# OpenSSL (libcrypto) — проверка ed25519-подписи установщика в авто-апдейте (UpdateClient).
# libcrypto-3-x64.dll уже кладётся в dist/ (см. build-dist.sh). Заголовки/импорт-либа — из
# того же ucrt64-префикса, что и Qt6 (пакет mingw-w64-ucrt-x86_64-openssl). sha256 файла
# считается средствами Qt (QCryptographicHash), поэтому из OpenSSL нужен только Crypto.
find_package(OpenSSL REQUIRED)

add_executable(RpOverlay WIN32
    src/main.cpp
    src/Config.cpp
    src/Config.h
    src/Obfusc.h
    src/Settings.cpp
    src/Settings.h
    # «Режим невидимка» (тестер-билд): централизованное скрытие всех окон из захвата экрана/OBS.
    src/StealthGuard.cpp
    src/StealthGuard.h
    # Страховка перерисовки слоёных окон: закрытая выпадашка/меню оставляли прозрачную дыру.
    src/LayerRepaint.cpp
    src/LayerRepaint.h
    src/OverlayWindow.cpp
    src/OverlayWindow.h
    # Окно-хаб (лаунчер, 0.4.0) — главное окно приложения; единый таргет, поэтому хаб
    # автоматически входит и в публичный Q&A-вариант (RPOVERLAY_QA — только define).
    src/HubWindow.cpp
    src/HubWindow.h
    src/AuthClient.cpp
    src/AuthClient.h
    src/Dpapi.cpp
    src/Dpapi.h
    src/Hwid.cpp
    src/Hwid.h
    src/LicenseClient.cpp
    src/LicenseClient.h
    src/TelemetryClient.cpp
    src/TelemetryClient.h
    src/KillSwitch.cpp
    src/KillSwitch.h
    src/ActivationWindow.cpp
    src/ActivationWindow.h
    src/Hardening.cpp
    src/Hardening.h
    src/AudioCapture.cpp
    src/AudioCapture.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    src/WasapiProcessLoopback.h
    src/VoiceClient.cpp
    src/VoiceClient.h
    src/AdviceClient.cpp
    src/AdviceClient.h
    # Поиск по законке (бета): клиент /v1/overlay/laws/* — страница хаба + режим «Поиск» бара.
    src/LawsClient.cpp
    src/LawsClient.h
    src/Updater.cpp
    src/Updater.h
    src/update_pubkey.h
    src/Connectivity.cpp
    src/Connectivity.h
    src/relay_cert.h
    src/Theme.cpp
    src/Theme.h
    src/DebugLog.cpp
    src/DebugLog.h
    src/CrashHandler.cpp
    src/CrashHandler.h
    # --- Локальный голосовой пайплайн (sherpa-onnx): STT + диаризация сцены на ПК ---
    src/RingBuffer.cpp
    src/RingBuffer.h
    src/SherpaPipeline.cpp
    src/SherpaPipeline.h
    src/TurnEndpointer.cpp
    src/TurnEndpointer.h
    src/SpeakerLabels.cpp
    src/SpeakerLabels.h
    src/ModelManager.cpp
    src/ModelManager.h
)

# Версия из project(... VERSION ...) — ЕДИНСТВЕННЫЙ источник. Пробрасываем в код как
# APP_VERSION; main.cpp зовёт setApplicationVersion(APP_VERSION). Установщик (RpOverlay.iss)
# и manifest.json должны нести ту же версию (release.sh берёт её из CMake).
target_compile_definitions(RpOverlay PRIVATE APP_VERSION="${PROJECT_VERSION}")
# RPOVERLAY_SEARCH — фича «Поиск по законке» (пункты хаба «Поиск»/«Избранное», режим «Поиск» бара).
# Присутствует в tester(full) и laws-билде, но НЕ в публичном QA. Раньше surface'ы поиска были
# безусловны/гейтились только рантайм-флагом сервера search → пересборка QA протащила бы поиск в
# публичный билд. Теперь предикат жёсткий: поиск есть ⇔ (НЕ QA) ИЛИ laws → компайл-тайм вырезание.
if (RPOVERLAY_LAWS)
    # laws-билд наследует ВСЕ QA-вырезания (#ifndef RPOVERLAY_QA) + свои laws-ветки + поиск.
    target_compile_definitions(RpOverlay PRIVATE RPOVERLAY_QA RPOVERLAY_LAWS RPOVERLAY_SEARCH)
elseif (RPOVERLAY_QA)
    # Публичный QA: поиск ВЫРЕЗАН на компиляции (RPOVERLAY_SEARCH не определён) — не «спрятан».
    target_compile_definitions(RpOverlay PRIVATE RPOVERLAY_QA)
else()
    # Тестерский full-билд: все режимы, включая поиск.
    target_compile_definitions(RpOverlay PRIVATE RPOVERLAY_SEARCH)
endif()

message(STATUS "RpOverlay: единая сборка 0.3.x — компактный бар, все режимы (канал manifest-v2, variant=full)")

target_link_libraries(RpOverlay PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    OpenSSL::Crypto
)

# На Windows для device-auth храним токены в DPAPI (Crypt32) — подключим на этапе auth.
if (WIN32)
    # crypt32 — DPAPI (токены/hwid-ключ); ole32/oleaut32/uuid/winmm/ksuser/avrt/mmdevapi — WASAPI-захват;
    # dwmapi — DwmSetWindowAttribute (снятие системной рамки окна Win11); iphlpapi — GetAdaptersAddresses (MAC для HWID).
    # dbghelp — CrashHandler (StackWalk64/MiniDumpWriteDump): минидамп+стек при краше.
    target_link_libraries(RpOverlay PRIVATE crypt32 ole32 oleaut32 uuid winmm ksuser avrt mmdevapi dwmapi iphlpapi dbghelp)
    # Фиксируем целевую версию Windows ДО windows.h: process-loopback API
    # (ActivateAudioInterfaceAsync и пр.) в mmdeviceapi.h защищён гейтом _WIN32_WINNT>=Win8.
    # 0x0A00 = Windows 10/11 (process-loopback требует Win10 20H1+).
    target_compile_definitions(RpOverlay PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000)
endif()

# --- sherpa-onnx: локальная диаризация + STT (Фаза 1 локального голосового пайплайна) ---
# Рантайм лежит в thirdparty/ (prebuilt win64 shared, C-API DLL + onnxruntime). MinGW линкуется
# НАПРЯМУЮ против MSVC-собранной C-API DLL (проверено Фазой 0.3: gcc smoke.c <...>.dll — без
# import-lib/gendef). Если папки нет (чужой клон без thirdparty) — собираемся без локального STT
# (SherpaPipeline компилится в заглушку через #ifndef HAVE_LOCAL_STT).
set(SHERPA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sherpa-onnx-v1.13.2-win-x64-shared-MD-Release-no-tts")
if (EXISTS "${SHERPA_DIR}/lib/sherpa-onnx-c-api.dll")
    target_include_directories(RpOverlay PRIVATE "${SHERPA_DIR}/include")
    target_link_libraries(RpOverlay PRIVATE "${SHERPA_DIR}/lib/sherpa-onnx-c-api.dll")
    target_compile_definitions(RpOverlay PRIVATE HAVE_LOCAL_STT)
    # ⚠️ КРИТИЧНО (Фаза 0.3): onnxruntime.dll ОБЯЗАН лежать рядом с RpOverlay.exe. В System32 есть
    # ЧУЖОЙ onnxruntime.dll v1.17.1 (Windows ML), а порядок поиска DLL = каталог exe → System32 →
    # … → PATH. Без нашей DLL рядом грузится System32-1.17.1 → «requested API version [24] not
    # available» → segfault. Кладём 3 DLL в каталог сборки сразу после линковки.
    add_custom_command(TARGET RpOverlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_DIR}/lib/sherpa-onnx-c-api.dll"
            "${SHERPA_DIR}/lib/onnxruntime.dll"
            "${SHERPA_DIR}/lib/onnxruntime_providers_shared.dll"
            "$<TARGET_FILE_DIR:RpOverlay>"
        COMMENT "Копирую sherpa-onnx + onnxruntime DLL рядом с RpOverlay.exe (обход System32-хайджека)")
    message(STATUS "RpOverlay: sherpa-onnx найден — локальный STT включён (HAVE_LOCAL_STT)")
else()
    message(WARNING "RpOverlay: ${SHERPA_DIR} НЕ найден — сборка БЕЗ локального STT (заглушка)")
endif()

# --- Иконка приложения ---
# Встраиваем ТОЛЬКО если есть assets/app.ico (генерится из фото, см. scripts/make-icon.sh):
#   app.rc       → иконка в exe (Explorer / ярлык / установщик);
#   resources.qrc→ QIcon ":/assets/app.ico" для окна/таскбара (setWindowIcon в main.cpp).
# Без файла сборка идёт как есть (просто без иконки) — ничего не ломается.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico")
    target_sources(RpOverlay PRIVATE resources.qrc)
    target_compile_definitions(RpOverlay PRIVATE HAVE_APP_ICON)
    if (WIN32)
        enable_language(RC)
        # app.rc генерим из шаблона с АБСОЛЮТНЫМ путём к .ico (прямые слэши) — windres
        # иначе может не найти файл (компилируется из build/, а бэкслеш в .rc — escape).
        set(RPOVERLAY_ICON_ABS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
        target_sources(RpOverlay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
    endif()
    message(STATUS "RpOverlay: assets/app.ico найдена — иконка встроена")
else()
    message(STATUS "RpOverlay: assets/app.ico НЕТ — сборка без иконки (положи .ico и пересобери)")
endif()

# --- RpUpdater.exe — хелпер бесшовного автообновления (ОТДЕЛЬНЫЙ exe, БЕЗ Qt) ---
# Подменяет файлы установки, пока RpOverlay закрыт (rename→replace + .bak-откат), и перезапускает.
# Без Qt-зависимостей — иначе держал бы Qt-DLL открытыми и не смог бы их подменить при апдейте.
# WIN32 (без окна консоли). shell32 — CommandLineToArgvW / ShellExecuteW / SHFileOperationW.
# Собирается в тот же build-каталог рядом с RpOverlay.exe; build-dist.sh копирует его в dist/.
if (WIN32)
    add_executable(RpUpdater WIN32 src/updater_main.cpp)
    target_link_libraries(RpUpdater PRIVATE shell32)
    # Статическая линковка MinGW-рантайма → RpUpdater.exe самодостаточен (НЕ держит libstdc++/libgcc
    # открытыми, иначе не смог бы подменить их при апдейте). Только системные DLL (kernel32/shell32).
    target_link_options(RpUpdater PRIVATE -static -static-libgcc -static-libstdc++)
    # VERSIONINFO для апдейтера (A5): метаданные издателя/версии снижают AV-FP (апдейтер = главный
    # AV-риск проекта, см. план §A2). Иконки нет (headless WIN32).
    enable_language(RC)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/updater.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/updater.rc" @ONLY)
    target_sources(RpUpdater PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/updater.rc")
endif()

# --- Харденинг сборки (Пиллар C: не основная защита, но дёшево поднимает планку, 0 AV-риска) ---
# • -fvisibility=hidden — символы Hwid/LicenseClient/Hardening не торчат в динамической таблице exe
#   (труднее цеплять перехваты по имени; для .exe ничего не экспортируем — поведение не меняется);
# • -s (strip) — снять таблицу символов в РЕЛИЗНОЙ сборке (всё, кроме явного Debug). Манифест-v2
#   считает sha256 уже стрипнутого exe (build-dist копирует именно его) → integrity-чек сходится.
# Пакеры/обфускаторы (UPX/VMProtect) НЕ применяем намеренно: высокий AV-false-positive и они ломают
# подмену файлов бесшовным апдейтером (паковка → карантин при свопе).
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(_tgt RpOverlay RpUpdater)
        if (TARGET ${_tgt})
            target_compile_options(${_tgt} PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
            target_link_options(${_tgt} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-s>)
        endif()
    endforeach()
endif()

# --- (опционально) Themida-защита: SDK-маркеры (Слой A плана усиления защиты) ---
# ПО УМОЛЧАНИЮ ВЫКЛ → макросы в src/ThemidaMarks.h раскрываются в no-op, бинарь НЕ меняется (0 риска,
# 0 AV-FP, бесшовный апдейтер не ломается). Реальная защита включается осознанно:
#   cmake -S . -B build-prot -DWITH_THEMIDA_SDK=ON -DTHEMIDA_SDK_DIR=/path/to/ThemidaSDK
#   cmake --build build-prot   →   BUILD_DIR=build-prot bash build-dist.sh
#   bash scripts/protect-dist.sh   (Themida CLI прогоняет оба exe; .tmd-профили из GUI)
# Полная инструкция и .tmd-настройки — docs/THEMIDA_PROTECTION_SPEC.md.
option(WITH_THEMIDA_SDK "Включить Themida SDK-маркеры защиты (нужен последующий прогон Themida)" OFF)
set(THEMIDA_SDK_DIR "" CACHE PATH "Путь к ThemidaSDK (каталог, содержащий Include/C/ThemidaSDK.h)")
if (WITH_THEMIDA_SDK)
    if (NOT THEMIDA_SDK_DIR OR NOT EXISTS "${THEMIDA_SDK_DIR}/Include/C/ThemidaSDK.h")
        message(FATAL_ERROR "WITH_THEMIDA_SDK=ON, но THEMIDA_SDK_DIR не указывает на ThemidaSDK "
                            "(нет Include/C/ThemidaSDK.h). Задай -DTHEMIDA_SDK_DIR=<путь>.")
    endif()
    foreach(_tgt RpOverlay RpUpdater)
        if (TARGET ${_tgt})
            target_include_directories(${_tgt} PRIVATE "${THEMIDA_SDK_DIR}/Include/C")
            target_compile_definitions(${_tgt} PRIVATE WITH_THEMIDA_SDK)
        endif()
    endforeach()
    message(STATUS "RpOverlay: Themida SDK-маркеры ВКЛЮЧЕНЫ (${THEMIDA_SDK_DIR}) — после сборки прогони protect-dist.sh")
endif()
