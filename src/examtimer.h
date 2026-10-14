/* examtimer.h - shared types for ExamTimer (考核计时录屏器)
 *
 * Target: Windows XP SP3 (and everything newer).  Pure Win32 + C99,
 * no runtimes to install, no gdiplus, no DirectX.
 */
#ifndef EXAMTIMER_H
#define EXAMTIMER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define APP_NAME     "考核计时录屏器"
#define APP_NAME_EN  "ExamTimer"
#define APP_VERSION  "2.0.0"

#define MAX_PATH_LONG 1024

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

enum {
    POS_TOP_LEFT = 0,
    POS_TOP_CENTER = 1,
    POS_TOP_RIGHT = 2,
    POS_CUSTOM = 3
};

enum {
    GRAN_HUNDREDTH = 0,   /* 00:00:00.00 */
    GRAN_TENTH     = 1,   /* 00:00:00.0  */
    GRAN_SECOND    = 2    /* 00:00:00    */
};

typedef struct {
    /* ---- capture ---- */
    int  cap_x, cap_y, cap_w, cap_h;  /* 0,0,0,0 = whole primary screen    */
    int  scale;                       /* 10..100 (%)                       */
    int  fps;                         /* 1..30                             */
    int  quality;                     /* 1..100                            */
    int  subsample;                   /* 1 = 4:2:0 (smaller), 0 = 4:4:4    */
    int  captureblt;                  /* 1 = also capture layered windows  */
    int  skip_identical;              /* 1 = do not re-encode identical frames */
    char out_dir[MAX_PATH_LONG];      /* where recordings are written (UTF-8) */
    char out_name[128];               /* file name template (strftime ok)  */
    int  split_mb;                    /* split AVI at N megabytes (0=off)  */
    int  overwrite;                   /* 1 = overwrite existing output     */

    /* ---- timer ---- */
    int  countdown;                   /* 1 = count down                    */
    int  countdown_seconds;           /* used when countdown = 1           */
    int  granularity;                 /* GRAN_*                            */

    /* ---- audio ---- */
    int  audio_enabled;               /* 1 = record sound                  */
    int  audio_device;                /* -1 off, -2 auto(立体声混音), -3 synthetic test, >=0 index */
    char audio_match[64];             /* pick the first device whose name contains this */
    int  audio_rate, audio_channels, audio_bits;

    /* ---- overlay ---- */
    int  overlay_enabled;
    int  pos;                         /* POS_*                             */
    int  offset_x, offset_y;          /* extra margin from the anchor      */
    int  fontsize;                    /* logical units (px at 96 dpi)      */
    int  fg_alpha;                    /* 文字/边框/红点 不透明度 0..255    */
    int  bg_alpha;                    /* 背景底板 不透明度 0..255          */
    int  fixed;                       /* 1 = 固定(点击穿透/不可移动)       */
    int  auto_topmost_ms;             /* re-assert topmost every N ms      */
    int  show_label;
    int  show_rec_dot;
    int  show_border;
    COLORREF fg, bg, border;
    char label[128];

    /* ---- burn-in ---- */
    int  burnin;                      /* 1 = draw the clock into the video */

    /* ---- hotkeys ---- */
    int  hk_toggle;                   /* 开始/暂停 录像                    */
    int  hk_timer;                    /* 开始/暂停 计时                    */
    int  hk_stop;                     /* 停止并保存                        */
    int  hk_overlay;                  /* show / hide the overlay           */
    int  hk_click;                    /* 固定 / 非固定                     */
    int  hotkeys_enabled;

    /* ---- misc ---- */
    int  autostart;                   /* start recording immediately       */
    int  duration_sec;                /* auto-stop after N seconds (0=off) */
    int  minimize_control;            /* start with the控制面板 minimised */
    int  tray;                        /* show a tray icon                  */
    int  show_elapsed_in_title;
    char logfile[MAX_PATH_LONG];

    /* ---- command line overrides (not persisted) ---- */
    int  cmd_shot;                    /* --shot given                      */
    char shot_file[MAX_PATH_LONG];
    int  cmd_selftest;
    int  cmd_help;
    int  cmd_run_seconds;             /* --run-seconds N (self test)       */
} Config;

/* ------------------------------------------------------------------ */
/* timer                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int           running;
    long long     base_ms;      /* accumulated time while paused        */
    unsigned long start_tick;   /* GetTickCount when started            */
    long long     total_ms;     /* elapsed at the moment of the call    */
} TimerState;

/* ------------------------------------------------------------------ */
/* visual style shared by the on-screen overlay and the burn-in        */
/* ------------------------------------------------------------------ */

typedef struct {
    int   fontsize;
    int   fg_alpha;          /* 文字层不透明度 0..255                  */
    int   bg_alpha;          /* 背景层不透明度 0..255                  */
    int   show_label;
    int   show_rec_dot;
    int   show_border;
    COLORREF fg, bg, border;
    char  label[128];
} TimerStyle;

/* ------------------------------------------------------------------ */
/* modules                                                             */
/* ------------------------------------------------------------------ */

/* --- util.c --- */
void  cfg_defaults(Config *c);
int   cfg_load_ini(Config *c, const char *path);
int   cfg_parse_cmdline(Config *c, const char *cmdline);
void  cfg_log(const Config *c, const char *fmt, ...);
void  app_set_logfile(const char *path);
void  app_log(const char *fmt, ...);
char *expand_timestamp(const char *fmt, char *out, int outcap);
const char *cfg_output_path(const Config *c, char *out, int outcap);
void  str_trim(char *s);
int   str_ieq(const char *a, const char *b);
void  path_ensure_dir(const char *file);

/* --- timer --- */
void      timer_reset(TimerState *t);
void      timer_start(TimerState *t);
void      timer_pause(TimerState *t);
long long timer_elapsed_ms(TimerState *t);
long long timer_elapsed_ms_static(const TimerState *t);
void      timer_format(long long ms, int granularity, char *out, int outcap);
void      timer_format_countdown(long long remain_ms, int granularity, char *out, int outcap);

/* control panel button ids (the overlay context menu reuses these) */
#ifndef IDC_REC
#define IDC_REC        1001
#define IDC_STOP       1002
#define IDC_FOLDER     1003
#define IDC_OVERLAY    1004
#define IDC_FIXED      1005
#define IDC_QUIT       1006
#define IDC_TIMER      1007
#define IDC_RESET      1008
#define IDC_SETTINGS   1009
#endif

/* menu / notification command ids */
#define IDM_SHOW_OVERLAY   40001
#define IDM_TOGGLE_CLICK   40002
#define IDM_START_PAUSE    40003
#define IDM_STOP_SAVE      40004
#define IDM_SHOW_CONTROL   40005
#define IDM_QUIT           40006
#define IDM_OPEN_OUTPUT    40007
#define WM_APP_TRAY        (WM_APP + 1)
#define WM_APP_STATUS      (WM_APP + 2)

enum { TRAY_ID = 1 };

/* --- encoding helpers (util.c): internally everything is UTF-8 --- */
int  utf8_to_wide(const char *utf8, wchar_t *out, int outcap);
int  ansi_to_utf8(const char *ansi, char *out, int outcap);
int  wide_to_utf8(const wchar_t *w, char *out, int outcap);
int  utf8_to_ansi(const char *utf8, char *out, int outcap);
FILE *u8_fopen(const char *utf8_path, const char *mode);   /* wide fopen */
int  u8_mkdir(const char *utf8_path);
int  u8_mkdir_deep(const char *utf8_dir);
/* make sure a string coming from Windows/ini/hard disk is UTF-8 */
void normalize_path_utf8(char *s, int cap);

/* --- audio.c --- */
typedef struct {
    int  active;
    int  device;
    int  rate, channels, bits;
    long long bytes;
    char name[128];
    char message[128];
} AudioStatus;

int  audio_device_count(void);
int  audio_device_name_at(int index, char *out, int outcap);   /* UTF-8 */
int  audio_init(const Config *cfg);
void audio_shutdown(void);
int  audio_start(void);
void audio_pause(int paused);
int  audio_read(unsigned char *buf, int maxbytes);             /* drains PCM */
long long audio_total_bytes(void);
void audio_status(AudioStatus *out);
const char *audio_format_desc(void);
int  audio_selftest(char *msg, int msgcap);
void audio_synth_fill(int ms);   /* synthetic source pump (test only) */

/* --- overlay.c --- */
int  overlay_create(HINSTANCE hi, const TimerStyle *style, const Config *cfg);
void overlay_destroy(void);
HWND overlay_hwnd(void);
void overlay_set_text(const char *text);
void overlay_set_recording(int on);
int  overlay_visible(void);
void overlay_show(int on);
void overlay_set_fixed(int on);
int  overlay_fixed(void);
void overlay_tick_topmost(void);
void overlay_move_to(int pos, int offx, int offy, int screen_w, int screen_h);
void overlay_set_owner(HWND h);
int  overlay_rect(RECT *r);            /* screen coordinates            */
/* report the live geometry so drag/resize can be saved back to the config */
void overlay_get_geometry(int *pos, int *offx, int *offy, int *fontsize);
void overlay_set_fontsize(int fontsize);
void overlay_begin_menu(void);         /* drop topmost so the menu is visible */
void overlay_end_menu(void);
void overlay_style(TimerStyle *out);
void overlay_apply(const TimerStyle *st, const Config *cfg);

/* shared painter: draws the timer box into an arbitrary DC.
 * (x,y) is the top-left corner in the target DC's coordinates.        */
void timer_box_size(HDC hdc, const TimerStyle *st, const char *text,
                    int recording, int *w, int *h);
void timer_box_paint(HDC hdc, const TimerStyle *st, const char *text,
                     int recording, int x, int y, int w, int h, int solid);

/* Render the timer box into a premultiplied BGRA buffer (caller frees).
 * This is what makes independent text/background transparency possible. */
unsigned char *timer_box_render_argb(const TimerStyle *st, const char *text,
                                     int recording, int *w, int *h);
/* Alpha-composite such a buffer onto a BGRA frame (used for the burn-in). */
void timer_box_blit_argb(unsigned char *dst, int dst_stride, int dst_w, int dst_h,
                         const unsigned char *argb, int aw, int ah, int x, int y);

/* anchor helper shared by the overlay and the burn-in:
 * computes the screen position of a box according to cfg->pos/offsets  */
void timer_box_anchor(const Config *cfg, int box_w, int box_h, int *x, int *y);

/* Convert a user visible string to UTF-16.
 * Tries UTF-8 first and falls back to the system ANSI code page, so both
 * UTF-8 ini files and GBK strings coming back from the OS render correctly. */
int  text_to_wide(const char *s, wchar_t *out, int outcap);

/* --- recorder.c --- */
typedef struct {
    int      active;          /* recording in progress                     */
    int      paused;
    int      frames;
    long long bytes;
    int      parts;
    int      fps_actual_x10;
    char     file[MAX_PATH_LONG];
    char     message[256];
    int      error;
    int      elapsed_sec;
    int      capture_w, capture_h;
    int      encode_ms_last;
    int      audio_bytes;
    int      audio_active;
} RecStatus;

int  recorder_start(const Config *cfg, const TimerStyle *style, const char *initial_text,
                    const char *output_path, int with_audio);
void recorder_stop(void);
void recorder_set_paused(int paused);
int  recorder_set_timer_text(const char *text);
void recorder_get_status(RecStatus *out);
/* one-shot BMP screenshot helper (used by --shot and by the tests) */
int  capture_screen_to_bmp(const char *file, int x, int y, int w, int h,
                           char *err, int errcap);

/* --- main.c helpers exposed to the recorder --- */
void app_request_stop(const char *reason);

#endif /* EXAMTIMER_H */
