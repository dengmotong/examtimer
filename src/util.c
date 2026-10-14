/* util.c - configuration, logging and timing helpers for ExamTimer */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <mmsystem.h>

#include "examtimer.h"

/* ------------------------------------------------------------------ */
/* text encoding                                                        */
/*                                                                      */
/* Everything inside the program is UTF-8.  Windows hands us ANSI       */
/* (GBK on a Chinese XP) and the UI wants UTF-16, so we convert at the   */
/* boundaries instead of mixing code pages - that mixing is exactly what */
/* used to turn the status line into mojibake when the recording folder  */
/* had Chinese characters in it.                                         */
/* ------------------------------------------------------------------ */

int utf8_to_wide(const char *utf8, wchar_t *out, int outcap)
{
    int n;
    if (!utf8 || !out || outcap <= 0) return 0;
    out[0] = 0;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, outcap);
    if (n <= 0)
        n = MultiByteToWideChar(CP_ACP, 0, utf8, -1, out, outcap);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}

int wide_to_utf8(const wchar_t *w, char *out, int outcap)
{
    int n;
    if (!w || !out || outcap <= 0) return 0;
    out[0] = 0;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outcap, NULL, NULL);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}

int ansi_to_utf8(const char *ansi, char *out, int outcap)
{
    wchar_t w[2048];
    if (!ansi || !out || outcap <= 0) return 0;
    out[0] = 0;
    if (MultiByteToWideChar(CP_ACP, 0, ansi, -1, w, 2048) <= 0) return 0;
    return wide_to_utf8(w, out, outcap);
}

int utf8_to_ansi(const char *utf8, char *out, int outcap)
{
    wchar_t w[2048];
    int n;
    if (!utf8 || !out || outcap <= 0) return 0;
    out[0] = 0;
    if (utf8_to_wide(utf8, w, 2048) <= 0) return 0;
    n = WideCharToMultiByte(CP_ACP, 0, w, -1, out, outcap, NULL, NULL);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}

FILE *u8_fopen(const char *utf8_path, const char *mode)
{
    wchar_t wp[2048], wm[16];
    if (utf8_to_wide(utf8_path, wp, 2048) <= 0) return NULL;
    utf8_to_wide(mode, wm, 16);
    return _wfopen(wp, wm);
}

/* Windows hands us ANSI (GBK on a Chinese XP); the ini may be either.
 * Normalise once so the rest of the program only ever sees UTF-8. */
void normalize_path_utf8(char *str, int cap)
{
    wchar_t w[2048];
    char out[4096];
    int i, ascii = 1;

    if (!str || !str[0]) return;
    for (i = 0; str[i]; i++) {
        if ((unsigned char)str[i] >= 0x80) { ascii = 0; break; }
    }
    if (ascii) return;

    /* already valid UTF-8?  keep it */
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1, w, 2048) > 0) {
        if (wide_to_utf8(w, out, sizeof(out)) > 0) {
            strncpy(str, out, (size_t)cap - 1);
            str[cap - 1] = 0;
        }
        return;
    }
    if (MultiByteToWideChar(CP_ACP, 0, str, -1, w, 2048) > 0) {
        if (wide_to_utf8(w, out, sizeof(out)) > 0) {
            strncpy(str, out, (size_t)cap - 1);
            str[cap - 1] = 0;
        }
    }
}

int u8_mkdir(const char *utf8_path)
{
    wchar_t wp[2048];
    if (utf8_to_wide(utf8_path, wp, 2048) <= 0) return -1;
    if (CreateDirectoryW(wp, NULL)) return 0;
    return (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
}

/* create every missing component of a directory path */
int u8_mkdir_deep(const char *utf8_dir)
{
    char tmp[MAX_PATH_LONG];
    int i, len;
    strncpy(tmp, utf8_dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    len = (int)strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '\\' || tmp[i] == '/') {
            char save = tmp[i];
            tmp[i] = 0;
            if (strlen(tmp) > 2) u8_mkdir(tmp);
            tmp[i] = save;
        }
    }
    if (len > 2) u8_mkdir(tmp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* small string helpers                                                */
/* ------------------------------------------------------------------ */

void str_trim(char *s)
{
    char *p = s, *e;
    size_t n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    e = s + n;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
}

int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* ------------------------------------------------------------------ */
/* time helpers                                                        */
/* ------------------------------------------------------------------ */

static double g_qpc_freq = 0.0;

static void tick_init(void)
{
    static int done = 0;
    LARGE_INTEGER f;
    if (done) return;
    done = 1;
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0)
        g_qpc_freq = (double)f.QuadPart;
    else
        g_qpc_freq = 0.0;
}

static unsigned long now_ms(void)
{
    tick_init();
    if (g_qpc_freq > 0.0) {
        LARGE_INTEGER c;
        if (QueryPerformanceCounter(&c))
            return (unsigned long)((double)c.QuadPart * 1000.0 / g_qpc_freq);
    }
    return timeGetTime();
}

/* ------------------------------------------------------------------ */
/* timer state                                                         */
/* ------------------------------------------------------------------ */

void timer_reset(TimerState *t)
{
    t->running = 0;
    t->base_ms = 0;
    t->start_tick = 0;
    t->total_ms = 0;
}

void timer_start(TimerState *t)
{
    if (t->running) return;
    t->start_tick = now_ms();
    t->running = 1;
}

void timer_pause(TimerState *t)
{
    if (!t->running) return;
    t->base_ms += (long long)(now_ms() - t->start_tick);
    t->running = 0;
}

long long timer_elapsed_ms(TimerState *t)
{
    long long v = t->base_ms;
    if (t->running) v += (long long)(now_ms() - t->start_tick);
    t->total_ms = v;
    return v;
}

long long timer_elapsed_ms_static(const TimerState *t)
{
    long long v = t->base_ms;
    if (t->running) v += (long long)(now_ms() - t->start_tick);
    return v;
}

static void fmt_hms(long long ms, int granularity, char *out, int outcap)
{
    int h, m, s, frac = 0;
    long long sec;

    if (ms < 0) ms = 0;
    sec = ms / 1000;
    s = (int)(sec % 60);
    m = (int)((sec / 60) % 60);
    h = (int)(sec / 3600);

    switch (granularity) {
        case GRAN_HUNDREDTH: frac = (int)((ms / 10) % 100); break;
        case GRAN_TENTH:     frac = (int)((ms / 100) % 10); break;
        default:             frac = -1; break;
    }

    if (h > 99) h = 99;
    if (frac < 0)
        snprintf(out, (size_t)outcap, "%02d:%02d:%02d", h, m, s);
    else if (granularity == GRAN_HUNDREDTH)
        snprintf(out, (size_t)outcap, "%02d:%02d:%02d.%02d", h, m, s, frac);
    else
        snprintf(out, (size_t)outcap, "%02d:%02d:%02d.%d", h, m, s, frac);
}

void timer_format(long long ms, int granularity, char *out, int outcap)
{
    fmt_hms(ms, granularity, out, outcap);
}

void timer_format_countdown(long long remain_ms, int granularity, char *out, int outcap)
{
    if (remain_ms < 0) remain_ms = 0;
    fmt_hms(remain_ms, granularity, out, outcap);
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static char g_logfile[MAX_PATH_LONG] = "";

void app_set_logfile(const char *path)
{
    if (!path) { g_logfile[0] = 0; return; }
    strncpy(g_logfile, path, sizeof(g_logfile) - 1);
    g_logfile[sizeof(g_logfile) - 1] = 0;
    path_ensure_dir(g_logfile);
}

void app_log(const char *fmt, ...)
{
    FILE *f;
    SYSTEMTIME st;
    va_list ap;
    char u8[2048], ansi[4096];
    if (!g_logfile[0]) return;
    f = u8_fopen(g_logfile, "a");
    if (!f) return;
    GetLocalTime(&st);
    va_start(ap, fmt);
    vsnprintf(u8, sizeof(u8), fmt, ap);
    va_end(ap);
    utf8_to_ansi(u8, ansi, sizeof(ansi));      /* readable in XP notepad */
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ansi);
    fclose(f);
}

void cfg_log(const Config *c, const char *fmt, ...)
{
    FILE *f;
    SYSTEMTIME st;
    va_list ap;
    char u8[2048], ansi[4096];
    if (!c || !c->logfile[0]) return;
    f = u8_fopen(c->logfile, "a");
    if (!f) return;
    GetLocalTime(&st);
    va_start(ap, fmt);
    vsnprintf(u8, sizeof(u8), fmt, ap);
    va_end(ap);
    utf8_to_ansi(u8, ansi, sizeof(ansi));
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ansi);
    fclose(f);
}

void path_ensure_dir(const char *file)
{
    char dir[MAX_PATH_LONG];
    char *p;
    if (!file) return;
    strncpy(dir, file, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    p = strrchr(dir, '\\');
    if (!p) p = strrchr(dir, '/');
    if (!p) return;
    *p = 0;
    if (strlen(dir) < 2) return;
    u8_mkdir_deep(dir);
}

/* ------------------------------------------------------------------ */
/* strftime style expansion for output file names                      */
/* ------------------------------------------------------------------ */

const char *cfg_output_path(const Config *c, char *out, int outcap)
{
    if (!c->out_dir[0]) {
        strncpy(out, c->out_name, (size_t)outcap - 1);
        out[outcap - 1] = 0;
    } else {
        snprintf(out, (size_t)outcap, "%s\\%s", c->out_dir, c->out_name);
    }
    return out;
}

char *expand_timestamp(const char *fmt, char *out, int outcap)
{
    SYSTEMTIME st;
    struct tm tmv;
    char tmp[MAX_PATH_LONG];
    char res[MAX_PATH_LONG];
    int i = 0, o = 0;

    if (!fmt) { out[0] = 0; return out; }
    GetLocalTime(&st);
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = st.wYear - 1900;
    tmv.tm_mon  = st.wMonth - 1;
    tmv.tm_mday = st.wDay;
    tmv.tm_hour = st.wHour;
    tmv.tm_min  = st.wMinute;
    tmv.tm_sec  = st.wSecond;
    strftime(tmp, sizeof(tmp), fmt, &tmv);

    /* expand %N as a running counter to avoid collisions */
    while (tmp[i] && o < outcap - 1) {
        if (tmp[i] == '%' && (tmp[i + 1] == 'N' || tmp[i + 1] == 'n')) {
            static int counter = 0;
            char nb[16];
            snprintf(nb, sizeof(nb), "%d", ++counter);
            if (o + (int)strlen(nb) < outcap - 1) {
                strcpy(res + o, nb);
                o += (int)strlen(nb);
            }
            i += 2;
        } else {
            res[o++] = tmp[i++];
        }
    }
    res[o] = 0;
    strncpy(out, res, (size_t)outcap - 1);
    out[outcap - 1] = 0;
    return out;
}

/* ------------------------------------------------------------------ */
/* defaults                                                            */
/* ------------------------------------------------------------------ */

void cfg_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->cap_x = c->cap_y = c->cap_w = c->cap_h = 0;
    c->scale = 100;
    c->fps = 5;
    c->quality = 75;
    c->subsample = 1;
    c->captureblt = 0;   /* 1 forces a full redraw and makes the XP cursor jitter */
    c->skip_identical = 1;   /* identical frames reuse the previous JPEG */
    strcpy(c->out_dir, "record");
    strcpy(c->out_name, "exam_%Y%m%d_%H%M%S.avi");
    c->split_mb = 0;                 /* 0 = auto (1800 MB hard cap) */
    c->overwrite = 0;

    c->countdown = 0;
    c->countdown_seconds = 3600;
    c->granularity = GRAN_TENTH;

    c->overlay_enabled = 1;
    c->pos = POS_TOP_CENTER;
    c->offset_x = 0;
    c->offset_y = 0;
    c->fontsize = 44;
    c->fg_alpha = 255;
    c->bg_alpha = 224;
    c->fixed = 1;                    /* 固定：点击穿透、不可移动 */
    c->auto_topmost_ms = 500;
    c->show_label = 1;
    c->show_rec_dot = 1;
    c->show_border = 1;
    c->fg = RGB(255, 232, 32);
    c->bg = RGB(12, 20, 32);
    c->border = RGB(0, 200, 255);
    strcpy(c->label, "考核计时");

    c->burnin = 1;

    c->audio_enabled = 0;
    c->audio_device = -2;            /* auto: prefer 立体声混音 */
    c->audio_match[0] = 0;
    c->audio_rate = 44100;
    c->audio_channels = 2;
    c->audio_bits = 16;

    c->hk_toggle = VK_F8;            /* 开始/暂停 录像 */
    c->hk_timer = VK_F9;             /* 开始/暂停 计时 */
    c->hk_stop = VK_F10;             /* 停止并保存     */
    c->hk_overlay = VK_F11;          /* 显示/隐藏计时  */
    c->hk_click = VK_F12;            /* 固定/非固定    */
    c->hotkeys_enabled = 1;

    c->autostart = 0;    /* 由用户点击“开始录像” */
    c->duration_sec = 0;
    c->minimize_control = 0;
    c->tray = 1;
    c->show_elapsed_in_title = 1;
    c->logfile[0] = 0;

    c->cmd_shot = 0;
    c->shot_file[0] = 0;
    c->cmd_selftest = 0;
    c->cmd_help = 0;
    c->cmd_run_seconds = 0;
}

/* ------------------------------------------------------------------ */
/* colour / key parsing                                                */
/* ------------------------------------------------------------------ */

static COLORREF parse_color(const char *v, COLORREF def)
{
    int r = 0, g = 0, b = 0;
    const char *p = v;
    if (*p == '#') {
        unsigned int hex = 0;
        if (sscanf(p + 1, "%x", &hex) == 1) {
            r = (hex >> 16) & 0xff;
            g = (hex >> 8) & 0xff;
            b = hex & 0xff;
            return RGB(r, g, b);
        }
        return def;
    }
    if (sscanf(p, "%d,%d,%d", &r, &g, &b) == 3)
        return RGB(r, g, b);
    return def;
}

static int parse_key(const char *v)
{
    if (!v || !*v) return 0;
    if (str_ieq(v, "none") || str_ieq(v, "off") || strcmp(v, "0") == 0) return 0;
    if ((v[0] == 'F' || v[0] == 'f')) {
        int n = atoi(v + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }
    if (str_ieq(v, "F12")) return VK_F12;
    if (strlen(v) == 1) return toupper((unsigned char)v[0]);
    return 0;
}

static int parse_pos(const char *v)
{
    if (str_ieq(v, "topleft") || str_ieq(v, "left")) return POS_TOP_LEFT;
    if (str_ieq(v, "topright") || str_ieq(v, "right")) return POS_TOP_RIGHT;
    if (str_ieq(v, "custom")) return POS_CUSTOM;
    return POS_TOP_CENTER;
}

/* ------------------------------------------------------------------ */
/* INI                                                                 */
/* ------------------------------------------------------------------ */

static int cfg_set(Config *c, const char *section, const char *key, const char *val)
{
    if (str_ieq(section, "record") || str_ieq(section, "record")) {
        if (str_ieq(key, "output")) {   /* 兼容旧版：拆成目录 + 文件名 */
            const char *bs = strrchr(val, '\\');
            if (bs) {
                size_t n = (size_t)(bs - val);
                if (n >= sizeof(c->out_dir)) n = sizeof(c->out_dir) - 1;
                memcpy(c->out_dir, val, n);
                c->out_dir[n] = 0;
                strncpy(c->out_name, bs + 1, sizeof(c->out_name) - 1);
            } else {
                strncpy(c->out_name, val, sizeof(c->out_name) - 1);
            }
            return 1;
        }
        if (str_ieq(key, "out_dir") || str_ieq(key, "dir")) { strncpy(c->out_dir, val, sizeof(c->out_dir) - 1); return 1; }
        if (str_ieq(key, "out_name") || str_ieq(key, "prefix")) { strncpy(c->out_name, val, sizeof(c->out_name) - 1); return 1; }
        if (str_ieq(key, "audio")) {
            if (str_ieq(val, "off") || str_ieq(val, "none") || strcmp(val, "0") == 0) { c->audio_enabled = 0; c->audio_device = -1; }
            else if (str_ieq(val, "auto")) { c->audio_enabled = 1; c->audio_device = -2; }
            else if (str_ieq(val, "test")) { c->audio_enabled = 1; c->audio_device = -3; }
            else { c->audio_enabled = 1; c->audio_device = atoi(val); }
            return 1;
        }
        if (str_ieq(key, "audio_device")) { c->audio_device = atoi(val); c->audio_enabled = (c->audio_device != -1); return 1; }
        if (str_ieq(key, "audio_match")) { strncpy(c->audio_match, val, sizeof(c->audio_match) - 1); return 1; }
        if (str_ieq(key, "audio_rate")) { c->audio_rate = atoi(val); return 1; }
        if (str_ieq(key, "audio_channels")) { c->audio_channels = atoi(val); return 1; }
        if (str_ieq(key, "audio_bits")) { c->audio_bits = atoi(val); return 1; }
        if (str_ieq(key, "fps")) { c->fps = atoi(val); return 1; }
        if (str_ieq(key, "quality")) { c->quality = atoi(val); return 1; }
        if (str_ieq(key, "scale")) { c->scale = atoi(val); return 1; }
        if (str_ieq(key, "subsample")) { c->subsample = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "captureblt")) { c->captureblt = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "skip_identical")) { c->skip_identical = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "split_mb")) { c->split_mb = atoi(val); return 1; }
        if (str_ieq(key, "region")) {
            int x, y, w, h;
            if (sscanf(val, "%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
                c->cap_x = x; c->cap_y = y; c->cap_w = w; c->cap_h = h;
            }
            return 1;
        }
        if (str_ieq(key, "overwrite")) { c->overwrite = atoi(val) ? 1 : 0; return 1; }
    }
    if (str_ieq(section, "timer")) {
        if (str_ieq(key, "countdown")) { c->countdown = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "countdown_seconds")) { c->countdown_seconds = atoi(val); return 1; }
        if (str_ieq(key, "granularity")) {
            if (str_ieq(val, "hundredth")) c->granularity = GRAN_HUNDREDTH;
            else if (str_ieq(val, "tenth")) c->granularity = GRAN_TENTH;
            else if (str_ieq(val, "second")) c->granularity = GRAN_SECOND;
            else c->granularity = atoi(val);
            return 1;
        }
    }
    if (str_ieq(section, "overlay")) {
        if (str_ieq(key, "enabled")) { c->overlay_enabled = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "pos")) { c->pos = parse_pos(val); return 1; }
        if (str_ieq(key, "offset_x")) { c->offset_x = atoi(val); return 1; }
        if (str_ieq(key, "offset_y")) { c->offset_y = atoi(val); return 1; }
        if (str_ieq(key, "x")) { c->offset_x = atoi(val); c->pos = POS_CUSTOM; return 1; }
        if (str_ieq(key, "y")) { c->offset_y = atoi(val); c->pos = POS_CUSTOM; return 1; }
        if (str_ieq(key, "fontsize")) { c->fontsize = atoi(val); return 1; }
        if (str_ieq(key, "alpha")) {   /* 旧版：同时设置两者 */
            int a = atoi(val);
            if (a < 16) a = 16;
            if (a > 255) a = 255;
            c->fg_alpha = 255;
            c->bg_alpha = a;
            return 1;
        }
        if (str_ieq(key, "fg_alpha")) { c->fg_alpha = atoi(val); return 1; }
        if (str_ieq(key, "bg_alpha")) { c->bg_alpha = atoi(val); return 1; }
        if (str_ieq(key, "fixed")) { c->fixed = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "click_through")) { c->fixed = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "auto_topmost_ms")) { c->auto_topmost_ms = atoi(val); return 1; }
        if (str_ieq(key, "show_label")) { c->show_label = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "show_rec_dot")) { c->show_rec_dot = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "show_border")) { c->show_border = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "label")) { strncpy(c->label, val, sizeof(c->label) - 1); return 1; }
        if (str_ieq(key, "fg")) { c->fg = parse_color(val, c->fg); return 1; }
        if (str_ieq(key, "bg")) { c->bg = parse_color(val, c->bg); return 1; }
        if (str_ieq(key, "border")) { c->border = parse_color(val, c->border); return 1; }
        if (str_ieq(key, "burnin")) { c->burnin = atoi(val) ? 1 : 0; return 1; }
    }
    if (str_ieq(section, "hotkeys")) {
        if (str_ieq(key, "enabled")) { c->hotkeys_enabled = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "toggle")) { c->hk_toggle = parse_key(val); return 1; }
        if (str_ieq(key, "timer")) { c->hk_timer = parse_key(val); return 1; }
        if (str_ieq(key, "stop")) { c->hk_stop = parse_key(val); return 1; }
        if (str_ieq(key, "overlay")) { c->hk_overlay = parse_key(val); return 1; }
        if (str_ieq(key, "click_through")) { c->hk_click = parse_key(val); return 1; }
    }
    if (str_ieq(section, "app")) {
        if (str_ieq(key, "autostart")) { c->autostart = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "duration_sec")) { c->duration_sec = atoi(val); return 1; }
        if (str_ieq(key, "minimize_control")) { c->minimize_control = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "tray")) { c->tray = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "show_elapsed_in_title")) { c->show_elapsed_in_title = atoi(val) ? 1 : 0; return 1; }
        if (str_ieq(key, "logfile")) { strncpy(c->logfile, val, sizeof(c->logfile) - 1); return 1; }
    }
    return 0;
}

int cfg_load_ini(Config *c, const char *path)
{
    FILE *f;
    char line[2048];
    char section[64] = "record";
    int n = 0;

    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char *eq, *p;
        /* strip comments starting with ';' */
        p = strchr(line, ';');
        if (p) *p = 0;
        str_trim(line);
        if (!line[0]) continue;
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (e) {
                *e = 0;
                strncpy(section, line + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = 0;
                str_trim(section);
            }
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        {
            char *k = line, *v = eq + 1;
            str_trim(k);
            str_trim(v);
            if (cfg_set(c, section, k, v)) n++;
        }
    }
    fclose(f);
    return n;
}

/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

static const char *next_arg(char **argv, int *i, int argc, const char *def)
{
    if (*i + 1 < argc) { (*i)++; return argv[*i]; }
    return def;
}

/* Very small tokeniser that understands quoted arguments, used so the
 * command line can be handled without pulling in the CRT's splitter. */
static int split_args(const char *cmd, char **argv, int max)
{
    int n = 0;
    const char *p = cmd;
    static char buf[32][MAX_PATH_LONG];

    if (n < max) argv[n++] = (char *)"";     /* argv[0] */
    while (*p && n < max) {
        char *o = buf[n];
        int c = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && c < MAX_PATH_LONG - 1) o[c++] = *p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && c < MAX_PATH_LONG - 1) o[c++] = *p++;
        }
        o[c] = 0;
        argv[n++] = o;
    }
    return n;
}

int cfg_parse_cmdline(Config *c, const char *cmdline)
{
    char *argv[32];
    int argc, i;

    argc = split_args(cmdline, argv, 32);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (str_ieq(a, "--help") || str_ieq(a, "-h") || str_ieq(a, "/?")) { c->cmd_help = 1; }
        else if (str_ieq(a, "--selftest")) { c->cmd_selftest = 1; }
        else if (str_ieq(a, "--out") || str_ieq(a, "-o")) {
            const char *v = next_arg(argv, &i, argc, c->out_name);
            const char *bs = strrchr(v, '\\');
            if (bs) {
                size_t n = (size_t)(bs - v);
                if (n >= sizeof(c->out_dir)) n = sizeof(c->out_dir) - 1;
                memcpy(c->out_dir, v, n);
                c->out_dir[n] = 0;
                strncpy(c->out_name, bs + 1, sizeof(c->out_name) - 1);
            } else {
                strncpy(c->out_name, v, sizeof(c->out_name) - 1);
            }
        }
        else if (str_ieq(a, "--out-dir") || str_ieq(a, "--dir")) {
            strncpy(c->out_dir, next_arg(argv, &i, argc, c->out_dir), sizeof(c->out_dir) - 1);
        }
        else if (str_ieq(a, "--out-name")) {
            strncpy(c->out_name, next_arg(argv, &i, argc, c->out_name), sizeof(c->out_name) - 1);
        }
        else if (str_ieq(a, "--audio")) {
            const char *v = next_arg(argv, &i, argc, "auto");
            if (str_ieq(v, "off") || str_ieq(v, "none") || strcmp(v, "0") == 0) { c->audio_enabled = 0; c->audio_device = -1; }
            else if (str_ieq(v, "test")) { c->audio_enabled = 1; c->audio_device = -3; }
            else if (str_ieq(v, "auto")) { c->audio_enabled = 1; c->audio_device = -2; }
            else { c->audio_enabled = 1; c->audio_device = atoi(v); }
        }
        else if (str_ieq(a, "--audio-match")) {
            strncpy(c->audio_match, next_arg(argv, &i, argc, ""), sizeof(c->audio_match) - 1);
            c->audio_enabled = 1;
        }
        else if (str_ieq(a, "--no-audio")) { c->audio_enabled = 0; c->audio_device = -1; }
        else if (str_ieq(a, "--fixed")) { c->fixed = 1; }
        else if (str_ieq(a, "--no-fixed")) { c->fixed = 0; }
        else if (str_ieq(a, "--config") || str_ieq(a, "-c")) {
            const char *p = next_arg(argv, &i, argc, NULL);
            if (p) cfg_load_ini(c, p);
        }
        else if (str_ieq(a, "--fps")) { c->fps = atoi(next_arg(argv, &i, argc, "5")); }
        else if (str_ieq(a, "--quality") || str_ieq(a, "--q")) { c->quality = atoi(next_arg(argv, &i, argc, "75")); }
        else if (str_ieq(a, "--scale")) { c->scale = atoi(next_arg(argv, &i, argc, "100")); }
        else if (str_ieq(a, "--subsample")) { c->subsample = atoi(next_arg(argv, &i, argc, "1")) ? 1 : 0; }
        else if (str_ieq(a, "--no-captureblt")) { c->captureblt = 0; }
        else if (str_ieq(a, "--skip-identical")) { c->skip_identical = 1; }
        else if (str_ieq(a, "--split-mb")) { c->split_mb = atoi(next_arg(argv, &i, argc, "0")); }
        else if (str_ieq(a, "--region")) {
            int x, y, w, h;
            const char *p = next_arg(argv, &i, argc, NULL);
            if (p && sscanf(p, "%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
                c->cap_x = x; c->cap_y = y; c->cap_w = w; c->cap_h = h;
            }
        }
        else if (str_ieq(a, "--pos")) { c->pos = parse_pos(next_arg(argv, &i, argc, "topcenter")); }
        else if (str_ieq(a, "--fontsize")) { c->fontsize = atoi(next_arg(argv, &i, argc, "44")); }
        else if (str_ieq(a, "--alpha")) { c->bg_alpha = atoi(next_arg(argv, &i, argc, "224")); }
        else if (str_ieq(a, "--fg-alpha")) { c->fg_alpha = atoi(next_arg(argv, &i, argc, "255")); }
        else if (str_ieq(a, "--bg-alpha")) { c->bg_alpha = atoi(next_arg(argv, &i, argc, "224")); }
        else if (str_ieq(a, "--offset")) {
            int x, y;
            const char *p = next_arg(argv, &i, argc, NULL);
            if (p && sscanf(p, "%d,%d", &x, &y) == 2) { c->offset_x = x; c->offset_y = y; c->pos = POS_CUSTOM; }
        }
        else if (str_ieq(a, "--label")) {
            strncpy(c->label, next_arg(argv, &i, argc, ""), sizeof(c->label) - 1);
        }
        else if (str_ieq(a, "--fg")) { c->fg = parse_color(next_arg(argv, &i, argc, "#FFE820"), c->fg); }
        else if (str_ieq(a, "--bg")) { c->bg = parse_color(next_arg(argv, &i, argc, "#0C1420"), c->bg); }
        else if (str_ieq(a, "--no-overlay")) { c->overlay_enabled = 0; }
        else if (str_ieq(a, "--no-burnin")) { c->burnin = 0; }
        else if (str_ieq(a, "--no-clickthrough")) { c->fixed = 0; }
        else if (str_ieq(a, "--no-fixed-once")) { c->fixed = 0; }
        else if (str_ieq(a, "--no-tray")) { c->tray = 0; }
        else if (str_ieq(a, "--minimize")) { c->minimize_control = 1; }
        else if (str_ieq(a, "--no-hotkeys")) { c->hotkeys_enabled = 0; }
        else if (str_ieq(a, "--autostart")) { c->autostart = 1; }
        else if (str_ieq(a, "--no-autostart")) { c->autostart = 0; }
        else if (str_ieq(a, "--duration")) { c->duration_sec = atoi(next_arg(argv, &i, argc, "0")); }
        else if (str_ieq(a, "--run-seconds")) { c->cmd_run_seconds = atoi(next_arg(argv, &i, argc, "0")); c->autostart = 1; }
        else if (str_ieq(a, "--countdown")) { c->countdown = 1; c->countdown_seconds = atoi(next_arg(argv, &i, argc, "3600")); }
        else if (str_ieq(a, "--granularity")) {
            const char *p = next_arg(argv, &i, argc, "tenth");
            if (str_ieq(p, "hundredth")) c->granularity = GRAN_HUNDREDTH;
            else if (str_ieq(p, "second")) c->granularity = GRAN_SECOND;
            else c->granularity = GRAN_TENTH;
        }
        else if (str_ieq(a, "--shot")) {
            c->cmd_shot = 1;
            strncpy(c->shot_file, next_arg(argv, &i, argc, "shot.bmp"), sizeof(c->shot_file) - 1);
        }
        else if (str_ieq(a, "--log")) { strncpy(c->logfile, next_arg(argv, &i, argc, ""), sizeof(c->logfile) - 1); }
        else if (str_ieq(a, "--overwrite")) { c->overwrite = 1; }
    }

    /* sanity clamp */
    if (c->fps < 1) c->fps = 1;
    if (c->fps > 30) c->fps = 30;
    if (c->quality < 1) c->quality = 1;
    if (c->quality > 100) c->quality = 100;
    if (c->scale < 10) c->scale = 10;
    if (c->scale > 100) c->scale = 100;
    if (c->fontsize < 8) c->fontsize = 8;
    if (c->fontsize > 400) c->fontsize = 400;
    if (c->fg_alpha < 0) c->fg_alpha = 0;
    if (c->fg_alpha > 255) c->fg_alpha = 255;
    if (c->bg_alpha < 0) c->bg_alpha = 0;
    if (c->bg_alpha > 255) c->bg_alpha = 255;
    if (c->auto_topmost_ms < 0) c->auto_topmost_ms = 0;
    if (c->audio_rate < 8000 || c->audio_rate > 192000) c->audio_rate = 44100;
    if (c->audio_channels < 1 || c->audio_channels > 2) c->audio_channels = 2;
    if (c->audio_bits != 8) c->audio_bits = 16;
    if (!c->out_name[0]) strcpy(c->out_name, "exam_%Y%m%d_%H%M%S.avi");
    if (c->audio_device < -3) c->audio_device = -2;
    if (c->granularity < 0 || c->granularity > 2) c->granularity = GRAN_TENTH;
    return 0;
}
