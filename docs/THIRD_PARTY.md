# Сторонние компоненты

| Что | Версия | Лицензия | Где |
|---|---|---|---|
| MuPDF | 1.28.6 | AGPL-3.0 | `third_party/` (не в git), патчи — `patches/mupdf/` |
| Lucide (значки) | lucide-static 1.60.0 | ISC | `src/ui/icons_data.c` |
| QR Code generator (Nayuki) | v1.8.0 | MIT | `src/vendor/qrcodegen.c`, `qrcodegen.h` (как есть, из `c/` репозитория nayuki/QR-Code-generator) |
| Шрифты Roboto, PT Sans | прошивка книги | Apache-2.0, OFL | берутся из `/ebrmain/fonts` при запуске, в программу не входят |

Значки скачаны с cdn.jsdelivr.net по одному SVG (список — `scripts/icons.py`) и встроены как
контуры SVG.

- Lucide: ISC License. Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as
  part of Feather (MIT). All other copyright (c) for Lucide are held by Lucide Contributors 2022.

## QR Code generator library (Nayuki)

QR-код с адресом на экране «Приём по Wi‑Fi». Файлы `src/vendor/qrcodegen.c` и `qrcodegen.h` взяты без
изменений с https://github.com/nayuki/QR-Code-generator, тег v1.8.0, каталог `c/`.

MIT License. Copyright (c) Project Nayuki. Полный текст — в шапке каждого из этих файлов.
