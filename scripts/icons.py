#!/usr/bin/env python3
"""Значки хаба из SVG Lucide → src/ui/icons_data.c (контуры SVG строками).

    python3 -I scripts/icons.py build/icons-src src/ui/icons_data.c

build/icons-src/lucide/<имя>.svg скачаны с cdn.jsdelivr.net (см. ICONS ниже и docs/THIRD_PARTY.md):
  lucide-static@1.60.0/icons/<имя>.svg  (ISC; линии, stroke-width 2 на 24)
Скачать заново: scripts/icons-download.sh (по списку ICONS).
Круги, прямоугольники и линии переводятся в контуры; программа разбирает только path d.
"""
import sys
import xml.etree.ElementTree as ET

# Смысл значка → имя в Lucide (порядок = gfx_icon_id в gfx.h).
ICONS = [
    ("BOOK", "book-open"),
    ("FOLDER", "folder"),
    ("APPS", "layout-grid"),
    ("SETTINGS", "settings"),
    ("TOC", "list-tree"),
    ("BACK", "chevron-left"),
    ("NEXT", "chevron-right"),
    ("SEARCH", "search"),
    ("BOOKMARK", "bookmark"),
    ("WIFI", "wifi"),
    ("WIFI_OFF", "wifi-off"),
    ("FIT_PAGE", "scan"),
    ("SUN", "sun"),
    ("LIBRARY", "library"),
    ("DOWNLOAD", "download"),
    ("CATALOG", "book-down"),
    ("SYNC", "folder-sync"),
    ("PLUS", "plus"),
    # Значки приложений во вкладке «Приложения».
    ("CALC", "calculator"),
    ("CALENDAR", "calendar"),
    ("NOTES", "notebook-pen"),
    ("DRAW", "pencil-line"),
    ("IMAGE", "image"),
    ("GLOBE", "globe"),
    ("CHESS", "crown"),
    ("SUDOKU", "grid-3x3"),
    ("CARDS", "spade"),
    ("DICTIONARY", "languages"),
    ("PUZZLE", "puzzle"),
    ("APP", "app-window"),
]


def num(v):
    return float(v) if v is not None else 0.0


def fmt(x):
    s = ("%.3f" % x).rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


def element_path(el):
    tag = el.tag.split("}")[-1]
    a = el.attrib
    if tag == "path":
        d = a["d"].strip()
        # Первый «m» элемента — от начала координат (SVG), а в общем контуре он стал бы
        # относительным к прошлому элементу: ставим перед ним «M0 0».
        return ("M0 0 " + d) if d.startswith("m") else d
    if tag == "circle":
        cx, cy, r = num(a.get("cx")), num(a.get("cy")), num(a.get("r"))
        return "M%s %sa%s %s 0 1 0 %s 0a%s %s 0 1 0 %s 0Z" % (
            fmt(cx - r), fmt(cy), fmt(r), fmt(r), fmt(2 * r), fmt(r), fmt(r), fmt(-2 * r))
    if tag == "rect":
        x, y, w, h = num(a.get("x")), num(a.get("y")), num(a.get("width")), num(a.get("height"))
        rx = num(a.get("rx", a.get("ry")))
        if rx <= 0:
            return "M%s %sh%sv%sh%sZ" % (fmt(x), fmt(y), fmt(w), fmt(h), fmt(-w))
        rx = min(rx, w / 2, h / 2)
        return ("M%s %sH%sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %sH%sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %sZ" % (
            fmt(x + rx), fmt(y), fmt(x + w - rx), fmt(rx), fmt(rx), fmt(x + w), fmt(y + rx), fmt(y + h - rx),
            fmt(rx), fmt(rx), fmt(x + w - rx), fmt(y + h), fmt(x + rx), fmt(rx), fmt(rx), fmt(x), fmt(y + h - rx),
            fmt(y + rx), fmt(rx), fmt(rx), fmt(x + rx), fmt(y)))
    if tag == "line":
        return "M%s %sL%s %s" % (fmt(num(a.get("x1"))), fmt(num(a.get("y1"))), fmt(num(a.get("x2"))),
                                 fmt(num(a.get("y2"))))
    if tag in ("polyline", "polygon"):
        pts = a["points"].replace(",", " ").split()
        s = "M" + " ".join(pts[:2]) + "L" + " ".join(pts[2:])
        return s + ("Z" if tag == "polygon" else "")
    return ""


def load(path):
    root = ET.parse(path).getroot()
    vb = [float(v) for v in root.attrib["viewBox"].replace(",", " ").split()]
    parts = [element_path(el) for el in root.iter() if el is not root]
    return vb, " ".join(p for p in parts if p)


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    src, out = sys.argv[1], sys.argv[2]
    lines = [
        "/* Создано scripts/icons.py из SVG Lucide — не править вручную.",
        " * Lucide (ISC): docs/THIRD_PARTY.md. */",
        '#include "icons_data.h"',
        "",
    ]
    lines.append("const icon_svg icons_lucide[ICON_COUNT] = {")
    for sense, name in ICONS:
        vb, d = load("%s/lucide/%s.svg" % (src, name))
        lines.append("    [ICON_%s] = { %s, %s, %s, %s, %s }," % (
            sense, fmt(vb[0]), fmt(vb[1]), fmt(vb[2]), fmt(vb[3]), c_string(d)))
    lines.append("};")
    lines.append("")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
