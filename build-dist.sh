#!/usr/bin/env bash
# ============================================================================
#  build-dist.sh — собрать самодостаточный dist/ для RpOverlay (Qt6/ucrt64)
#
#  ЗАПУСК (Git Bash / MSYS2):  bash build-dist.sh
#  Требует: собранный build/RpOverlay.exe (cmake --build build) и MSYS2 ucrt64.
#
#  ⚠️ ПОЧЕМУ НЕ ПРОСТО windeployqt6: на MinGW/ucrt64 windeployqt6 НЕ копирует
#  ни не-Qt транзитивные DLL (ICU/pcre2/harfbuzz/glib/...), ни mingw-рантайм,
#  ни OpenSSL, ни плагин tls/qopensslbackend.dll (нужен --force-openssl).
#  Без них приложение НЕ стартует (0xC0000135) или молча мертвы wss/https.
#  Полный список из 26 DLL вычислен рекурсивным замыканием objdump и
#  проверён запуском на чистом PATH. В конце скрипт сам пере-проверяет замыкание.
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DIST="$ROOT/dist"
# Каталог сборки: build = полная (тестерская), build-qa = публичная Q&A (QA_ONLY). Переопределяется
# через env BUILD_DIR, чтобы собирать dist/ под нужный вариант.
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SRC=/c/msys64/ucrt64/bin
WINDEPLOY="$SRC/windeployqt6.exe"
OBJDUMP="$SRC/objdump.exe"

[ -f "$BUILD_DIR/RpOverlay.exe" ] || { echo "НЕТ build/RpOverlay.exe — сперва: cmake --build build"; exit 1; }

echo ">>> Готовлю dist/"
rm -rf "$DIST"; mkdir -p "$DIST"
cp "$BUILD_DIR/RpOverlay.exe" "$DIST/"
# RpUpdater.exe — хелпер бесшовного автообновления (отдельный exe, БЕЗ Qt, статически слинкован).
[ -f "$BUILD_DIR/RpUpdater.exe" ] && cp "$BUILD_DIR/RpUpdater.exe" "$DIST/" && echo "  + RpUpdater.exe"

echo ">>> windeployqt6 (--force-openssl обязателен для tls/qopensslbackend.dll)"
PATH="$SRC:$PATH" "$WINDEPLOY" --release --no-translations --force-openssl --no-system-d3d-compiler \
  --dir "$DIST" "$DIST/RpOverlay.exe"

echo ">>> Докидываю не-Qt DLL + mingw runtime + OpenSSL + glib-core (windeployqt их НЕ копирует)"
MANUAL=(
  # mingw runtime (сборка ucrt64-g++, не MSVC)
  libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll
  # транзитивные зависимости Qt6Core/Gui/Widgets/Network
  libicudt78.dll libicuin78.dll libicuuc78.dll
  libpcre2-16-0.dll libpcre2-8-0.dll zlib1.dll libzstd.dll
  libb2-1.dll libdouble-conversion.dll libmd4c.dll
  libharfbuzz-0.dll libfreetype-6.dll libpng16-16.dll libgraphite2.dll
  libbrotlidec.dll libbrotlicommon.dll libbz2-1.dll libjpeg-8.dll
  # glib-core: Qt6Core на MSYS2 слинкован с glib event-loop -> нужен libglib + его i18n
  libglib-2.0-0.dll libintl-8.dll libiconv-2.dll
  # OpenSSL 3 для wss/https (windeployqt НЕ копирует никогда)
  libssl-3-x64.dll libcrypto-3-x64.dll
)
for d in "${MANUAL[@]}"; do cp "$SRC/$d" "$DIST/" && echo "  + $d"; done

echo ">>> Локальный движок: sherpa-onnx + onnxruntime + MSVC-рантайм"
# sherpa-onnx-c-api.dll слинкована в RpOverlay.exe LOAD-TIME → её замыкание ОБЯЗАНО быть в dist,
# иначе exe вообще не стартует (0xC0000135). onnxruntime — MD-сборка (MSVC CRT), поэтому нужны
# vcruntime140/msvcp140(+_1) из System32 (на чистой машине без VC++ Redist их нет). UCRT
# (api-ms-win-crt-*) копировать НЕ надо — он часть Windows 10+.
SHERPA="$ROOT/thirdparty/sherpa-onnx-v1.13.2-win-x64-shared-MD-Release-no-tts/lib"
[ -f "$SHERPA/sherpa-onnx-c-api.dll" ] || { echo "НЕТ sherpa-onnx в thirdparty/ — релиз с локальным движком невозможен"; exit 1; }
for d in sherpa-onnx-c-api.dll onnxruntime.dll onnxruntime_providers_shared.dll; do
  cp "$SHERPA/$d" "$DIST/" && echo "  + $d"
done
for d in vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_1.dll; do
  cp "/c/Windows/System32/$d" "$DIST/" && echo "  + $d (VC++ runtime)"
done

echo ">>> VAD-модель silero (зашита в установщик — нужна с первого запуска авто-советов)"
# silero_vad.onnx ~1.8МБ кладём в dist/models/ → .iss [Files] (dist\* recursesubdirs) донесёт в {app}/models.
# В отличие от тяжёлых STT-моделей (download по кнопке), VAD нужен сразу → bundle. Резолвится в коде
# через resolveVadPath() (рядом с exe / dev-thirdparty / download-каталог).
mkdir -p "$DIST/models"
SILERO="$ROOT/thirdparty/models/silero_vad.onnx"
if [ -f "$SILERO" ]; then
  cp "$SILERO" "$DIST/models/" && echo "  + models/silero_vad.onnx"
else
  echo "  ! НЕТ $SILERO — авто-советы будут без VAD. Положи silero_vad.onnx в thirdparty/models и пересобери dist."
fi

echo ">>> Убираю networkinformation/qglib.dll (тянет тяжёлый glib-стек gio/gobject; Qt откатится на qnetworklistmanager.dll)"
rm -f "$DIST/networkinformation/qglib.dll"

echo ">>> Пере-проверка замыкания зависимостей (должно быть 0 недостающих)"
declare -A present
while IFS= read -r f; do present["$(basename "$f"|tr 'A-Z' 'a-z')"]=1; done < <(find "$DIST" -type f \( -iname '*.dll' -o -iname '*.exe' \))
mapfile -t queue < <(find "$DIST" -type f \( -iname '*.dll' -o -iname '*.exe' \))
declare -A seen; declare -A missing; i=0; miss_count=0
while [ $i -lt ${#queue[@]} ]; do
  pe="${queue[$i]}"; i=$((i+1)); key="$(basename "$pe"|tr 'A-Z' 'a-z')"
  [ -n "${seen[$key]:-}" ] && continue; seen[$key]=1
  while IFS= read -r dep; do
    dl="$(echo "$dep"|tr 'A-Z' 'a-z')"; [ -n "${present[$dl]:-}" ] && continue
    if [ -f "$SRC/$dep" ] && [ -z "${missing[$dl]:-}" ]; then missing[$dl]="$dep"; queue+=("$SRC/$dep"); miss_count=$((miss_count+1)); fi
  done < <("$OBJDUMP" -p "$pe" 2>/dev/null | grep -i 'DLL Name:' | sed 's/.*DLL Name: *//')
done
# (counter вместо ${#missing[@]} — пустой ассоциативный массив + set -u трогать нельзя)
if [ "$miss_count" -ne 0 ]; then
  echo "!!! НЕ ХВАТАЕТ $miss_count DLL — добавь их в MANUAL:"; for k in "${!missing[@]}"; do echo "    ${missing[$k]}"; done; exit 1
fi
echo ">>> OK: 0 недостающих. Файлов в dist: $(find "$DIST" -type f | wc -l)"
echo ">>> Дальше: bash release.sh  (соберёт установщик с версией из CMake, подпишет и сделает manifest.json)"
