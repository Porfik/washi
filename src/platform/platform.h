/* platform — тонкий слой платформы: экран, ввод, таймер, цикл событий.
 *
 * Реализации: inkview.c (книга PocketBook) и sdl.c (окно на ПК).
 * Остальной код не знает, на какой платформе работает.
 * Все функции, кроме pf_data_dir, вызываются только из потока цикла событий.
 */
#ifndef WASHI_PLATFORM_H
#define WASHI_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *pixels; /* 8 бит серого на пиксель: 0 — чёрный, 255 — белый */
    int width, height;
    int stride; /* байт на строку, может быть больше width */
} pf_screen;

typedef enum {
    PF_UPDATE_FULL,    /* полное, с миганием: убирает следы прошлых картинок */
    PF_UPDATE_PARTIAL, /* без мигания, с оттенками серого */
    PF_UPDATE_FAST,    /* самое быстрое, оттенки могут теряться */
    /* Остальные режимы InkView — для подбора на книге (тест экрана в washi-bench).
     * Каких нет в прошивке, те делаются как PF_UPDATE_PARTIAL; pf_update_mode_available. */
    PF_UPDATE_FULL_HQ,
    PF_UPDATE_SOFT,
    PF_UPDATE_SOFT_HQ,
    PF_UPDATE_PARTIAL_HQ,
    PF_UPDATE_PARTIAL_BW,
    PF_UPDATE_PARTIAL_BLACK,
    PF_UPDATE_PARTIAL_DU4,
    PF_UPDATE_FAST_BW,
    PF_UPDATE_A2,
    PF_UPDATE_MODES
} pf_update_mode;

typedef enum {
    PF_KEY_UNKNOWN,
    PF_KEY_PREV,
    PF_KEY_NEXT,
    PF_KEY_MENU,
    PF_KEY_HOME, /* «Домой»: в хаб, на главную вкладку */
} pf_key;

typedef enum {
    PF_EVENT_START, /* первое событие: подготовиться, экран ещё не рисовать */
    PF_EVENT_SHOW,  /* перерисовать экран целиком (запуск, возврат из системы) */
    PF_EVENT_KEY_DOWN,
    PF_EVENT_KEY_UP,
    PF_EVENT_KEY_REPEAT,
    PF_EVENT_POINTER_DOWN,
    PF_EVENT_POINTER_UP,
    PF_EVENT_POINTER_LONG, /* палец держат на месте (~1 с); после него ещё придёт POINTER_UP */
    PF_EVENT_POINTER_MOVE, /* палец ведут (между DOWN и UP; часто — обрабатывать быстро) */
    PF_EVENT_TIMER,
    PF_EVENT_AUX_TIMER, /* второй таймер (pf_set_aux_timer) */
    PF_EVENT_BACKGROUND, /* приложение уходит в фон */
    PF_EVENT_EXIT,       /* последнее событие: сохранить состояние */
} pf_event_type;

typedef struct {
    pf_event_type type;
    pf_key key;
    int raw_key; /* код клавиши платформы, для лога */
    int x, y;
    double time_ms; /* clock_ms() в момент получения события */
} pf_event;

typedef void (*pf_handler)(const pf_event *ev, void *user);

/* Запускает цикл событий и возвращается после выхода из него. */
int pf_run(pf_handler handler, void *user);

/* Просит завершить приложение; последним придёт PF_EVENT_EXIT. */
void pf_quit(void);

/* Буфер экрана; NULL, если формат не 8 бит серого. В альбомной ориентации — логический
 * экран (ширина больше высоты), платформа сама поворачивает его при pf_update. */
pf_screen *pf_get_screen(void);

/* Ориентация: 0 — портрет, 1 — альбом (верх у правого края книги), 2 — альбом (верх у левого).
 * Экран после смены — белый; касания приходят уже в координатах новой ориентации. */
void pf_set_orientation(int orient);
int pf_get_orientation(void);

/* Показывает область буфера на экране. Вызов не ждёт окончания обновления e-ink. */
void pf_update(int x, int y, int w, int h, pf_update_mode mode);

/* Через ms придёт один PF_EVENT_TIMER; повторный вызов перезапускает таймер. */
void pf_set_timer(int ms);

/* Второй, независимый таймер: через ms придёт один PF_EVENT_AUX_TIMER. */
void pf_set_aux_timer(int ms);

/* Заряд батареи, %; -1 — неизвестно. charging (может быть NULL) — идёт ли зарядка. */
int pf_battery(int *charging);

/* Запустить другое приложение (путь к .app) и показать его; эта программа уходит в фон
 * и вернётся на экран, когда то закроется. 0 — запущено, -1 — нет. */
int pf_launch(const char *path);

/* Хаб — главный экран: сказать монитору прошивки «главный экран запущен», как это делает
 * штатный bookshelf.app (iv_ipc_cmd(0x600) — MSG_START_SERVICES). Без этого монитор не
 * запускает диспетчер задач (кнопка «Домой», окно «Подключение к ПК», переключение задач),
 * панель управления и службы. Один раз после первого показа экрана. */
void pf_home_started(void);

/* Сбор статистики прошивкой (statistics_enabled в её global.cfg; служба usage_stat
 * запускается, только если он включён): 1 / 0, -1 — неизвестно. Запись — через API прошивки
 * (файл с контрольной суммой), действует со следующего запуска службы. */
int pf_fw_statistics(void);
void pf_set_fw_statistics(int on);

/* Открыть книгу штатной читалкой прошивки — для форматов, которых нет у нас (EPUB, DjVu…).
 * 0 — открывается. */
int pf_open_book(const char *path);

/* Подсветка: яркость 0..100 (0 — выключена) и теплота 0..100 (-1 — у экрана нет).
 * pf_light_get: 0 — есть подсветка, -1 — нет. */
int pf_light_get(int *brightness, int *warmth);
void pf_light_set(int brightness, int warmth);

/* Wi‑Fi: 1 — включён, 0 — выключен, -1 — неизвестно; pf_wifi_set — включить или выключить. */
int pf_wifi_get(void);
void pf_wifi_set(int on);

/* Подключение к сети, не блокируя экран: pf_net_start — начать (1 — сеть уже есть, 0 — идёт
 * подключение к известной сети, -1 — не начать); pf_net_poll — раз в тик, пока идёт: 1 —
 * подключено, 0 — ещё идёт, -1 — не вышло (нет известной сети рядом, Wi‑Fi выключен…).
 * Тогда — pf_net_connect: окно выбора сети прошивки (блокирует, пока человек выбирает; после
 * него экран нужно нарисовать заново). 0 — сеть есть, -1 — нет.
 * На ПК сеть есть всегда; WASHI_NET=slow — подключение 3 с, fail — не выходит (для снимков). */
int pf_net_start(void);
int pf_net_poll(void);
int pf_net_connect(void);

/* Экранная клавиатура прошивки: ввести строку (до maxlen байт). done вызывается с текстом
 * или NULL (отмена), после чего экран нужно нарисовать заново. */
typedef void (*pf_keyboard_done)(const char *text, void *user);
void pf_keyboard(const char *title, const char *initial, int maxlen, pf_keyboard_done done, void *user);

/* Строка, присущая этому устройству и не лежащая в файлах, видных по USB (ключ и серийный
 * номер из прошивки), — из неё выводится ключ шифрования паролей (src/base/secret.h).
 * 0 — есть, -1 — платформа такого не знает. */
int pf_device_secret(char *buf, size_t len);

/* Не давать устройству засыпать ближайшие sec секунд. */
void pf_keep_awake(int sec);

/* Папка приложения: настройки, лог. */
const char *pf_data_dir(void);

const char *pf_update_mode_name(pf_update_mode mode);

/* Есть ли режим на этой платформе (в этой прошивке). */
int pf_update_mode_available(pf_update_mode mode);

/* Дождаться конца всех начатых обновлений e-ink (для замеров; блокирует поток). */
void pf_wait_update(void);

/* Сведения об экране для лога (на книге — длительности режимов e-ink). */
void pf_screen_info(char *buf, size_t len);

/* Сколько событий ввода ждёт в очереди; -1 — неизвестно. */
int pf_pending_input(void);

#endif
