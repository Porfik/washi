/* Платформа PocketBook через InkView. Коды кнопок и порядок событий — docs/device.md. */
#include "platform.h"

#include <dlfcn.h>
#include <inkview.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../base/clock.h"
#include "../base/log.h"
#include "rotate.h"

static pf_handler app_handler;
static void *app_user;
static pf_screen screen; /* физический экран (canvas InkView) */
static rotator rot;      /* логический экран в альбомной ориентации */
static int in_background;
static int wifi_off_pending;      /* выключение Wi‑Fi отложено до таймера (pf_wifi_set) */
static double dialog_bg_until;    /* до этого времени «в фон» — эхо окна прошивки, не настоящий уход */
static int net_connected(void);
static double last_input_ms;      /* последнее касание или кнопка (выключать Wi‑Fi — только в покое) */
static double last_show_ms; /* когда последний раз показывали экран (EVT_SHOW) */

static int phys_screen(void)
{
    icanvas *c = GetCanvas();
    if (!c || c->depth != 8)
        return -1;
    screen.pixels = c->addr;
    screen.width = c->width;
    screen.height = c->height;
    screen.stride = c->scanline;
    return 0;
}

/* Модель, прошивка и экран — в лог при запуске: хаб ставят на разные модели. */
static void log_device(void)
{
    const char *(*str)(void);
    const char *model = NULL, *fw = NULL, *hw = NULL;
    if ((str = (const char *(*)(void))dlsym(RTLD_DEFAULT, "GetDeviceModel")))
        model = str();
    if ((str = (const char *(*)(void))dlsym(RTLD_DEFAULT, "GetSoftwareVersion")))
        fw = str();
    if ((str = (const char *(*)(void))dlsym(RTLD_DEFAULT, "GetHardwareType")))
        hw = str();
    icanvas *c = GetCanvas();
    log_msg("устройство: %s, прошивка %s, железо %s; экран %d×%d, глубина %d, строка %d",
            model && *model ? model : "?", fw && *fw ? fw : "?", hw && *hw ? hw : "?",
            c ? c->width : 0, c ? c->height : 0, c ? c->depth : 0, c ? c->scanline : 0);
}

static pf_key map_key(int code)
{
    switch (code) {
    case IV_KEY_PREV: return PF_KEY_PREV;
    case IV_KEY_NEXT: return PF_KEY_NEXT;
    case IV_KEY_MENU: return PF_KEY_MENU;
    case IV_KEY_HOME: return PF_KEY_HOME; /* приходит, когда хаб — главный экран и активен */
    default: return PF_KEY_UNKNOWN;
    }
}

static void dispatch(pf_event_type type, int raw_key, int x, int y, double t)
{
    if (rot.orient && (type == PF_EVENT_POINTER_DOWN || type == PF_EVENT_POINTER_UP || type == PF_EVENT_POINTER_LONG ||
                       type == PF_EVENT_POINTER_MOVE) && phys_screen() == 0)
        rot_touch(&rot, &screen, &x, &y);
    if (type >= PF_EVENT_KEY_DOWN && type <= PF_EVENT_POINTER_MOVE)
        last_input_ms = t;
    pf_event ev = { type, map_key(raw_key), raw_key, x, y, t };
    app_handler(&ev, app_user);
}

static void on_timer(void)
{
    dispatch(PF_EVENT_TIMER, 0, 0, 0, clock_ms());
}

static void on_aux_timer(void)
{
    dispatch(PF_EVENT_AUX_TIMER, 0, 0, 0, clock_ms());
}

static int on_event(int type, int par1, int par2)
{
    double t = clock_ms();
    switch (type) {
    case EVT_INIT:
        SetPanelType(PANEL_DISABLED);
        log_device();
        dispatch(PF_EVENT_START, 0, 0, 0, t);
        return 1;
    case EVT_SHOW:
        dispatch(PF_EVENT_SHOW, 0, 0, 0, t);
        last_show_ms = clock_ms(); /* после отрисовки: FOREGROUND следом за SHOW — тот же показ */
        return 1;
    case EVT_BACKGROUND:
        if (t < dialog_bg_until) {
            log_msg("InkView: «в фон» сразу после окна прошивки — не считаем");
            return 1;
        }
        log_msg("InkView: в фон");
        in_background = 1;
        dispatch(PF_EVENT_BACKGROUND, 0, 0, 0, t);
        return 1;
    case EVT_FOREGROUND:
        /* При запуске FOREGROUND приходит сразу после SHOW — тогда не перерисовываем. После фона
         * или когда нас сделали активными («Домой» из другой программы) — перерисовка. */
        log_msg("InkView: на экран%s; Wi‑Fi %s", in_background ? " (после фона)" : "",
                net_connected() ? "подключён" : "не подключён");
        if (in_background || t - last_show_ms > 1000) {
            in_background = 0;
            dispatch(PF_EVENT_SHOW, 0, 0, 0, t);
            last_show_ms = clock_ms();
        }
        return 1;
    case EVT_GLOBALREQUEST:
        /* Запросы кнопок прошивки (REQ_MAINMENU — «Домой»). Главному экрану — стать активным. */
        log_msg("InkView: глобальный запрос %d", par1);
        if (par1 == REQ_MAINMENU)
            SetActiveTask(GetCurrentTask(), 0);
        return 0;
    case EVT_KEYPRESS:
        dispatch(PF_EVENT_KEY_DOWN, par1, 0, 0, t);
        return 1;
    case EVT_KEYRELEASE:
        dispatch(PF_EVENT_KEY_UP, par1, 0, 0, t);
        return 1;
    case EVT_KEYREPEAT:
        dispatch(PF_EVENT_KEY_REPEAT, par1, 0, 0, t);
        return 1;
    case EVT_POINTERDOWN:
        if (in_background) { /* касание пришло нам — значит, мы на экране, а «на экран» потерялось */
            log_msg("InkView: касание в «фоне» — считаем, что на экране");
            in_background = 0;
            dispatch(PF_EVENT_SHOW, 0, 0, 0, t);
            last_show_ms = clock_ms();
        }
        dispatch(PF_EVENT_POINTER_DOWN, 0, par1, par2, t);
        return 1;
    case EVT_POINTERUP:
        dispatch(PF_EVENT_POINTER_UP, 0, par1, par2, t);
        return 1;
    case EVT_POINTERLONG:
    case EVT_POINTERHOLD:
        /* В этой прошивке на долгое касание приходит EVT_POINTERHOLD (35); LONG — на всякий
         * случай. Повторы отсекает распознавание жестов (gesture.c). */
        dispatch(PF_EVENT_POINTER_LONG, 0, par1, par2, t);
        return 1;
    case EVT_POINTERMOVE:
    case EVT_POINTERDRAG:
        dispatch(PF_EVENT_POINTER_MOVE, 0, par1, par2, t);
        return 1;
    case EVT_MTSYNC:
    /* Прошивка 6.8 шлёт их часто, хабу они не нужны: отложить автовыключение (на каждое
     * касание), индексатор закончил, читалка сменила прогресс. */
    case EVT_POSTPONE_TIMED_POWEROFF:
    case EVT_SCANSTOPPED:
    case EVT_READ_PROGRESS_CHANGED:
        return 1;
    case EVT_EXIT:
        dispatch(PF_EVENT_EXIT, 0, 0, 0, t);
        return 1;
    default:
        /* Для хаба: какие ещё события присылает прошивка (USB, сон, обложка, сеть). */
        log_msg("событие InkView %d (%d, %d)", type, par1, par2);
        return 0;
    }
}

int pf_run(pf_handler handler, void *user)
{
    app_handler = handler;
    app_user = user;
    InkViewMain(on_event);
    return 0;
}

void pf_quit(void)
{
    CloseApp();
}

pf_screen *pf_get_screen(void)
{
    if (phys_screen() < 0)
        return NULL;
    return rot_screen(&rot, &screen);
}

void pf_set_orientation(int orient)
{
    if (phys_screen() == 0 && rot_set(&rot, orient, &screen) != 0)
        log_msg("ориентация %d: нет памяти под экран", orient);
}

int pf_get_orientation(void)
{
    return rot.orient;
}

/* Режимы сверх основных берутся из libinkview при работе: если какого-то нет в прошивке,
 * программа всё равно запускается. */
typedef void (*update_all_fn)(void);
typedef void (*update_rect_fn)(int x, int y, int w, int h);

static const char *const extra_names[PF_UPDATE_MODES] = {
    [PF_UPDATE_FULL_HQ] = "FullUpdateHQ",
    [PF_UPDATE_SOFT] = "SoftUpdate",
    [PF_UPDATE_SOFT_HQ] = "SoftUpdateHQ",
    [PF_UPDATE_PARTIAL_HQ] = "PartialUpdateHQ",
    [PF_UPDATE_PARTIAL_BW] = "PartialUpdateBW",
    [PF_UPDATE_PARTIAL_BLACK] = "PartialUpdateBlack",
    [PF_UPDATE_PARTIAL_DU4] = "PartialUpdateDU4",
    [PF_UPDATE_FAST_BW] = "DynamicUpdateBW",
    [PF_UPDATE_A2] = "DynamicUpdateA2",
};
static void *extra_fn[PF_UPDATE_MODES];
static int extra_looked_up;

static void look_up_modes(void)
{
    if (extra_looked_up)
        return;
    for (int m = 0; m < PF_UPDATE_MODES; m++)
        if (extra_names[m])
            extra_fn[m] = dlsym(RTLD_DEFAULT, extra_names[m]);
    extra_looked_up = 1;
}

int pf_update_mode_available(pf_update_mode mode)
{
    if (mode == PF_UPDATE_FULL || mode == PF_UPDATE_PARTIAL || mode == PF_UPDATE_FAST)
        return 1;
    look_up_modes();
    return mode < PF_UPDATE_MODES && extra_fn[mode] != NULL;
}

void pf_update(int x, int y, int w, int h, pf_update_mode mode)
{
    static int in_a2;
    if (rot.orient && phys_screen() == 0) {
        int o[4];
        rot_flush(&rot, &screen, x, y, w, h, o);
        x = o[0], y = o[1], w = o[2], h = o[3];
        if (w <= 0 || h <= 0)
            return;
    }
    if (in_a2 && mode != PF_UPDATE_A2) {
        void (*exit_a2)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "ExitUpdateA2");
        if (exit_a2)
            exit_a2();
        in_a2 = 0;
    }
    switch (mode) {
    case PF_UPDATE_FULL:
        FullUpdate();
        return;
    case PF_UPDATE_PARTIAL:
        PartialUpdate(x, y, w, h);
        return;
    case PF_UPDATE_FAST:
        DynamicUpdate(x, y, w, h);
        return;
    default:
        break;
    }
    if (!pf_update_mode_available(mode)) {
        if (mode == PF_UPDATE_FULL_HQ || mode == PF_UPDATE_SOFT || mode == PF_UPDATE_SOFT_HQ)
            FullUpdate();
        else
            PartialUpdate(x, y, w, h);
        return;
    }
    if (mode == PF_UPDATE_FULL_HQ || mode == PF_UPDATE_SOFT || mode == PF_UPDATE_SOFT_HQ) {
        ((update_all_fn)extra_fn[mode])();
        return;
    }
    if (mode == PF_UPDATE_A2)
        in_a2 = 1;
    ((update_rect_fn)extra_fn[mode])(x, y, w, h);
}

void pf_wait_update(void)
{
    WaitForUpdateComplete();
}

void pf_set_timer(int ms)
{
    SetHardTimer("washi", on_timer, ms);
}

void pf_set_aux_timer(int ms)
{
    SetHardTimer("washi-aux", on_aux_timer, ms);
}

int pf_battery(int *charging)
{
    if (charging)
        *charging = IsCharging() > 0;
    return GetBatteryPower();
}

int pf_launch(const char *path)
{
    /* Как штатный главный экран (bookshelf.app): OpenTask с TASK_MAKEACTIVE. */
    int rc = OpenTask(path, 0, NULL, TASK_MAKEACTIVE);
    return rc < 0 ? -1 : 0;
}

void pf_home_started(void)
{
    int (*ipc_cmd)(int, int) = (int (*)(int, int))dlsym(RTLD_DEFAULT, "iv_ipc_cmd");
    if (!ipc_cmd) {
        log_msg("главный экран: нет iv_ipc_cmd — службы прошивки не запустятся");
        return;
    }
    int rc = ipc_cmd(0x600, 0); /* MSG_START_SERVICES, как у штатного bookshelf.app */
    log_msg("главный экран: монитору «службы» (0x600) = %d", rc);
}

int pf_fw_statistics(void)
{
    iconfig *cfg = GetGlobalConfig();
    return cfg ? ReadInt(cfg, "statistics_enabled", 1) != 0 : -1;
}

void pf_set_fw_statistics(int on)
{
    iconfig *cfg = GetGlobalConfig();
    if (!cfg)
        return;
    WriteInt(cfg, "statistics_enabled", on ? 1 : 0);
    int rc = SaveConfig(cfg);
    NotifyConfigChanged();
    log_msg("статистика прошивки: %s (SaveConfig = %d)", on ? "включена" : "выключена", rc);
}

int pf_open_book(const char *path)
{
    /* Как штатный главный экран: OpenBook — книга в программе, назначенной формату. */
    int rc = OpenBook(path, "", 0);
    log_msg("OpenBook(%s) = %d", path, rc);
    return rc < 0 ? -1 : 0;
}

/* Подсветка — через dlsym: если функции нет в прошивке, программа всё равно запускается. */
static void *light_fn(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

/* Что стоит сейчас (по последнему pf_light_get или pf_light_set); -1 — неизвестно. Меняют
 * подсветку и вне программы, поэтому перед жестом и в листах уровень читается заново. */
static int light_known_b = -1, light_known_w = -1;

int pf_light_get(int *brightness, int *warmth)
{
    int (*get_state)(void) = (int (*)(void))light_fn("GetFrontlightState");
    int (*get_enabled)(void) = (int (*)(void))light_fn("GetFrontlightEnabled");
    int (*get_color)(void) = (int (*)(void))light_fn("GetFrontlightColor");
    if (!get_state)
        return -1;
    int b = get_state();
    if (get_enabled && !get_enabled())
        b = 0;
    *brightness = b < 0 ? 0 : b > 100 ? 100 : b;
    int c = get_color ? get_color() : -1;
    *warmth = c < 0 ? -1 : c > 100 ? 100 : c;
    light_known_b = *brightness;
    light_known_w = *warmth;
    return 0;
}

void pf_light_set(int brightness, int warmth)
{
    void (*set_state)(int) = (void (*)(int))light_fn("SetFrontlightState");
    void (*set_enabled)(int) = (void (*)(int))light_fn("SetFrontlightEnabled");
    void (*set_color)(int) = (void (*)(int))light_fn("SetFrontlightColor");
    /* Только то, что изменилось: каждый вызов прошивки пишет её настройки, а жест вдоль края
     * зовёт pf_light_set много раз подряд. */
    int on = brightness > 0;
    if (set_enabled && (light_known_b < 0 || (light_known_b > 0) != on))
        set_enabled(on);
    if (set_state && on && brightness != light_known_b)
        set_state(brightness);
    if (set_color && warmth >= 0 && warmth != light_known_w)
        set_color(warmth);
    light_known_b = brightness;
    light_known_w = warmth;
}

int pf_wifi_get(void)
{
    if (wifi_off_pending)
        return 0;
    int (*query)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "QueryNetwork");
    if (!query)
        return -1;
    int q = query();
    log_msg("QueryNetwork: 0x%x", q);
    if (!(q & NET_WIFI))
        return -1;
    return (q & NET_WIFIREADY) ? 1 : 0;
}


static void wifi_power(int on)
{
    int (*power)(int) = (int (*)(int))dlsym(RTLD_DEFAULT, "WiFiPower");
    int (*disconnect)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "NetDisconnect");
    double t0 = clock_ms();
    if (!on && disconnect)
        disconnect();
    int rc = power ? power(on) : -1;
    log_msg("Wi‑Fi %s: WiFiPower → %d за %.0f ms", on ? "вкл" : "выкл", rc, clock_ms() - t0);
}

#define WIFI_OFF_IDLE_MS 15000 /* выключать Wi‑Fi, когда столько не трогали книгу */

static void wifi_off_timer(void)
{
    if (!wifi_off_pending)
        return;
    double idle = clock_ms() - last_input_ms;
    if (idle < WIFI_OFF_IDLE_MS) { /* человек ещё пользуется книгой — подождать */
        SetHardTimer("washi-wifi", wifi_off_timer, (int)(WIFI_OFF_IDLE_MS - idle) + 100);
        return;
    }
    wifi_off_pending = 0;
    wifi_power(0);
}

static void wifi_off_cancel(void)
{
    if (wifi_off_pending) {
        wifi_off_pending = 0;
        ClearTimer(wifi_off_timer);
    }
}

void pf_wifi_set(int on)
{
    if (!on) {
        /* WiFiPower(0) держит поток интерфейса 2–4,5 с (замерено на книге): касания в это время
         * не доходят. Поэтому выключаем, только когда книгу 15 с не трогали. */
        wifi_off_pending = 1;
        SetHardTimer("washi-wifi", wifi_off_timer, WIFI_OFF_IDLE_MS);
        return;
    }
    wifi_off_cancel();
    wifi_power(1);
}

/* Подключена ли сеть. QueryNetwork для этого не годится: NET_WIFIREADY — «Wi‑Fi включён»,
 * а не «подключён». */
static int net_connected(void)
{
    iv_netinfo *(*info)(void) = (iv_netinfo * (*)(void)) dlsym(RTLD_DEFAULT, "NetInfo");
    iv_netinfo *ni = info ? info() : NULL;
    return ni && ni->connected;
}

int pf_device_secret(char *buf, size_t len)
{
    char *(*key)(void) = (char *(*)(void))dlsym(RTLD_DEFAULT, "GetDeviceKey");
    char *(*serial)(void) = (char *(*)(void))dlsym(RTLD_DEFAULT, "GetSerialNumber");
    const char *k = key ? key() : NULL, *n = serial ? serial() : NULL; /* владелец строк — прошивка */
    snprintf(buf, len, "%s|%s", k ? k : "", n ? n : "");
    log_msg("ключ устройства: GetDeviceKey %s (%zu), GetSerialNumber %s (%zu)", k && *k ? "есть" : "нет",
            k ? strlen(k) : 0, n && *n ? "есть" : "нет", n ? strlen(n) : 0);
    return (k && *k) || (n && *n) ? 0 : -1;
}

/* Подключение без блокировки: NetConnectAsync (прошивка подключает к известной сети в фоне,
 * cb — когда закончила); ждём по NetInfo. Без NetConnectAsync — молча, с блокировкой. */
#define NET_ASYNC_MS 40000 /* дольше — считаем, что известной сети рядом нет */

static volatile int net_cb_done, net_cb_status;
static int net_waiting;
static double net_t0;

static int net_async_cb(int status)
{
    net_cb_status = status;
    net_cb_done = 1;
    return 0;
}

int pf_net_start(void)
{
    wifi_off_cancel(); /* снова нужна сеть — отложенное выключение не нужно */
    if (net_connected())
        return 1;
    int (*async)(int (*)(int)) = (int (*)(int (*)(int)))dlsym(RTLD_DEFAULT, "NetConnectAsync");
    net_t0 = clock_ms();
    if (!async) {
        int (*silent)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "NetConnectSilent");
        int rs = silent ? silent(NULL) : -1;
        log_msg("сеть: нет NetConnectAsync, молча → %d за %.0f ms", rs, clock_ms() - net_t0);
        return net_connected() ? 1 : -1;
    }
    net_cb_done = 0;
    net_waiting = 1;
    int rc = async(net_async_cb);
    log_msg("сеть: NetConnectAsync → %d за %.0f ms", rc, clock_ms() - net_t0);
    return net_connected() ? 1 : 0;
}

int pf_net_poll(void)
{
    if (net_connected()) {
        if (net_waiting)
            log_msg("сеть: подключена за %.0f ms", clock_ms() - net_t0);
        net_waiting = 0;
        return 1;
    }
    if (!net_waiting)
        return -1;
    double dt = clock_ms() - net_t0;
    /* Прошивка сказала «всё» (с ошибкой или без), а сети нет — подождать ещё чуть-чуть:
     * NetInfo может обновиться позже обратного вызова. */
    if ((net_cb_done && (net_cb_status < 0 || dt > 3000)) || dt > NET_ASYNC_MS) {
        log_msg("сеть: не подключилась за %.0f ms (ответ прошивки: %s %d)", dt, net_cb_done ? "был" : "нет",
                net_cb_status);
        net_waiting = 0;
        return -1;
    }
    return 0;
}

int pf_net_connect(void)
{
    if (net_connected())
        return 0;
    int (*dialog)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "NetConnect");
    double t0 = clock_ms();
    int rd = dialog ? dialog(NULL) : -1, ok = net_connected();
    /* После окна прошивка присылает «в фон» уже после «на экран» — это эхо окна. */
    dialog_bg_until = clock_ms() + 3000;
    log_msg("сеть: окно выбора сети → %d за %.0f ms, подключена: %d", rd, clock_ms() - t0, ok);
    return ok ? 0 : -1;
}

static pf_keyboard_done kbd_done;
static void *kbd_user;
static char kbd_buf[512];

static void kbd_handler(char *text)
{
    pf_keyboard_done done = kbd_done;
    kbd_done = NULL;
    if (done)
        done(text && *text ? text : NULL, kbd_user);
}

void pf_keyboard(const char *title, const char *initial, int maxlen, pf_keyboard_done done, void *user)
{
    if (maxlen <= 0 || maxlen >= (int)sizeof kbd_buf)
        maxlen = (int)sizeof kbd_buf - 1;
    snprintf(kbd_buf, sizeof kbd_buf, "%s", initial ? initial : "");
    kbd_done = done;
    kbd_user = user;
    OpenKeyboard(title, kbd_buf, maxlen, KBD_NORMAL, kbd_handler);
}

void pf_keep_awake(int sec)
{
    BanSleep(sec);
}

#define DATA_DIR "/mnt/ext1/washi"
#define OLD_DATA_DIR "/mnt/ext1/openbook" /* до 0.1.0-beta проект назывался openbook */

const char *pf_data_dir(void)
{
    static int moved;
    if (!moved) {
        /* Первый запуск после переименования: настройки, места чтения, пароли, обложки — на новое место. */
        moved = 1;
        struct stat st;
        if (stat(DATA_DIR, &st) != 0 && stat(OLD_DATA_DIR, &st) == 0)
            rename(OLD_DATA_DIR, DATA_DIR);
    }
    return DATA_DIR;
}

/* Эти функции есть в inkview.h SDK 6.5, но берём их через dlsym: если в прошивке
 * их не окажется, приложение всё равно запустится. */
void pf_screen_info(char *buf, size_t len)
{
    void (*get_times)(unsigned short[16]) = (void (*)(unsigned short[16]))dlsym(RTLD_DEFAULT, "GetWaveformTimes");
    if (!get_times) {
        snprintf(buf, len, "GetWaveformTimes нет в libinkview");
        return;
    }
    unsigned short t[16];
    get_times(t);
    int n = snprintf(buf, len, "GetWaveformTimes:");
    for (int i = 0; i < 16 && n > 0 && (size_t)n < len; i++)
        n += snprintf(buf + n, len - (size_t)n, " %u", t[i]);
}

int pf_pending_input(void)
{
    static int (*pending)(void);
    static int looked_up;
    if (!looked_up) {
        pending = (int (*)(void))dlsym(RTLD_DEFAULT, "PendingHwEventsCount");
        looked_up = 1;
    }
    return pending ? pending() : -1;
}

const char *pf_update_mode_name(pf_update_mode mode)
{
    switch (mode) {
    case PF_UPDATE_FULL: return "FullUpdate";
    case PF_UPDATE_PARTIAL: return "PartialUpdate";
    case PF_UPDATE_FAST: return "DynamicUpdate";
    default: return mode < PF_UPDATE_MODES && extra_names[mode] ? extra_names[mode] : "?";
    }
}
