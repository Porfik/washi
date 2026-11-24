/* Платформа ПК через SDL2: окно с экраном книги 1404×1872 в уменьшенном масштабе.
 *
 * Обновления e-ink эмулируются рамкой вокруг обновлённой области на время, которое
 * такое обновление занимает на книге (docs/device.md): красная — полное, синяя — частичное,
 * зелёная — быстрое.
 *
 * Переменные окружения:
 *   WASHI_DATA            папка данных (по умолчанию ./washi-data)
 *   WASHI_SCALE           масштаб окна (по умолчанию 0.45)
 *   WASHI_SCREENSHOT_DIR  сохранять экран после каждого обновления в DIR/NNNN.pgm
 *   WASHI_KEYS            сценарий нажатий, по одному на 300 мс: n — вперёд, p — назад,
 *                            N — пять раз вперёд подряд, m — меню, l/r — тап слева/справа,
 *                            @X,Y; — тап в точку экрана книги, !X,Y; — долгое касание,
 *                            < > ^ v — свайп влево, вправо, вверх, вниз по центру экрана,
 *                            ~X1,Y1,X2,Y2; — провести пальцем (нажать, вести, отпустить),
 *                            координаты сценария — по логическому экрану (с учётом поворота),
 *                            w — пауза, q — выход.
 *                            С SDL_VIDEODRIVER=dummy работает без окна.
 * Клавиши: →, PgDn, пробел — вперёд; ←, PgUp — назад; M, Esc — меню; Q — выход.
 * Мышь: щелчок — касание, протяжка — свайп, правая кнопка — долгое касание.
 */
#include "platform.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../base/clock.h"
#include "../base/log.h"
#include "rotate.h"

#define SCREEN_W 1404
#define SCREEN_H 1872
/* Как на книге: шаг строки больше ширины (1408), чтобы ошибки шага было видно и здесь. */
#define SCREEN_STRIDE ((SCREEN_W + 15) & ~15)
#define SCRIPT_STEP_MS 300

static pf_handler app_handler;
static void *app_user;
static pf_screen screen;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static Uint32 ev_timer, ev_flash_end, ev_script, ev_aux;
static SDL_TimerID timer_id, aux_id;
static int quit_requested;

static SDL_Rect flash_rect;
static pf_update_mode flash_mode;
static int flash_active;

static rotator rot; /* логический экран в альбомной ориентации */
static const char *screenshot_dir;
static int screenshot_no;
static const char *script;

static void dispatch(pf_event_type type, pf_key key, int raw_key, int x, int y)
{
    pf_event ev = { type, key, raw_key, x, y, clock_ms() };
    app_handler(&ev, app_user);
}

static Uint32 push_event(Uint32 interval, void *param)
{
    (void)interval;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = (Uint32)(uintptr_t)param;
    SDL_PushEvent(&e);
    return 0;
}

static void present(void)
{
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    if (flash_active) {
        static const SDL_Color colors[] = { { 220, 0, 0, 255 }, { 0, 80, 220, 255 }, { 0, 160, 0, 255 } };
        SDL_Color c = colors[flash_mode];
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        for (int i = 0; i < 6; i++) {
            SDL_Rect r = { flash_rect.x + i, flash_rect.y + i, flash_rect.w - 2 * i, flash_rect.h - 2 * i };
            SDL_RenderDrawRect(renderer, &r);
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    }
    SDL_RenderPresent(renderer);
}

static void save_screenshot(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%04d.pgm", screenshot_dir, ++screenshot_no);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P5\n%d %d\n255\n", screen.width, screen.height);
    for (int y = 0; y < screen.height; y++)
        fwrite(screen.pixels + (size_t)y * screen.stride, 1, (size_t)screen.width, f);
    fclose(f);
}

static pf_key map_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RIGHT:
    case SDLK_PAGEDOWN:
    case SDLK_SPACE: return PF_KEY_NEXT;
    case SDLK_LEFT:
    case SDLK_PAGEUP: return PF_KEY_PREV;
    case SDLK_m:
    case SDLK_ESCAPE: return PF_KEY_MENU;
    case SDLK_h:
    case SDLK_HOME: return PF_KEY_HOME;
    default: return PF_KEY_UNKNOWN;
    }
}

static void run_script_step(void)
{
    if (!script || !*script)
        return;
    char c = *script++;
    switch (c) {
    case 'n':
    case 'p':
    case 'm':
    case 'h': {
        pf_key k = c == 'n' ? PF_KEY_NEXT : c == 'p' ? PF_KEY_PREV : c == 'h' ? PF_KEY_HOME : PF_KEY_MENU;
        dispatch(PF_EVENT_KEY_DOWN, k, c, 0, 0);
        dispatch(PF_EVENT_KEY_UP, k, c, 0, 0);
        break;
    }
    case 'N':
        for (int i = 0; i < 5; i++) {
            dispatch(PF_EVENT_KEY_DOWN, PF_KEY_NEXT, c, 0, 0);
            dispatch(PF_EVENT_KEY_UP, PF_KEY_NEXT, c, 0, 0);
        }
        break;
    case 'l':
    case 'r': {
        int x = c == 'l' ? SCREEN_W / 6 : SCREEN_W * 5 / 6;
        dispatch(PF_EVENT_POINTER_DOWN, PF_KEY_UNKNOWN, 0, x, SCREEN_H / 2);
        dispatch(PF_EVENT_POINTER_UP, PF_KEY_UNKNOWN, 0, x, SCREEN_H / 2);
        break;
    }
    case '@': {
        int x = 0, y = 0, used = 0;
        if (sscanf(script, "%d,%d;%n", &x, &y, &used) == 2 && used > 0) {
            script += used;
            dispatch(PF_EVENT_POINTER_DOWN, PF_KEY_UNKNOWN, 0, x, y);
            dispatch(PF_EVENT_POINTER_UP, PF_KEY_UNKNOWN, 0, x, y);
        }
        break;
    }
    case '!': {
        int x = 0, y = 0, used = 0;
        if (sscanf(script, "%d,%d;%n", &x, &y, &used) == 2 && used > 0) {
            script += used;
            dispatch(PF_EVENT_POINTER_DOWN, PF_KEY_UNKNOWN, 0, x, y);
            dispatch(PF_EVENT_POINTER_LONG, PF_KEY_UNKNOWN, 0, x, y);
            dispatch(PF_EVENT_POINTER_UP, PF_KEY_UNKNOWN, 0, x, y);
        }
        break;
    }
    case '~': {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0, used = 0;
        if (sscanf(script, "%d,%d,%d,%d;%n", &x1, &y1, &x2, &y2, &used) == 4 && used > 0) {
            script += used;
            dispatch(PF_EVENT_POINTER_DOWN, PF_KEY_UNKNOWN, 0, x1, y1);
            for (int i = 1; i <= 4; i++)
                dispatch(PF_EVENT_POINTER_MOVE, PF_KEY_UNKNOWN, 0, x1 + (x2 - x1) * i / 4, y1 + (y2 - y1) * i / 4);
            dispatch(PF_EVENT_POINTER_UP, PF_KEY_UNKNOWN, 0, x2, y2);
        }
        break;
    }
    case '<':
    case '>':
    case '^':
    case 'v': {
        int cx = SCREEN_W / 2, cy = SCREEN_H / 2, d = SCREEN_W / 3;
        int dx = c == '<' ? -d : c == '>' ? d : 0, dy = c == '^' ? -d : c == 'v' ? d : 0;
        dispatch(PF_EVENT_POINTER_DOWN, PF_KEY_UNKNOWN, 0, cx - dx / 2, cy - dy / 2);
        dispatch(PF_EVENT_POINTER_UP, PF_KEY_UNKNOWN, 0, cx + dx / 2, cy + dy / 2);
        break;
    }
    case 'q':
        quit_requested = 1;
        break;
    }
    SDL_AddTimer(SCRIPT_STEP_MS, push_event, (void *)(uintptr_t)ev_script);
}

int pf_run(pf_handler handler, void *user)
{
    app_handler = handler;
    app_user = user;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    const char *scale_env = getenv("WASHI_SCALE");
    float scale = scale_env ? (float)atof(scale_env) : 0.45f;
    window = SDL_CreateWindow("washi", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              (int)(SCREEN_W * scale), (int)(SCREEN_H * scale), SDL_WINDOW_RESIZABLE);
    renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H)
                       : NULL;
    if (!texture) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);

    screen.width = SCREEN_W;
    screen.height = SCREEN_H;
    screen.stride = SCREEN_STRIDE;
    screen.pixels = malloc((size_t)SCREEN_STRIDE * SCREEN_H);
    memset(screen.pixels, 255, (size_t)SCREEN_STRIDE * SCREEN_H);

    ev_timer = SDL_RegisterEvents(4);
    ev_flash_end = ev_timer + 1;
    ev_script = ev_timer + 2;
    ev_aux = ev_timer + 3;

    screenshot_dir = getenv("WASHI_SCREENSHOT_DIR");
    if (screenshot_dir)
        mkdir(screenshot_dir, 0755);
    script = getenv("WASHI_KEYS");

    dispatch(PF_EVENT_START, PF_KEY_UNKNOWN, 0, 0, 0);
    dispatch(PF_EVENT_SHOW, PF_KEY_UNKNOWN, 0, 0, 0);
    if (script)
        SDL_AddTimer(SCRIPT_STEP_MS, push_event, (void *)(uintptr_t)ev_script);

    SDL_Event e;
    while (!quit_requested && SDL_WaitEvent(&e)) {
        if (e.type == SDL_QUIT) {
            break;
        } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q)
                break;
            pf_event_type t = e.type == SDL_KEYUP     ? PF_EVENT_KEY_UP
                              : e.key.repeat ? PF_EVENT_KEY_REPEAT
                                             : PF_EVENT_KEY_DOWN;
            dispatch(t, map_key(e.key.keysym.sym), e.key.keysym.sym, 0, 0);
        } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
            int x = e.button.x, y = e.button.y;
            rot_touch(&rot, &screen, &x, &y); /* мышь — по физическому экрану */
            dispatch(e.type == SDL_MOUSEBUTTONDOWN ? PF_EVENT_POINTER_DOWN : PF_EVENT_POINTER_UP,
                     PF_KEY_UNKNOWN, 0, x, y);
            /* Правая кнопка мыши — долгое касание. */
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT)
                dispatch(PF_EVENT_POINTER_LONG, PF_KEY_UNKNOWN, 0, x, y);
        } else if (e.type == SDL_MOUSEMOTION && e.motion.state) {
            int x = e.motion.x, y = e.motion.y;
            rot_touch(&rot, &screen, &x, &y);
            dispatch(PF_EVENT_POINTER_MOVE, PF_KEY_UNKNOWN, 0, x, y);
        } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_EXPOSED) {
            present();
        } else if (e.type == ev_timer) {
            dispatch(PF_EVENT_TIMER, PF_KEY_UNKNOWN, 0, 0, 0);
        } else if (e.type == ev_aux) {
            dispatch(PF_EVENT_AUX_TIMER, PF_KEY_UNKNOWN, 0, 0, 0);
        } else if (e.type == ev_flash_end) {
            flash_active = 0;
            present();
        } else if (e.type == ev_script) {
            run_script_step();
        }
    }
    dispatch(PF_EVENT_EXIT, PF_KEY_UNKNOWN, 0, 0, 0);

    free(screen.pixels);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

void pf_quit(void)
{
    quit_requested = 1;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_USEREVENT; /* разбудить SDL_WaitEvent */
    SDL_PushEvent(&e);
}

pf_screen *pf_get_screen(void)
{
    return rot_screen(&rot, &screen);
}

void pf_set_orientation(int orient)
{
    rot_set(&rot, orient, &screen);
}

int pf_get_orientation(void)
{
    return rot.orient;
}

/* Длительность обновлений на книге (docs/device.md). */
static const int duration_ms[] = { 630, 440, 250 };

void pf_update(int x, int y, int w, int h, pf_update_mode mode)
{
    if (rot.orient) {
        int o[4];
        rot_flush(&rot, &screen, x, y, w, h, o);
        x = o[0], y = o[1], w = o[2], h = o[3];
        if (w <= 0 || h <= 0)
            return;
    }
    if (mode == PF_UPDATE_FULL_HQ)
        mode = PF_UPDATE_FULL;
    else if (mode > PF_UPDATE_FAST)
        mode = PF_UPDATE_PARTIAL; /* на ПК — только три основных */
    if (mode == PF_UPDATE_FULL) {
        x = y = 0;
        w = screen.width;
        h = screen.height;
    }
    SDL_Rect r = { x, y, w, h };
    void *tex_pixels;
    int tex_pitch;
    if (SDL_LockTexture(texture, &r, &tex_pixels, &tex_pitch) == 0) {
        for (int row = 0; row < h; row++) {
            const uint8_t *src = screen.pixels + (size_t)(y + row) * screen.stride + x;
            Uint32 *dst = (Uint32 *)((uint8_t *)tex_pixels + (size_t)row * tex_pitch);
            for (int col = 0; col < w; col++)
                dst[col] = 0xff000000u | src[col] * 0x010101u;
        }
        SDL_UnlockTexture(texture);
    }
    flash_rect = r;
    flash_mode = mode;
    flash_active = 1;
    present();
    SDL_AddTimer((Uint32)duration_ms[mode], push_event, (void *)(uintptr_t)ev_flash_end);
    if (screenshot_dir)
        save_screenshot();
}

void pf_set_timer(int ms)
{
    if (timer_id)
        SDL_RemoveTimer(timer_id);
    timer_id = SDL_AddTimer((Uint32)ms, push_event, (void *)(uintptr_t)ev_timer);
}

void pf_set_aux_timer(int ms)
{
    if (aux_id)
        SDL_RemoveTimer(aux_id);
    aux_id = SDL_AddTimer((Uint32)ms, push_event, (void *)(uintptr_t)ev_aux);
}

int pf_battery(int *charging)
{
    if (charging)
        *charging = 0;
    const char *v = getenv("WASHI_BATTERY");
    return v ? atoi(v) : 76;
}

int pf_launch(const char *path)
{
    fprintf(stderr, "pf_launch: %s (на ПК не запускается)\n", path);
    return -1;
}

void pf_home_started(void)
{
    fprintf(stderr, "pf_home_started (на ПК монитора прошивки нет)\n");
}

static int fw_statistics = 1;

int pf_fw_statistics(void)
{
    return fw_statistics;
}

void pf_set_fw_statistics(int on)
{
    fw_statistics = on;
}

int pf_open_book(const char *path)
{
    fprintf(stderr, "pf_open_book: %s (на ПК штатной читалки нет)\n", path);
    return -1;
}

static int light_b = 40, light_w = 30;

int pf_light_get(int *brightness, int *warmth)
{
    *brightness = light_b;
    *warmth = light_w;
    return 0;
}

void pf_light_set(int brightness, int warmth)
{
    light_b = brightness;
    if (warmth >= 0)
        light_w = warmth;
}

static int wifi_on = 1;

int pf_wifi_get(void)
{
    return wifi_on;
}

void pf_wifi_set(int on)
{
    wifi_on = on;
}

/* На ПК — текст из WASHI_KEYBOARD (для сценариев), иначе отмена. Несколько вводов подряд
 * (адрес, логин, пароль) — через «|»: каждый вызов берёт следующий, пустой — отмена. */
void pf_keyboard(const char *title, const char *initial, int maxlen, pf_keyboard_done done, void *user)
{
    (void)initial;
    static int used; /* сколько вводов уже отдали */
    const char *text = getenv("WASHI_KEYBOARD");
    char buf[1024] = "";
    for (int i = 0; text && i < used; i++) {
        text = strchr(text, '|');
        if (text)
            text++;
    }
    used++;
    if (text) {
        size_t n = strcspn(text, "|");
        if (maxlen > 0 && n > (size_t)maxlen)
            n = (size_t)maxlen;
        if (n >= sizeof buf)
            n = sizeof buf - 1;
        memcpy(buf, text, n);
        buf[n] = 0;
    }
    /* Сам текст в лог не пишем: это может быть пароль. */
    log_msg("клавиатура «%s»: %s", title ? title : "", *buf ? "введено" : "отмена");
    done(*buf ? buf : NULL, user);
}

/* На ПК — machine-id системы (в контейнере его может не быть — тогда -1). */
int pf_device_secret(char *buf, size_t len)
{
    FILE *f = fopen("/etc/machine-id", "r");
    if (!f)
        return -1;
    int ok = fgets(buf, (int)len, f) != NULL;
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return ok && *buf ? 0 : -1;
}

/* WASHI_NET: пусто — сеть есть; slow — подключение 3 с; fail — не подключается. */
static double net_t0;

int pf_net_start(void)
{
    const char *m = getenv("WASHI_NET");
    if (!m || !*m)
        return 1;
    net_t0 = clock_ms();
    return 0;
}

int pf_net_poll(void)
{
    const char *m = getenv("WASHI_NET");
    if (!m || !*m)
        return 1;
    if (clock_ms() - net_t0 < (strcmp(m, "fail") ? 3000 : 2000))
        return 0;
    return strcmp(m, "fail") ? 1 : -1;
}

int pf_net_connect(void)
{
    const char *m = getenv("WASHI_NET");
    return m && !strcmp(m, "fail") ? -1 : 0;
}

void pf_keep_awake(int sec)
{
    (void)sec;
}

const char *pf_data_dir(void)
{
    const char *dir = getenv("WASHI_DATA");
    return dir ? dir : "washi-data";
}

void pf_screen_info(char *buf, size_t len)
{
    snprintf(buf, len, "SDL, имитация обновления: full %d, partial %d, fast %d ms", duration_ms[0],
             duration_ms[1], duration_ms[2]);
}

int pf_pending_input(void)
{
    return -1;
}

const char *pf_update_mode_name(pf_update_mode mode)
{
    switch (mode) {
    case PF_UPDATE_FULL: return "full";
    case PF_UPDATE_PARTIAL: return "partial";
    case PF_UPDATE_FAST: return "fast";
    case PF_UPDATE_FULL_HQ: return "full-hq";
    default: return "other";
    }
}

int pf_update_mode_available(pf_update_mode mode)
{
    return mode == PF_UPDATE_FULL || mode == PF_UPDATE_PARTIAL || mode == PF_UPDATE_FAST;
}

void pf_wait_update(void)
{
}
