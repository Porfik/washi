# PocketBook InkPad 3 (PB740): что измерено на устройстве

Источник: `hello.app` (этап 1), лог `/mnt/ext1/washi/hello-log.txt`, 2026-11-09.

## Система

| | |
|---|---|
| Прошивка | U740.6.5.1379, `GetDeviceModel()` = `PB740`, `GetHardwareType()` пустой |
| Ядро | Linux 3.10.65+ armv7l, SMP |
| glibc | 2.23 (совпадает с sysroot SDK ветки 6.5) |
| Права приложения | uid = euid = 101, не root |
| ОЗУ | MemTotal 1 024 188 kB; при работающем hello MemFree ≈ 791 МБ |
| CPU | 2× Cortex-A7 r0p5 (part 0xc07), Hardware `sun8iw10` |
| Возможности CPU | neon, vfpv3, vfpv4, idiva, idivt, vfpd32 |
| Частота | governor `interactive`, доступно 240–1008 МГц, scaling_min 400 МГц, при старте 1008 МГц |

## Экран и буфер

- `ScreenWidth/Height` = 1404×1872, `GetOrientation()` = 0 (портрет).
- `GetCanvas()`: 1404×1872, **depth 8, scanline 1408** (строки выровнены, шаг брать из canvas).
- `GetHardwareDepth()` = 4 (16 оттенков).
- Запись серого `v` (0..255) прямо в `canvas->addr` выглядит так же, как `FillArea` с цветом `0xvvvvvv`.
- `FineUpdateSupported()` = 0, `HQUpdateSupported()` = 1.
- `GetWaveformTimes()`: 11 247 576 435 117 435 235 352 270 0 0 0 0 0 435 0 (мс).
  Соответствие режимам пока не установлено.

## Обновление экрана (первые замеры)

Вызовы асинхронные. «До конца» — время до возврата `WaitForUpdateComplete()`.

| Вызов | Область | Вызов | До конца |
|---|---|---|---|
| `FullUpdate` | весь экран | 8 мс | ~627 мс |
| `PartialUpdate` | 40×40, чёрный квадрат на белом | 0–1 мс | 240–290 мс |
| `PartialUpdate` | 1324×260, текст со сглаживанием | 1–3 мс | ~440 мс |

Гипотеза: драйвер сам выбирает режим по содержимому (чисто ч/б ≈ DU, с серыми ≈ GL16). Проверить на этапе 5.

`WaitForUpdateComplete()` в обработчике событий блокирует очередь: события копятся, задержка ввода растёт.
В читалке не ждать завершения обновления в главном потоке.

## Ввод

| Кнопка | Код | Имя в `inkview.h` |
|---|---|---|
| (меню) | 0x17 (23) | `IV_KEY_MENU` |
| назад | 0x18 (24) | `IV_KEY_PREV` |
| вперёд | 0x19 (25) | `IV_KEY_NEXT` |
| самая правая, «Домой» | — | перехватывается системой: приложение получает `EVT_BACKGROUND` (152), при возврате `EVT_FOREGROUND` (151) и `EVT_UPDATE` (95) |

- Удержание: `EVT_KEYREPEAT` (28) впервые через ~1 с, далее каждые ~0,5 с.
- Касания: `EVT_POINTERDOWN/UP` (30/29), par1 = x, par2 = y в экранных координатах портрета; совпадают с местом касания.
- Долгое касание: `EVT_POINTERLONG` (34), затем `EVT_POINTERHOLD` (35).
- `EVT_MTSYNC` (39) приходит потоком (сотни за минуту) — игнорировать без обработки.
- Порядок при старте: `EVT_INIT` → `EVT_SHOW` → `EVT_FOREGROUND`; при выходе через `CloseApp()`: `EVT_HIDE` → `EVT_EXIT`.

## Режимы обновления экрана (тест экрана washi-bench, 2026-11-09)

Время до конца обновления всего экрана, мс (текст / манга / скан с фото):

| Режим | Текст | Манга | Скан | Заметки |
|---|---|---|---|---|
| `FullUpdate` | 780 | 655 | 656 | с миганием |
| `FullUpdateHQ` | 605 | 515 | 516 | с миганием, на глаз не хуже — **для полной перерисовки** |
| `SoftUpdate` / `SoftUpdateHQ` | 520–610 / 507 | 525 / 514 | 530 / 518 | |
| `PartialUpdate` / `PartialUpdateHQ` | 535 / 516 | 522 / 513 | 515 / 507 | без мигания, 16 оттенков — **для листания** |
| `PartialUpdateBW` | 530 | 528 | 535 | |
| `PartialUpdateBlack` | 656 | 655 | 656 | |
| `PartialUpdateDU4` | 366 | 376 | 364 | 4 оттенка |
| `DynamicUpdate` | 522 | 535 | 531 | |
| `DynamicUpdateBW` | 346 | 351 | 348 | 1 бит |
| `DynamicUpdateA2` | 204 (+330 вход) | 214 | 206 | 1 бит, для анимации |

## Система в простое (probe.sh, 2026-11-10)

Процессы прошивки (uid 102, приложение — uid 101 `reader`): `./pocketbook` (монитор, root),
`bookshelf.app` (главный экран, 22 МБ), `eink-reader.app --firsttime` (штатная читалка
держится в памяти, 41 МБ), `eink-cache-reader.app`, `reader_controller.app`, `scanner.app`
(индекс книг), `taskmgr.app`, `usage_stat.app`, `netagent net on` и `wpa_supplicant` (Wi‑Fi).
За 120 с простоя все вместе — около 2 % процессора; частота почти всё время 480 МГц.

Ток батареи читается в `/sys/class/power_supply/battery/current_now` (мкА): 41 мА без сна
с горящим экраном приложения. `/mnt/secure` недоступна приложению.

## Главный экран и USB

- Диалог «Подключение к ПК / Зарядка» показывает `taskmgr.app` (строки `@Choose_USB_mode`,
  `@PC_link`), переключает режим функция `hw_usblink()` из libinkview (в inkview.h её нет).
  При подключении кабеля открытое приложение получает `EVT_BACKGROUND`, а после выбора
  «Подключение к ПК» — `EVT_EXIT`.
- Главный экран — `bookshelf.app`. Прошивка запускает `/mnt/ext1/system/bin/bookshelf.app`,
  если он есть, вместо `/ebrmain/bin/bookshelf.app` (так делают EinkHome и KOReader).
- Штатный главный экран запускает приложения `OpenTask(путь, 0, NULL, TASK_MAKEACTIVE)`,
  книги — `OpenBook`.
- **Главный экран обязан сказать монитору «запущен»**: штатный bookshelf.app один раз шлёт
  `iv_ipc_cmd(0x600, 0)` (MSG_START_SERVICES; монитор: `do_start_services`). Только после
  этого монитор запускает `taskmgr.app` (кнопка «Домой» — `gka_mmnu` → `SetActiveTask(MAINTASK)`,
  окно «Подключение к ПК» — `hw_usblink`), панель управления, `reader_controller`,
  `usage_stat` (если `statistics_enabled`). Без сигнала (первая версия хаба, 2026-11-10) не было
  ни «Домой», ни окна USB; вернуть книгу удалось только сбросом настроек (он удаляет
  `/mnt/ext1/system` целиком и очищает `applications/`, книги остаются).
- Кнопка «Домой» (код 26): в чужой программе — глобальное действие `@KA_mmnu` (taskmgr →
  `SetActiveTask(MAINTASK, 0)`), а если главный экран уже активен, монитор ничего не делает
  (`set_active_task` выходит сразу) — зато сама кнопка тогда приходит главному экрану как
  `EVT_KEYPRESS 26`. Хаб: из книги — в хаб, в хабе — на «Книги».
- Задача главного экрана — по pid процесса, который запустил монитор (`MAINTASK`). Запустить
  `/ebrmain/bin/bookshelf.app` вторым через `OpenTask` нельзя: прошивка находит задачу с тем же
  именем (наш хаб) и просто делает её активной.
- Обе кнопки листания при включении — режим обновления прошивки (руководство InkPad 3).
- Хаб как главный экран (`src/main.c`): узнаёт себя по имени `bookshelf.app`, пишет в лог
  аргументы, с которыми его запустила прошивка, и не принимает их за книгу. Защита от
  падений: в `/mnt/ext1/washi/home-fails` — сколько запусков подряд не дошли до первого
  экрана; 3 — вместо хаба `execv` штатного `/ebrmain/bin/bookshelf.app` (удалить файл —
  снова хаб). Откат без компьютера: «Настройки → Штатный главный экран» — «До перезагрузки»
  (файл-просьба `washi/stock-once`, хаб закрывается, монитор перезапускает главный экран,
  и хаб сразу `execv` штатный) или «Насовсем» (хаб удаляет подмену и закрывается). С
  компьютером: удалить `/mnt/ext1/system/bin/bookshelf.app` и перезагрузить.
- Индексатор прошивки (`scanner.app`) считает книгами `.txt` во всех папках — поэтому лог
  хаба `washi.log` (больше 512 КБ — в `washi.log.1`), сведения обложек — `.inf`.
- В `global.cfg` прошивки в конце строка-контрольная сумма (`#b52575ce`): руками его не
  править, только штатными настройками или `WriteString` + `SaveConfig` из inkview.
  Там `update_interval=@1day` — прошивка сама проверяет обновления, когда есть Wi‑Fi.
- Фоновые программы прошивки (`scanner.app`, `usage_stat.app`, `eink-reader.app`, Wi‑Fi)
  работают от системного пользователя: приложение (uid 101) остановить их не может, нужен root.
  В простое все вместе — около 2 % процессора.
- Шрифты прошивки: `/ebrmain/fonts` — Roboto (Thin…Black), PT Sans/Serif, Noto, DejaVu,
  Liberation.

## PocketBook InkPad 4 (PB743G), 2026-11-24

- Прошивка U743g.6.8.4143, `GetDeviceModel()` = `PB743G`. Экран как у InkPad 3: 1404×1872,
  canvas depth 8, scanline 1408. ОЗУ 999 МБ. Метка тома PB743G (`PB_LABEL=PB743G scripts/pb.sh …`).
- Хаб той же сборки работает без изменений: нужны только GLIBC ≤ 2.17 и базовые функции
  inkview, остальное — через dlsym. `GetDeviceKey` нет, `GetSerialNumber` есть (20 знаков).
  Wi‑Fi через `NetConnectAsync` — 3,2 с.
- Подмена главного экрана та же: монитор (`/ebrmain/pocketbook` → `bin/monitor.app`) знает
  `/mnt/ext1/system/bin/bookshelf.app`, штатный `bookshelf.app` шлёт `iv_ipc_cmd(0x600)`, как в 6.5.
- В простое у штатного экрана работают `eink-reader.app` (49 МБ), `bookshelf.app` (34 МБ),
  `scanner.app`, `eink-cache-reader`, `taskmgr.app`, `reader_controller`, Wi‑Fi.
- Новые частые события 6.8: 217 `EVT_POSTPONE_TIMED_POWEROFF` (на касания), 216
  `EVT_SCANSTOPPED`, 220 `EVT_READ_PROGRESS_CHANGED`, 73 `EVT_FSCHANGED`.
- Прошивка знает об обновлении U743g.6.11.1683 (`new_firmware=1`), автопроверка и автозагрузка
  выключены. Хаб проверен только на 6.8.
