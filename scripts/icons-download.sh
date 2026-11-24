#!/bin/sh
# Скачать SVG значков для scripts/icons.py в build/icons-src/ (cdn.jsdelivr.net, ~50 КБ).
# Потом: python3 -I scripts/icons.py build/icons-src src/ui/icons_data.c
set -e
D=${1:-build/icons-src}
mkdir -p "$D/lucide"
python3 -I - <<'PY' | while read -r name url; do curl -sf "$url" -o "$D/lucide/$name.svg" || echo "нет: $url" >&2; done
import importlib.util
spec = importlib.util.spec_from_file_location("icons", "scripts/icons.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
for _, name in m.ICONS:
    print(name, "https://cdn.jsdelivr.net/npm/lucide-static@1.60.0/icons/%s.svg" % name)
PY
