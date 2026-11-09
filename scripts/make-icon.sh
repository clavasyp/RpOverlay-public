#!/usr/bin/env bash
# ============================================================================
#  make-icon.sh — сделать assets/app.ico из фото (PNG/JPG) для иконки приложения.
#  Использование:  bash scripts/make-icon.sh <путь-к-фото>
#  Требует Python с Pillow (есть в системном python: PIL 12+).
#
#  Фото центр-кропится в квадрат (без искажений) и пакуется в мультиразмерный .ico
#  (256/128/64/48/32/16). Дальше CMake встроит app.ico в exe (см. CMakeLists).
# ============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-}"
[ -n "$SRC" ] && [ -f "$SRC" ] || { echo "Укажи путь к фото:  bash scripts/make-icon.sh <photo.png|jpg>"; exit 1; }

# Берём python, у которого ЕСТЬ Pillow (ucrt64-python его обычно не имеет — пропустим).
PY=""
for cand in "${RPOVERLAY_PYTHON:-}" python python3 py; do
  [ -n "$cand" ] || continue
  if command -v "$cand" >/dev/null 2>&1 && "$cand" -c "import PIL" >/dev/null 2>&1; then PY="$cand"; break; fi
done
[ -n "$PY" ] || { echo "Не нашёл python с Pillow. Установи (pip install pillow) или задай RPOVERLAY_PYTHON=/путь/к/python.exe"; exit 1; }

OUT="$ROOT/assets/app.ico"
mkdir -p "$ROOT/assets"
"$PY" - "$SRC" "$OUT" <<'PY'
import sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGBA")
w, h = im.size
side = min(w, h)                     # центр-кроп в квадрат (заполняет иконку, без полос)
left, top = (w - side) // 2, (h - side) // 2
im = im.crop((left, top, left + side, top + side))
im.save(out, format="ICO", sizes=[(256,256),(128,128),(64,64),(48,48),(32,32),(16,16)])
print("OK:", out, im.size)
PY
echo ">>> Готово: $OUT  (дальше: cmake --build build && bash build-dist.sh && bash release.sh)"
