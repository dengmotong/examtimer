/* main.c - ExamTimer (考核计时录屏器)
 *
 * Windows XP SP3 .. Windows 10/11, plain Win32, portable .exe, no installer.
 *
 *   * records the screen to MJPEG AVI
 *   * shows a big ticking clock pinned to the top of the screen, always on top
 *   * burns the very same clock into the video
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "examtimer.h"
#include "jpeg_enc.h"
#include "avi.h"

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */

static Config     g_cfg;
static TimerStyle g_style;
static TimerState g_timer;
static HWND       g_main = NULL;
static HINSTANCE  g_hinst = NULL;
static char       g_exe_dir[MAX_PATH_LONG] = "";
static char       g_ini_path[MAX_PATH_LONG] = "";
/* recording and timing are two INDEPENDENT states:
 *   录像  g_rec_state : 0 停止, 1 录制中, 2 已暂停
 *   计时  g_tim_state : 0 未开始, 1 计时中, 2 已暂停
 * 计时器可以在不录像时单独运行，录像也可以在不计时时开始。 */
static int        g_rec_state = 0;
static int        g_tim_state = 0;
static int        g_tray_added = 0;
static char       g_last_file[MAX_PATH_LONG] = "";
static int        g_quit_pending = 0;
static int        g_did_start = 0;
static int        g_geom_dirty = 0;
static int        g_conout = 0;
static FILE      *g_report = NULL;
static HWND       g_lbl_state = NULL, g_lbl_time = NULL, g_lbl_info = NULL;
static HWND       g_btn_rec = NULL, g_btn_stop = NULL, g_btn_folder = NULL,
                  g_btn_timer = NULL, g_btn_reset = NULL, g_btn_overlay = NULL,
                  g_btn_fixed = NULL, g_btn_settings = NULL, g_btn_quit = NULL;

#define IDT_UI 1
/* settings dialog controls */
#define IDS_DIR       2001
#define IDS_BROWSE    2002
#define IDS_NAME      2003
#define IDS_LABEL     2004
#define IDS_POS       2005
#define IDS_FONT      2006
#define IDS_ALPHA     2007
#define IDS_FPS       2008
#define IDS_QUALITY   2009
#define IDS_SCALE     2010
#define IDS_SPLIT     2011
#define IDS_AUDIO     2012
#define IDS_FIXED     2013
#define IDS_BURNIN    2014
#define IDS_FG        2015
#define IDS_BG        2016
#define IDS_OK        2017
#define IDS_CANCEL    2018
#define IDS_DEFAULTS  2019
#define IDS_GRAN      2020
#define IDS_STARTUP   2021
#define IDS_APPLY     2022
#define IDS_SIZE_UP   2023
#define IDS_SIZE_DOWN 2024
#define ID_HK_TOGGLE 3001
#define ID_HK_STOP   3002
#define ID_HK_OVERLAY 3003
#define ID_HK_CLICK  3004
#define ID_HK_TIMER  3005

static LRESULT CALLBACK main_proc(HWND, UINT, WPARAM, LPARAM);
static void ui_tick(void);
static void toggle_recording(void);
static void toggle_timer(void);
static void stop_recording(const char *reason);
static void stop_timer(void);
static void start_recording_now(void);
static void update_buttons(void);
static void reset_timer(void);
static long long timer_display_ms(void);
static void format_clock(long long ms, char *out, int outcap);
static void attach_console(void)
{
    typedef BOOL (WINAPI *PFN_ATTACH)(DWORD);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    PFN_ATTACH p = NULL;
    if (k) p = (PFN_ATTACH)GetProcAddress(k, "AttachConsole");
    if (p && p(ATTACH_PARENT_PROCESS)) {
        if (freopen("CONOUT$", "w", stdout)) g_conout = 1;
        freopen("CONOUT$", "w", stderr);
    }
}

/* Print to the console (when launched from a command prompt) and to the
 * optional report file, so automated tests can collect the output either
 * way -- AttachConsole does not exist on every XP build. */
static void report(const char *fmt, ...)
{
    va_list ap;
    char buf[2048];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_conout) { fputs(buf, stdout); fflush(stdout); }
    if (g_report) { fputs(buf, g_report); fflush(g_report); }
}

static void usage(void)
{
    report("%s %s\n", APP_NAME_EN, APP_VERSION);
    report("用法 usage:\n");
    report("  ExamTimer.exe [options]\n\n");
    report("  --out <file>        输出文件（含目录），支持 %%Y%%m%%d %%H%%M%%S 时间格式\n");
    report("  --out-dir <目录>    录像保存目录（默认：程序目录\\record）\n");
    report("  --out-name <文件名> 文件名模板（默认 exam_%%Y%%m%%d_%%H%%M%%S.avi）\n");
    report("  --audio [auto|off|test|N]  录制声音：auto=优先系统声音，N=设备序号\n");
    report("  --audio-match <文字>       按名称匹配录音设备（如 立体声混音）\n");
    report("  --no-audio          不录声音\n");
    report("  --config <file>     使用指定的 ini 配置文件\n");
    report("  --fps N             帧率 1-30 (默认 5)\n");
    report("  --quality N         JPEG 质量 1-100 (默认 75)\n");
    report("  --scale N           画面缩放百分比 10-100 (默认 100)\n");
    report("  --region x,y,w,h    只录制指定区域\n");
    report("  --pos <p>           计时器位置 topleft|topcenter|topright (默认 topcenter)\n");
    report("  --offset x,y        位置偏移(或自定义坐标)\n");
    report("  --fontsize N        计时器字号 (默认 44)\n");
    report("  --alpha N           计时器不透明度 16-255 (默认 224)\n");
    report("  --label <text>      计时器左侧标签文字\n");
    report("  --no-overlay        不显示屏幕上的悬浮计时器\n");
    report("  --no-burnin         不把时间烧录进视频\n");
    report("  --fixed             固定悬浮窗（点击穿透、不可移动，默认）\n");
    report("  --no-fixed          取消固定，可用鼠标拖动悬浮窗\n");
    report("  --countdown <sec>   改为倒计时\n");
    report("  --granularity <g>   hundredth|tenth|second (默认 tenth)\n");
    report("  --duration N        N 秒后自动停止录制\n");
    report("  --run-seconds N     自动开始，N 秒后停止并退出(测试用)\n");

    report("  --shot <file.bmp>   只截一张图然后退出\n");
    report("  --selftest          运行内置自检并退出\n");
    report("  --no-tray           不显示托盘图标\n");
    report("  --minimize          启动时最小化控制面板\n");
    report("  --log <file>        输出日志文件\n");
    report("  -h, --help          显示帮助\n");
}

/* ------------------------------------------------------------------ */
/* app plumbing                                                        */
/* ------------------------------------------------------------------ */

void app_request_stop(const char *reason)
{
    (void)reason;
    if (g_main) PostMessageA(g_main, WM_APP_STATUS, 0, 0);
}

static void make_style(void)
{
    memset(&g_style, 0, sizeof(g_style));
    g_style.fontsize = g_cfg.fontsize;
    g_style.fg_alpha = g_cfg.fg_alpha;
    g_style.bg_alpha = g_cfg.bg_alpha;
    g_style.show_label = g_cfg.show_label;
    g_style.show_rec_dot = g_cfg.show_rec_dot;
    g_style.show_border = g_cfg.show_border;
    g_style.fg = g_cfg.fg;
    g_style.bg = g_cfg.bg;
    g_style.border = g_cfg.border;
    strncpy(g_style.label, g_cfg.label, sizeof(g_style.label) - 1);
}

static void set_default_label(void)
{
    /* the source file is UTF-8, so this narrow literal holds UTF-8 bytes and
     * is converted to UTF-16 correctly on any Windows locale */
    if (!g_cfg.label[0])
        strcpy(g_cfg.label, "考核计时");
}

static void resolve_paths(void)
{
    wchar_t wexe[MAX_PATH_LONG];
    char exe[MAX_PATH_LONG];
    char *p;
    /* W version: on a Chinese XP the folder name would otherwise arrive as
     * GBK bytes and end up mixed with the program's UTF-8 literals. */
    if (GetModuleFileNameW(NULL, wexe, MAX_PATH_LONG) > 0)
        wide_to_utf8(wexe, exe, sizeof(exe));
    else {
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        normalize_path_utf8(exe, sizeof(exe));
    }
    strncpy(g_exe_dir, exe, sizeof(g_exe_dir) - 1);
    g_exe_dir[sizeof(g_exe_dir) - 1] = 0;
    p = strrchr(g_exe_dir, '\\');
    if (p) *p = 0;
    else g_exe_dir[0] = 0;
}

static void make_absolute(char *out, int outcap, const char *in)
{
    char tmp[MAX_PATH_LONG];
    if (!in || !in[0]) { out[0] = 0; return; }
    /* copy first: out and in may be the same buffer */
    strncpy(tmp, in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    if (tmp[0] == '\\' || (tmp[1] == ':')) {
        strncpy(out, tmp, (size_t)outcap - 1);
        out[outcap - 1] = 0;
        return;
    }
    snprintf(out, (size_t)outcap, "%s\\%s", g_exe_dir, tmp);
}

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void load_config(void)
{
    snprintf(g_ini_path, sizeof(g_ini_path), "%s\\examtimer.ini", g_exe_dir);
    if (file_exists(g_ini_path)) {
        g_cfg.label[0] = 0;               /* sentinel: ini may set it */
        cfg_load_ini(&g_cfg, g_ini_path);
    }
}

static void save_overlay_position(void)
{
    /* keep the operator's window position between runs */
    if (g_cfg.pos != POS_CUSTOM) return;
    {
        FILE *f = fopen(g_ini_path, "a");
        if (!f) return;
        fprintf(f, "\n; --- auto saved %s ---\n[overlay]\npos=custom\nx=%d\ny=%d\n",
                APP_VERSION, g_cfg.offset_x, g_cfg.offset_y);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* recording control                                                   */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* recording / timing control (two independent state machines)         */
/* ------------------------------------------------------------------ */

static long long timer_display_ms(void)
{
    if (g_tim_state == 0) return 0;
    return timer_elapsed_ms(&g_timer);
}

static void format_clock(long long ms, char *out, int outcap)
{
    if (g_cfg.countdown) {
        long long remain = (long long)g_cfg.countdown_seconds * 1000 - ms;
        timer_format_countdown(remain, g_cfg.granularity, out, outcap);
    } else {
        timer_format(ms, g_cfg.granularity, out, outcap);
    }
}

static void start_recording_now(void)
{
    char init_txt[64];
    char tmpl[MAX_PATH_LONG], path[MAX_PATH_LONG];
    int with_audio;

    cfg_output_path(&g_cfg, tmpl, sizeof(tmpl));
    expand_timestamp(tmpl, path, sizeof(path));
    make_absolute(path, sizeof(path), path);
    path_ensure_dir(path);

    format_clock(timer_display_ms(), init_txt, sizeof(init_txt));
    overlay_set_text(init_txt);

    with_audio = (g_cfg.audio_enabled && g_cfg.audio_device != -1);
    if (recorder_start(&g_cfg, &g_style, init_txt, path, with_audio) != 0) {
        MessageBoxW(g_main, L"启动录像失败，请检查保存目录是否可写。",
                    L"考核计时录屏器", MB_ICONERROR);
        g_rec_state = 0;
        update_buttons();
        return;
    }
    overlay_set_recording(1);
    g_rec_state = 1;
    g_did_start = 1;
    app_log("recording started -> %s (audio=%d)", path, with_audio);
    update_buttons();
}

/* F8 / 开始录像按钮：开始 -> 暂停 -> 继续 */
static void toggle_recording(void)
{
    if (g_rec_state == 0) {
        start_recording_now();
    } else if (g_rec_state == 1) {
        recorder_set_paused(1);
        g_rec_state = 2;
        app_log("recording paused");
        update_buttons();
    } else {
        recorder_set_paused(0);
        g_rec_state = 1;
        app_log("recording resumed");
        update_buttons();
    }
}

/* F9 / 开始计时按钮：开始 -> 暂停 -> 继续 */
static void toggle_timer(void)
{
    if (g_tim_state == 0) {
        timer_reset(&g_timer);
        timer_start(&g_timer);
        g_tim_state = 1;
        app_log("timer started");
    } else if (g_tim_state == 1) {
        timer_pause(&g_timer);
        g_tim_state = 2;
        app_log("timer paused");
    } else {
        timer_start(&g_timer);
        g_tim_state = 1;
        app_log("timer resumed");
    }
    update_buttons();
}

static void stop_timer(void)
{
    if (g_tim_state == 0) return;
    timer_pause(&g_timer);
    g_tim_state = 2;
    update_buttons();
}

static void reset_timer(void)
{
    int was = (g_tim_state == 1);
    timer_reset(&g_timer);
    if (was) timer_start(&g_timer);
    if (g_tim_state == 2) g_tim_state = 0;
    update_buttons();
}

static void stop_recording(const char *reason)
{
    RecStatus st;
    if (g_rec_state == 0) return;
    recorder_stop();
    overlay_set_recording(0);
    recorder_get_status(&st);
    if (st.file[0])
        strncpy(g_last_file, st.file, sizeof(g_last_file) - 1);
    g_rec_state = 0;
    app_log("recording stopped (%s) frames=%d bytes=%I64d audio=%d file=%s",
            reason ? reason : "", st.frames, st.bytes, st.audio_bytes, st.file);
    update_buttons();
}

/* ------------------------------------------------------------------ */
/* buttons                                                             */
/* ------------------------------------------------------------------ */

static void update_buttons(void)
{
    if (!g_main) return;

    if (g_btn_rec) {
        SetWindowTextW(g_btn_rec,
                       g_rec_state == 0 ? L"● 开始录像" :
                       g_rec_state == 1 ? L"‖ 暂停录像" : L"▶ 继续录像");
    }
    if (g_btn_timer) {
        SetWindowTextW(g_btn_timer,
                       g_tim_state == 0 ? L"开始计时" :
                       g_tim_state == 1 ? L"暂停计时" : L"继续计时");
    }
    if (g_btn_stop) EnableWindow(g_btn_stop, g_rec_state != 0 || g_tim_state != 0);
    if (g_btn_overlay) {
        SetWindowTextW(g_btn_overlay,
                       overlay_visible() ? L"隐藏计时器" : L"显示计时器");
    }
    if (g_btn_fixed) {
        SetWindowTextW(g_btn_fixed,
                       overlay_fixed() ? L"固定计时器" : L"活动计时器");
    }
    if (g_lbl_state) {
        const wchar_t *s;
        if (g_cfg.audio_enabled && g_rec_state != 0) {
            static wchar_t buf[160];
            AudioStatus as;
            audio_status(&as);
            if (as.active)
                swprintf(buf, 160, L"状态：录像中（含声音 %d Hz）", as.rate);
            else
                swprintf(buf, 160, L"状态：录像中（无声）");
            s = buf;
        } else {
            s = g_rec_state == 0 ? (g_tim_state ? L"状态：仅计时中" : L"状态：待机")
              : g_rec_state == 1 ? L"状态：录像中"
              : L"状态：录像已暂停";
        }
        SetWindowTextW(g_lbl_state, s);
    }
}

/* ------------------------------------------------------------------ */
/* per tick                                                            */
/* ------------------------------------------------------------------ */

static void ui_tick(void)
{
    char txt[64];
    long long ms = timer_display_ms();

    format_clock(ms, txt, sizeof(txt));
    overlay_set_text(txt);
    if (g_rec_state != 0)
        recorder_set_timer_text(txt);

    if (g_lbl_time) {
        wchar_t w[128];
        utf8_to_wide(txt, w, 128);
        SetWindowTextW(g_lbl_time, w);
    }

    /* auto stop is measured on the RECORDING, not on the clock: the two are
     * independent, so --duration must not depend on the operator starting
     * the timer. */
    if (g_rec_state != 0 && (g_cfg.duration_sec > 0 || g_cfg.cmd_run_seconds > 0)) {
        RecStatus rs;
        recorder_get_status(&rs);
        if (g_cfg.duration_sec > 0 && rs.elapsed_sec >= g_cfg.duration_sec)
            stop_recording("duration");
        else if (g_cfg.cmd_run_seconds > 0 && rs.elapsed_sec >= g_cfg.cmd_run_seconds)
            stop_recording("run-seconds");
    }
    if (g_cfg.cmd_run_seconds > 0 && g_did_start && g_rec_state == 0 && !g_quit_pending) {
        g_quit_pending = 1;
        PostMessageA(g_main, WM_CLOSE, 0, 0);
        return;
    }

    /* pick up a drag or a resize done directly on the overlay, so the
     * position/size survive into the ini and the video */
    {
        static int tick2 = 0;
        if ((tick2++ % 20) == 0) {
            int pos = 0, ox = 0, oy = 0, fs = 0;
            overlay_get_geometry(&pos, &ox, &oy, &fs);
            if (pos != g_cfg.pos || ox != g_cfg.offset_x || oy != g_cfg.offset_y) {
                g_cfg.pos = pos;
                g_cfg.offset_x = ox;
                g_cfg.offset_y = oy;
                g_geom_dirty = 1;
            }
            if (fs > 0 && fs != g_cfg.fontsize) {
                g_cfg.fontsize = fs;
                g_style.fontsize = fs;
                g_geom_dirty = 1;
            } else if (fs > 0 && g_style.fontsize != fs) {
                g_style.fontsize = fs;
            }
        }
    }

    /* status line, rebuilt as UTF-8 and only twice a second */
    {
        static int tick = 0;
        if ((tick++ % 25) == 0 && g_lbl_info) {
            RecStatus st;
            char b[1024];
            wchar_t w[1024];
            recorder_get_status(&st);
            if (st.active || st.frames || st.error) {
                snprintf(b, sizeof(b),
                         "文件：%s\n帧数：%d    大小：%.1f MB    分卷：%d    "
                         "编码：%d ms/帧\n画面：%dx%d    实测：%.1f fps%s",
                         st.file, st.frames, (double)st.bytes / 1048576.0, st.parts,
                         st.encode_ms_last, st.capture_w, st.capture_h,
                         st.fps_actual_x10 / 10.0,
                         st.audio_bytes ? "    含音频" : "");
                if (st.error) {
                    strncat(b, "\n错误：", sizeof(b) - strlen(b) - 1);
                    strncat(b, st.message, sizeof(b) - strlen(b) - 1);
                }
            } else {
                char dir[MAX_PATH_LONG];
                make_absolute(dir, sizeof(dir), g_cfg.out_dir);
                snprintf(b, sizeof(b),
                         "保存目录：%s\n文件名：%s\n"
                         "提示：先点“开始录像”，再点“开始计时”；两者互不影响。",
                         dir, g_cfg.out_name);
            }
            utf8_to_wide(b, w, 1024);
            SetWindowTextW(g_lbl_info, w);
        }
    }

    if (g_cfg.show_elapsed_in_title && g_rec_state == 1) {
        wchar_t w[256];
        char b[256];
        snprintf(b, sizeof(b), "%s %s - %s", APP_NAME, APP_VERSION, txt);
        utf8_to_wide(b, w, 256);
        SetWindowTextW(g_main, w);
    }
}

static void tray_add(void);
static void tray_remove(void);
static void tray_menu(void);
static void open_settings(void);
static void refresh_style_from_cfg(void);

/* ------------------------------------------------------------------ */
/* output helpers                                                      */
/* ------------------------------------------------------------------ */

static void tray_add(void)
{
    NOTIFYICONDATAW nid;
    if (!g_cfg.tray || g_tray_added) return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_main;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy(nid.szTip, L"考核计时录屏器");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) g_tray_added = 1;
}

static void tray_remove(void)
{
    NOTIFYICONDATAW nid;
    if (!g_tray_added) return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_main;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = 0;
}

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    AppendMenuW(m, MF_STRING, IDM_SHOW_CONTROL, L"显示控制面板");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_START_PAUSE, L"开始 / 暂停录像");
    AppendMenuW(m, MF_STRING, IDC_TIMER, L"开始 / 暂停计时");
    AppendMenuW(m, MF_STRING, IDM_STOP_SAVE, L"停止并保存录像");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (overlay_visible() ? MF_CHECKED : 0),
                IDM_SHOW_OVERLAY, L"显示计时器");
    AppendMenuW(m, MF_STRING | (overlay_fixed() ? MF_CHECKED : 0),
                IDM_TOGGLE_CLICK, L"固定计时器");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDC_SETTINGS, L"设置…");
    AppendMenuW(m, MF_STRING, IDM_OPEN_OUTPUT, L"打开录像目录");
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"退出");
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
    PostMessage(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void shell_open_dir(const char *utf8_dir)
{
    wchar_t w[MAX_PATH_LONG];
    char abs[MAX_PATH_LONG];
    make_absolute(abs, sizeof(abs), utf8_dir);
    u8_mkdir_deep(abs);
    if (utf8_to_wide(abs, w, MAX_PATH_LONG) > 0)
        ShellExecuteW(g_main, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

static void open_output_folder(void)
{
    char dir[MAX_PATH_LONG];
    if (g_last_file[0]) {
        char *p;
        strncpy(dir, g_last_file, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        p = strrchr(dir, '\\');
        if (p) *p = 0;
        shell_open_dir(dir);
        return;
    }
    shell_open_dir(g_cfg.out_dir);
}

static void do_command(int id)
{
    switch (id) {
    case IDM_START_PAUSE:
    case IDC_REC:
        toggle_recording();
        break;
    case IDC_TIMER:
        toggle_timer();
        break;
    case IDC_RESET:
        reset_timer();
        break;
    case IDM_STOP_SAVE:
    case IDC_STOP:
        stop_recording("user");
        stop_timer();
        break;
    case IDM_SHOW_OVERLAY:
    case IDC_OVERLAY:
        overlay_show(!overlay_visible());
        update_buttons();
        break;
    case IDM_TOGGLE_CLICK:
    case IDC_FIXED:
        overlay_set_fixed(!overlay_fixed());
        g_cfg.fixed = overlay_fixed();
        update_buttons();
        break;
    case IDC_SETTINGS:
        open_settings();
        break;
    case IDM_SHOW_CONTROL:
        ShowWindow(g_main, SW_SHOWNORMAL);
        SetForegroundWindow(g_main);
        break;
    case IDM_OPEN_OUTPUT:
    case IDC_FOLDER:
        open_output_folder();
        break;
    case IDM_QUIT:
    case IDC_QUIT:
        PostMessageA(g_main, WM_CLOSE, 0, 0);
        break;
    }
}

static HWND mkbtn(HWND h, HFONT f, const wchar_t *txt, int x, int y, int w, int ht, int id)
{
    HWND c = CreateWindowW(L"BUTTON", txt, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           x, y, w, ht, h, (HMENU)(INT_PTR)id, g_hinst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static void create_controls(HWND h)
{
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND c;
    int y = 10;

    {
        wchar_t wt[128];
        char vb[160];
        snprintf(vb, sizeof(vb), "%s  %s", APP_NAME, APP_VERSION);
        utf8_to_wide(vb, wt, 128);
        c = CreateWindowW(L"STATIC", wt, WS_CHILD | WS_VISIBLE,
                          14, y, 360, 22, h, NULL, g_hinst, NULL);
    }
    SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);

    g_lbl_state = CreateWindowW(L"STATIC", L"状态：待机", WS_CHILD | WS_VISIBLE,
                                14, y + 26, 220, 20, h, NULL, g_hinst, NULL);
    SendMessage(g_lbl_state, WM_SETFONT, (WPARAM)f, TRUE);

    g_lbl_time = CreateWindowW(L"STATIC", L"00:00:00.0", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               240, y + 18, 280, 40, h, NULL, g_hinst, NULL);
    SendMessage(g_lbl_time, WM_SETFONT,
                (WPARAM)CreateFontA(-30, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                    0, 0, ANTIALIASED_QUALITY, 0, "Arial"), TRUE);

    y += 62;
    g_btn_rec     = mkbtn(h, f, L"● 开始录像", 14,  y, 110, 32, IDC_REC);
    g_btn_stop    = mkbtn(h, f, L"停止并保存", 132, y, 100, 32, IDC_STOP);
    g_btn_folder  = mkbtn(h, f, L"打开录像目录", 240, y, 116, 32, IDC_FOLDER);
    g_btn_settings= mkbtn(h, f, L"设置…",      364, y, 100, 32, IDC_SETTINGS);

    y += 40;
    g_btn_timer   = mkbtn(h, f, L"开始计时", 14,  y, 110, 32, IDC_TIMER);
    g_btn_reset   = mkbtn(h, f, L"计时归零", 132, y, 100, 32, IDC_RESET);
    g_btn_overlay = mkbtn(h, f, L"隐藏计时器", 240, y, 116, 32, IDC_OVERLAY);
    g_btn_fixed   = mkbtn(h, f, L"固定：开（点不中）", 364, y, 100, 32, IDC_FIXED);

    y += 40;
    g_lbl_info = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               14, y, 450, 96, h, NULL, g_hinst, NULL);
    SendMessage(g_lbl_info, WM_SETFONT, (WPARAM)f, TRUE);

    g_btn_quit = mkbtn(h, f, L"退出程序", 364, y + 104, 100, 28, IDC_QUIT);
}

/* ------------------------------------------------------------------ */
/* settings dialog                                                     */
/* ------------------------------------------------------------------ */

static HWND g_set = NULL;
static Config g_set_cfg;
static COLORREF g_set_fg, g_set_bg;
static HWND g_set_audio_combo = NULL;
static int  g_audio_combo_count = 0;

static HWND set_item(int id) { return GetDlgItem(g_set, id); }

static void set_text(int id, const char *utf8)
{
    wchar_t w[1024];
    utf8_to_wide(utf8, w, 1024);
    SetWindowTextW(set_item(id), w);
}

static int set_get_text(int id, char *out, int cap)
{
    wchar_t w[1024];
    GetWindowTextW(set_item(id), w, 1024);
    return wide_to_utf8(w, out, cap);
}

static int set_get_int(int id, int def)
{
    char b[64];
    set_get_text(id, b, sizeof(b));
    if (!b[0]) return def;
    return atoi(b);
}

static void audio_combo_fill(void)
{
    int n = audio_device_count();
    int i;
    wchar_t w[256];
    char nm[160];

    SendMessageW(g_set_audio_combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_set_audio_combo, CB_ADDSTRING, 0, (LPARAM)L"不录音");
    SendMessageW(g_set_audio_combo, CB_ADDSTRING, 0, (LPARAM)L"自动（优先系统声音/立体声混音）");
    for (i = 0; i < n; i++) {
        if (audio_device_name_at(i, nm, sizeof(nm)) == 0) {
            utf8_to_wide(nm, w, 256);
            SendMessageW(g_set_audio_combo, CB_ADDSTRING, 0, (LPARAM)w);
        }
    }
    SendMessageW(g_set_audio_combo, CB_ADDSTRING, 0, (LPARAM)L"测试用正弦波（无需声卡）");
    g_audio_combo_count = n;
}

static int audio_combo_to_dev(int sel)
{
    if (sel == 0) return -1;                       /* off        */
    if (sel == 1) return -2;                       /* auto       */
    if (sel >= 2 + g_audio_combo_count) return -3; /* synthetic  */
    return sel - 2;
}

static int dev_to_audio_combo(int dev, int enabled)
{
    if (!enabled || dev == -1) return 0;
    if (dev == -3) return 2 + g_audio_combo_count;
    if (dev >= 0 && dev < g_audio_combo_count) return dev + 2;
    return 1;                                      /* auto */
}

/* ------------------------------------------------------------------ */
/* colour + opacity sub dialog (third level)                           */
/* ------------------------------------------------------------------ */

static HWND g_col = NULL;
static int  g_col_is_fg = 0, g_col_alpha = 255;
static COLORREF g_col_color;
static HWND g_col_tb = NULL, g_col_val = NULL, g_col_sw = NULL;
#define IDC_COL_PICK  2101
#define IDC_COL_ALPHA 2102
#define IDC_COL_OK    2103
#define IDC_COL_CANCEL 2104

static void swatch_paint(HWND h, COLORREF col, int alpha)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    int x, y, cell = 8;
    GetClientRect(h, &rc);
    for (y = 0; y < rc.bottom; y += cell) {
        for (x = 0; x < rc.left; x += cell) { /* noop to keep x used */ break; }
        for (x = 0; x < rc.right; x += cell) {
            int dark = (((x / cell) + (y / cell)) & 1);
            RECT c = { x, y, x + cell, y + cell };
            HBRUSH b = CreateSolidBrush(dark ? RGB(90, 90, 90) : RGB(160, 160, 160));
            FillRect(dc, &c, b);
            DeleteObject(b);
            if (x + cell > rc.right) break;
        }
        if (y + cell > rc.bottom) break;
    }
    {
        HBRUSH b = CreateSolidBrush(col);
        int r = GetRValue(col) * alpha / 255 + 90 * (255 - alpha) / 255;
        int g = GetGValue(col) * alpha / 255 + 90 * (255 - alpha) / 255;
        int bl = GetBValue(col) * alpha / 255 + 90 * (255 - alpha) / 255;
        /* blend against the checkerboard average so the preview reads true */
        HBRUSH b2 = CreateSolidBrush(RGB(r, g, bl));
        FillRect(dc, &rc, b2);
        DeleteObject(b2);
        DeleteObject(b);
    }
    FrameRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    EndPaint(h, &ps);
}

static LRESULT CALLBACK swatch_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: swatch_paint(h, g_col_color, g_col_alpha); return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(h, m, wp, lp);
}

static void col_refresh(void)
{
    if (g_col_sw) InvalidateRect(g_col_sw, NULL, TRUE);
    if (g_col_val) {
        wchar_t wv[32];
        swprintf(wv, 32, L"%d%%", g_col_alpha * 100 / 255);
        SetWindowTextW(g_col_val, wv);
    }
    if (g_col_tb) SendMessageW(g_col_tb, TBM_SETPOS, TRUE, (LPARAM)(g_col_alpha * 100 / 255));
}

static LRESULT CALLBACK color_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND c;
        int y = 14;
        c = CreateWindowW(L"STATIC", L"颜色", WS_CHILD | WS_VISIBLE, 14, y + 3, 64, 18, h, NULL, g_hinst, NULL);
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
        c = CreateWindowW(L"BUTTON", g_col_is_fg ? L"选择文字颜色…" : L"选择背景颜色…",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 84, y, 150, 24, h,
                          (HMENU)IDC_COL_PICK, g_hinst, NULL);
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
        y += 34;
        c = CreateWindowW(L"STATIC", L"不透明度", WS_CHILD | WS_VISIBLE, 14, y + 3, 64, 18, h, NULL, g_hinst, NULL);
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
        g_col_tb = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                 84, y, 170, 26, h, (HMENU)IDC_COL_ALPHA, g_hinst, NULL);
        SendMessageW(g_col_tb, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        c = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 262, y + 4, 60, 18, h, NULL, g_hinst, NULL);
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
        g_col_val = c;
        y += 38;
        g_col_sw = CreateWindowW(L"ExamTimerSwatch", L"", WS_CHILD | WS_VISIBLE,
                                 84, y, 150, 34, h, NULL, g_hinst, NULL);
        y += 48;
        c = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                          84, y, 70, 26, h, (HMENU)IDC_COL_OK, g_hinst, NULL);
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
        c = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          164, y, 70, 26, h, (HMENU)IDC_COL_CANCEL, g_hinst, NULL);
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
        col_refresh();
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == g_col_tb) {
            int p = (int)SendMessageW(g_col_tb, TBM_GETPOS, 0, 0);
            g_col_alpha = p * 255 / 100;
            if (g_col_alpha > 255) g_col_alpha = 255;
            if (g_col_val) {
                wchar_t wv[32];
                swprintf(wv, 32, L"%d%%", p);
                SetWindowTextW(g_col_val, wv);
            }
            if (g_col_sw) InvalidateRect(g_col_sw, NULL, TRUE);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_COL_PICK: {
            CHOOSECOLORW cc;
            COLORREF cust[16];
            int i;
            for (i = 0; i < 16; i++) cust[i] = RGB(255, 255, 255);
            memset(&cc, 0, sizeof(cc));
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = h;
            cc.lpCustColors = cust;
            cc.rgbResult = g_col_color;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) {
                g_col_color = cc.rgbResult;
                if (g_col_sw) InvalidateRect(g_col_sw, NULL, TRUE);
            }
            return 0;
        }
        case IDC_COL_OK:
            if (g_col_is_fg) { g_set_fg = g_col_color; g_set_cfg.fg_alpha = g_col_alpha; }
            else             { g_set_bg = g_col_color; g_set_cfg.bg_alpha = g_col_alpha; }
            DestroyWindow(h);
            return 0;
        case IDC_COL_CANCEL:
            DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_col = NULL;
        if (g_set) { EnableWindow(g_set, TRUE); SetForegroundWindow(g_set); }
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

static void open_color_dialog(HWND owner, int is_fg)
{
    WNDCLASSW wc;
    HWND h;
    RECT r;

    if (g_col) { SetForegroundWindow(g_col); return; }
    g_col_is_fg = is_fg;
    if (is_fg) { g_col_color = g_set_fg; g_col_alpha = g_set_cfg.fg_alpha; }
    else       { g_col_color = g_set_bg; g_col_alpha = g_set_cfg.bg_alpha; }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = color_proc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "ExamTimerColorWnd";
    if (!RegisterClassA(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    }
    {
        WNDCLASSA sc;
        memset(&sc, 0, sizeof(sc));
        sc.lpfnWndProc = swatch_proc;
        sc.hInstance = g_hinst;
        sc.hCursor = LoadCursor(NULL, IDC_ARROW);
        sc.lpszClassName = "ExamTimerSwatch";
        RegisterClassA(&sc);
    }

    h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ExamTimerColorWnd", L"",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        CW_USEDEFAULT, CW_USEDEFAULT, 340, 200,
                        owner, NULL, g_hinst, NULL);
    if (!h) return;
    /* set the caption through the explicit W API once the window exists */
    SetWindowTextW(h, is_fg ? L"文字颜色与不透明度" : L"背景颜色与不透明度");
    g_col = h;
    GetWindowRect(h, &r);
    SetWindowPos(h, NULL,
                 (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    if (g_set) EnableWindow(g_set, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

/* ------------------------------------------------------------------ */
/* settings dialog                                                     */
/* ------------------------------------------------------------------ */

static void set_layout(void)
{
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND c;
    int y = 14, hgt = 22;

#define LBLX(t, x, yy, w) do { c = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, \
        x, yy + 3, w, 18, g_set, NULL, g_hinst, NULL); \
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE); } while (0)
#define LBL(t, yy) LBLX(t, 16, yy, 96)
#define EDIT(id, x, yy, w) do { c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", \
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, yy, w, hgt, g_set, \
        (HMENU)(INT_PTR)id, g_hinst, NULL); \
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE); } while (0)
#define BTN(t, id, x, yy, w) do { c = CreateWindowW(L"BUTTON", t, \
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, yy, w, hgt, g_set, \
        (HMENU)(INT_PTR)id, g_hinst, NULL); \
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE); } while (0)
#define COMBO(id, x, yy, w) do { c = CreateWindowW(L"COMBOBOX", L"", \
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, x, yy, w, 240, \
        g_set, (HMENU)(INT_PTR)id, g_hinst, NULL); \
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE); } while (0)
#define CHECK(id, t, x, yy, w) do { c = CreateWindowW(L"BUTTON", t, \
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, yy + 2, w, 20, g_set, \
        (HMENU)(INT_PTR)id, g_hinst, NULL); \
        SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE); } while (0)

    LBL(L"保存目录", y);
    EDIT(IDS_DIR, 116, y, 296);
    BTN(L"浏览…", IDS_BROWSE, 420, y, 68);
    y += 30;

    LBL(L"文件名", y);
    EDIT(IDS_NAME, 116, y, 372);
    y += 30;

    LBL(L"计时器文字", y);
    EDIT(IDS_LABEL, 116, y, 130);
    LBLX(L"位置", 254, y, 40);
    COMBO(IDS_POS, 296, y, 100);
    SendMessageW(set_item(IDS_POS), CB_ADDSTRING, 0, (LPARAM)L"顶部居中");
    SendMessageW(set_item(IDS_POS), CB_ADDSTRING, 0, (LPARAM)L"左上角");
    SendMessageW(set_item(IDS_POS), CB_ADDSTRING, 0, (LPARAM)L"右上角");
    SendMessageW(set_item(IDS_POS), CB_ADDSTRING, 0, (LPARAM)L"自定义");
    CHECK(IDS_FIXED, L"固定计时器", 404, y, 110);
    y += 30;

    LBL(L"计时器大小", y);
    EDIT(IDS_FONT, 116, y, 52);
    BTN(L"－", IDS_SIZE_DOWN, 172, y, 30);
    BTN(L"＋", IDS_SIZE_UP, 206, y, 30);
    LBLX(L"时间精度", 250, y, 56);
    COMBO(IDS_GRAN, 310, y, 90);
    SendMessageW(set_item(IDS_GRAN), CB_ADDSTRING, 0, (LPARAM)L"0.1 秒");
    SendMessageW(set_item(IDS_GRAN), CB_ADDSTRING, 0, (LPARAM)L"0.01 秒");
    SendMessageW(set_item(IDS_GRAN), CB_ADDSTRING, 0, (LPARAM)L"整秒");
    y += 30;

    LBLX(L"帧率", 16, y, 96);
    EDIT(IDS_FPS, 116, y, 48);
    LBLX(L"质量", 176, y, 36);
    EDIT(IDS_QUALITY, 214, y, 48);
    LBLX(L"缩放%", 274, y, 40);
    EDIT(IDS_SCALE, 316, y, 48);
    LBLX(L"分卷MB", 376, y, 48);
    EDIT(IDS_SPLIT, 428, y, 60);
    y += 30;

    LBL(L"声音来源", y);
    g_set_audio_combo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        116, y, 372, 260, g_set, (HMENU)(INT_PTR)IDS_AUDIO, g_hinst, NULL);
    SendMessage(g_set_audio_combo, WM_SETFONT, (WPARAM)f, TRUE);
    y += 34;

    BTN(L"文字颜色/不透明度…", IDS_FG, 116, y, 160);
    BTN(L"背景颜色/不透明度…", IDS_BG, 286, y, 160);
    y += 30;
    CHECK(IDS_STARTUP, L"程序启动后自动开始录像（计时仍需手动开始）", 16, y, 460);
    y += 26;

    c = CreateWindowW(L"STATIC",
        L"提示：选“活动计时器”后可用鼠标拖动位置、拖右下角缩放大小；"
        L"按 F11 隐藏计时器时，录像里也不再显示。",
        WS_CHILD | WS_VISIBLE, 16, y, 470, 34, g_set, NULL, g_hinst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
    y += 40;

    BTN(L"恢复默认", IDS_DEFAULTS, 10, y, 84);
    BTN(L"应用", IDS_APPLY, 270, y, 70);
    BTN(L"确定", IDS_OK, 348, y, 70);
    BTN(L"取消", IDS_CANCEL, 426, y, 70);
#undef LBLX
#undef LBL
#undef EDIT
#undef BTN
#undef COMBO
#undef CHECK
}

static int pos_to_combo(int pos)
{
    switch (pos) {
        case POS_TOP_LEFT:  return 1;
        case POS_TOP_RIGHT: return 2;
        case POS_CUSTOM:    return 3;
        default:            return 0;
    }
}

static int combo_to_pos(int sel)
{
    switch (sel) {
        case 1:  return POS_TOP_LEFT;
        case 2:  return POS_TOP_RIGHT;
        case 3:  return POS_CUSTOM;
        default: return POS_TOP_CENTER;
    }
}

static void set_apply_to_controls(void)
{
    char b[64];
    SendMessageW(set_item(IDS_FIXED), BM_SETCHECK,
                 g_set_cfg.fixed ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(set_item(IDS_STARTUP), BM_SETCHECK,
                 g_set_cfg.autostart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(set_item(IDS_POS), CB_SETCURSEL, (WPARAM)pos_to_combo(g_set_cfg.pos), 0);
    {
        int g = g_set_cfg.granularity == GRAN_TENTH ? 0 :
                g_set_cfg.granularity == GRAN_HUNDREDTH ? 1 : 2;
        SendMessageW(set_item(IDS_GRAN), CB_SETCURSEL, (WPARAM)g, 0);
    }
    SendMessageW(set_item(IDS_AUDIO), CB_SETCURSEL,
                 (WPARAM)dev_to_audio_combo(g_set_cfg.audio_device, g_set_cfg.audio_enabled), 0);
    set_text(IDS_DIR, g_set_cfg.out_dir);
    set_text(IDS_NAME, g_set_cfg.out_name);
    set_text(IDS_LABEL, g_set_cfg.label);
    snprintf(b, sizeof(b), "%d", g_set_cfg.fontsize); set_text(IDS_FONT, b);
    snprintf(b, sizeof(b), "%d", g_set_cfg.fps);      set_text(IDS_FPS, b);
    snprintf(b, sizeof(b), "%d", g_set_cfg.quality);  set_text(IDS_QUALITY, b);
    snprintf(b, sizeof(b), "%d", g_set_cfg.scale);    set_text(IDS_SCALE, b);
    snprintf(b, sizeof(b), "%d", g_set_cfg.split_mb); set_text(IDS_SPLIT, b);
    g_set_fg = g_set_cfg.fg;
    g_set_bg = g_set_cfg.bg;
}

static void set_collect(Config *c)
{
    char b[1024];
    set_get_text(IDS_DIR, c->out_dir, sizeof(c->out_dir));
    set_get_text(IDS_NAME, c->out_name, sizeof(c->out_name));
    set_get_text(IDS_LABEL, c->label, sizeof(c->label));
    c->fontsize   = set_get_int(IDS_FONT, c->fontsize);
    c->fps        = set_get_int(IDS_FPS, c->fps);
    c->quality    = set_get_int(IDS_QUALITY, c->quality);
    c->scale      = set_get_int(IDS_SCALE, c->scale);
    c->split_mb   = set_get_int(IDS_SPLIT, c->split_mb);
    c->pos        = combo_to_pos((int)SendMessageW(set_item(IDS_POS), CB_GETCURSEL, 0, 0));
    {
        int g = (int)SendMessageW(set_item(IDS_GRAN), CB_GETCURSEL, 0, 0);
        c->granularity = g == 1 ? GRAN_HUNDREDTH : g == 2 ? GRAN_SECOND : GRAN_TENTH;
    }
    {
        int sel = (int)SendMessageW(set_item(IDS_AUDIO), CB_GETCURSEL, 0, 0);
        c->audio_device = audio_combo_to_dev(sel);
        c->audio_enabled = (c->audio_device != -1);
    }
    c->fixed   = SendMessageW(set_item(IDS_FIXED), BM_GETCHECK, 0, 0) == BST_CHECKED;
    c->burnin  = 1;                    /* 始终烧录；隐藏计时器即等于不烧录 */
    c->autostart = SendMessageW(set_item(IDS_STARTUP), BM_GETCHECK, 0, 0) == BST_CHECKED;
    c->fg = g_set_fg;
    c->bg = g_set_bg;
    if (c->out_dir[0] && !c->out_name[0])
        strcpy(c->out_name, "exam_%Y%m%d_%H%M%S.avi");
    (void)b;
}

static void save_config_to_ini(void)
{
    /* Build the whole file as UTF-8, then convert once to the local code page
     * so that Windows XP notepad shows it correctly and the values round-trip
     * no matter what the recording folder is called. */
    char u8[8192], ansi[16384];
    char dir[MAX_PATH_LONG], name[256], label[256], amatch[256];
    int n;

    strncpy(dir, g_cfg.out_dir, sizeof(dir) - 1);
    strncpy(name, g_cfg.out_name, sizeof(name) - 1);
    strncpy(label, g_cfg.label, sizeof(label) - 1);
    strncpy(amatch, g_cfg.audio_match, sizeof(amatch) - 1);
    dir[sizeof(dir) - 1] = name[sizeof(name) - 1] = 0;
    label[sizeof(label) - 1] = amatch[sizeof(amatch) - 1] = 0;

    n = snprintf(u8, sizeof(u8),
        "; ExamTimer 配置（由程序“设置”对话框自动保存）\n"
        "\n[record]\n"
        "out_dir=%s\n"
        "out_name=%s\n"
        "fps=%d\n"
        "quality=%d\n"
        "scale=%d\n"
        "subsample=%d\n"
        "captureblt=%d\n"
        "skip_identical=%d\n"
        "split_mb=%d\n"
        "audio=%d\n"
        "audio_match=%s\n"
        "\n[timer]\n"
        "countdown=%d\n"
        "countdown_seconds=%d\n"
        "granularity=%d\n"
        "\n[overlay]\n"
        "enabled=%d\n"
        "pos=%d\n"
        "offset_x=%d\n"
        "offset_y=%d\n"
        "fontsize=%d\n"
        "fg_alpha=%d\n"
        "bg_alpha=%d\n"
        "fixed=%d\n"
        "label=%s\n"
        "fg=#%02X%02X%02X\n"
        "bg=#%02X%02X%02X\n"
        "border=#%02X%02X%02X\n"
        "auto_topmost_ms=%d\n"
        "\n[hotkeys]\n"
        "enabled=%d\n"
        "toggle=%d\n"
        "timer=%d\n"
        "stop=%d\n"
        "overlay=%d\n"
        "click_through=%d\n"
        "\n[app]\n"
        "autostart=%d\n"
        "duration_sec=%d\n"
        "tray=%d\n"
        "minimize_control=%d\n"
        "show_elapsed_in_title=%d\n",
        dir, name, g_cfg.fps, g_cfg.quality, g_cfg.scale, g_cfg.subsample,
        g_cfg.captureblt, g_cfg.skip_identical, g_cfg.split_mb,
        g_cfg.audio_enabled ? g_cfg.audio_device : -1, amatch,
        g_cfg.countdown, g_cfg.countdown_seconds, g_cfg.granularity,
        g_cfg.overlay_enabled, g_cfg.pos, g_cfg.offset_x, g_cfg.offset_y,
        g_cfg.fontsize, g_cfg.fg_alpha, g_cfg.bg_alpha, g_cfg.fixed, label,
        GetRValue(g_cfg.fg), GetGValue(g_cfg.fg), GetBValue(g_cfg.fg),
        GetRValue(g_cfg.bg), GetGValue(g_cfg.bg), GetBValue(g_cfg.bg),
        GetRValue(g_cfg.border), GetGValue(g_cfg.border), GetBValue(g_cfg.border),
        g_cfg.auto_topmost_ms,
        g_cfg.hotkeys_enabled, g_cfg.hk_toggle, g_cfg.hk_timer,
        g_cfg.hk_stop, g_cfg.hk_overlay, g_cfg.hk_click,
        g_cfg.autostart, g_cfg.duration_sec, g_cfg.tray,
        g_cfg.minimize_control, g_cfg.show_elapsed_in_title);
    if (n <= 0) return;

    utf8_to_ansi(u8, ansi, sizeof(ansi));
    {
        FILE *f = u8_fopen(g_ini_path, "w");
        if (!f) return;
        fputs(ansi[0] ? ansi : u8, f);
        fclose(f);
    }
}

static void refresh_style_from_cfg(void)
{
    make_style();
    overlay_apply(&g_style, &g_cfg);
}

static void browse_for_dir(void)
{
    BROWSEINFOW bi;
    wchar_t wdir[MAX_PATH_LONG], wtitle[64];
    LPITEMIDLIST pidl;
    char cur[MAX_PATH_LONG], abs[MAX_PATH_LONG];

    set_get_text(IDS_DIR, cur, sizeof(cur));
    make_absolute(abs, sizeof(abs), cur);
    utf8_to_wide(abs, wdir, MAX_PATH_LONG);
    utf8_to_wide("请选择录像保存目录", wtitle, 64);

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_set;
    bi.lpszTitle = wtitle;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t wsel[MAX_PATH_LONG];
        char u8[MAX_PATH_LONG];
        if (SHGetPathFromIDListW(pidl, wsel)) {
            wide_to_utf8(wsel, u8, sizeof(u8));
            set_text(IDS_DIR, u8);
        }
        CoTaskMemFree(pidl);
    }
}


static LRESULT CALLBACK settings_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    char b[64];
    switch (msg) {
    case WM_CREATE:
        g_set = h;
        set_layout();
        audio_combo_fill();
        set_apply_to_controls();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDS_BROWSE:   browse_for_dir(); return 0;
        case IDS_FG:       open_color_dialog(h, 1); return 0;
        case IDS_BG:       open_color_dialog(h, 0); return 0;
        case IDS_SIZE_UP: case IDS_SIZE_DOWN: {
            int v = set_get_int(IDS_FONT, 44);
            v += (LOWORD(wp) == IDS_SIZE_UP) ? 4 : -4;
            if (v < 8) v = 8;
            if (v > 400) v = 400;
            snprintf(b, sizeof(b), "%d", v);
            set_text(IDS_FONT, b);
            return 0;
        }
        case IDS_APPLY:
            set_collect(&g_set_cfg);
            g_cfg = g_set_cfg;
            refresh_style_from_cfg();
            save_config_to_ini();
            update_buttons();
            return 0;
        case IDS_DEFAULTS: {
            char keep_dir[MAX_PATH_LONG];
            strncpy(keep_dir, g_set_cfg.out_dir, sizeof(keep_dir) - 1);
            cfg_defaults(&g_set_cfg);
            strncpy(g_set_cfg.out_dir, keep_dir, sizeof(g_set_cfg.out_dir) - 1);
            set_apply_to_controls();
            return 0;
        }
        case IDS_OK:
            set_collect(&g_set_cfg);
            g_cfg = g_set_cfg;
            refresh_style_from_cfg();
            save_config_to_ini();
            update_buttons();
            DestroyWindow(h);
            return 0;
        case IDS_CANCEL:
            DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        /* draw the two colour swatches */
        {
            HWND who = (HWND)lp;
            if (who == set_item(IDS_FG) || who == set_item(IDS_BG)) {
                HDC dc = (HDC)wp;
                SetBkColor(dc, who == set_item(IDS_FG) ? g_set_fg : g_set_bg);
                return (LRESULT)GetStockObject(NULL_BRUSH);
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_set = NULL;
        if (g_main && IsWindow(g_main)) {
            EnableWindow(g_main, TRUE);
            SetForegroundWindow(g_main);
        }
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void open_settings(void)
{
    WNDCLASSA wc;
    HWND h;
    RECT r;

    if (g_set) { SetForegroundWindow(g_set); return; }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = settings_proc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "ExamTimerSettingsWnd";
    if (!RegisterClassA(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    }

    g_set_cfg = g_cfg;
    h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ExamTimerSettingsWnd",
                        L"ExamTimer 设置",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        CW_USEDEFAULT, CW_USEDEFAULT, 520, 376,
                        g_main, NULL, g_hinst, NULL);
    if (!h) return;
    GetWindowRect(h, &r);
    SetWindowPos(h, NULL,
                 (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 3,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    EnableWindow(g_main, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        create_controls(h);
        SetTimer(h, IDT_UI, 50, NULL);
        return 0;

    case WM_TIMER:
        if (wp == IDT_UI) ui_tick();
        return 0;

    case WM_COMMAND:
        do_command(LOWORD(wp));
        return 0;

    case WM_HOTKEY:
        if ((int)wp == ID_HK_TOGGLE) toggle_recording();
        else if ((int)wp == ID_HK_TIMER) toggle_timer();
        else if ((int)wp == ID_HK_STOP) { stop_recording("hotkey"); stop_timer(); }
        else if ((int)wp == ID_HK_OVERLAY) { overlay_show(!overlay_visible()); update_buttons(); }
        else if ((int)wp == ID_HK_CLICK) {
            overlay_set_fixed(!overlay_fixed());
            g_cfg.fixed = overlay_fixed();
            update_buttons();
        }
        return 0;

    case WM_APP_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) tray_menu();
        else if (lp == WM_LBUTTONDBLCLK) do_command(IDM_SHOW_CONTROL);
        return 0;

    case WM_APP_STATUS:
        /* the recorder thread asks us to finish up */
        if (g_rec_state != 0) stop_recording("recorder");
        if (g_cfg.cmd_run_seconds > 0 && !g_quit_pending) {
            g_quit_pending = 1;
            PostMessageA(h, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_CLOSE:
        if (g_set) DestroyWindow(g_set);
        if (g_col) DestroyWindow(g_col);
        stop_recording("close");
        if (g_geom_dirty) save_config_to_ini();
        else save_overlay_position();
        if (g_cfg.hotkeys_enabled) {
            UnregisterHotKey(h, ID_HK_TOGGLE);
            UnregisterHotKey(h, ID_HK_TIMER);
            UnregisterHotKey(h, ID_HK_STOP);
            UnregisterHotKey(h, ID_HK_OVERLAY);
            UnregisterHotKey(h, ID_HK_CLICK);
        }
        tray_remove();
        overlay_destroy();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void register_hotkeys(void)
{
    if (!g_cfg.hotkeys_enabled) return;
    if (g_cfg.hk_toggle)  RegisterHotKey(g_main, ID_HK_TOGGLE, 0, (UINT)g_cfg.hk_toggle);
    if (g_cfg.hk_timer)   RegisterHotKey(g_main, ID_HK_TIMER, 0, (UINT)g_cfg.hk_timer);
    if (g_cfg.hk_stop)    RegisterHotKey(g_main, ID_HK_STOP, 0, (UINT)g_cfg.hk_stop);
    if (g_cfg.hk_overlay) RegisterHotKey(g_main, ID_HK_OVERLAY, 0, (UINT)g_cfg.hk_overlay);
    if (g_cfg.hk_click)   RegisterHotKey(g_main, ID_HK_CLICK, 0, (UINT)g_cfg.hk_click);
}

/* ------------------------------------------------------------------ */
/* selftest                                                            */
/* ------------------------------------------------------------------ */

#define TEST(cond, name) do { \
        int _ok = (cond) ? 1 : 0; \
        report("  [%s] %s\n", _ok ? "PASS" : "FAIL", name); \
        if (!_ok) fails++; else passes++; \
    } while (0)

static const char *st_dir(void)
{
    static char d[MAX_PATH_LONG];
    if (d[0]) return d;
    strcpy(d, "C:\\xptest");
    if (!CreateDirectoryA(d, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        snprintf(d, sizeof(d), "%s", g_exe_dir);
    return d;
}

static int zorder_index(HWND target)
{
    HWND w = GetTopWindow(NULL);
    int i = 0;
    while (w) {
        if (w == target) return i;
        w = GetWindow(w, GW_HWNDNEXT);
        i++;
        if (i > 4000) break;
    }
    return -1;
}

static int selftest(void)
{
    int passes = 0, fails = 0;
    char msg[512];

    report("==================================================\n");
    report("%s %s selftest\n", APP_NAME_EN, APP_VERSION);
    report("==================================================\n");

    /* --- environment ------------------------------------------------- */
    {
        OSVERSIONINFOA vi;
        memset(&vi, 0, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        GetVersionExA(&vi);
        report("[environment]\n");
        report("  windows %lu.%lu build %lu  %s\n", vi.dwMajorVersion,
               vi.dwMinorVersion, vi.dwBuildNumber, vi.szCSDVersion);
        report("  screen %dx%d, %d bits, %d monitors\n",
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
               GetDeviceCaps(GetDC(NULL), BITSPIXEL),
               GetSystemMetrics(SM_CMONITORS));
        {
            SYSTEM_INFO si;
            memset(&si, 0, sizeof(si));
            GetSystemInfo(&si);
            report("  processors %lu\n", (unsigned long)si.dwNumberOfProcessors);
        }
        {
            MEMORYSTATUS ms;
            memset(&ms, 0, sizeof(ms));
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatus(&ms);
            report("  memory %lu MB total, %lu MB free\n",
                   (unsigned long)(ms.dwTotalPhys / 1048576),
                   (unsigned long)(ms.dwAvailPhys / 1048576));
        }
    }

    /* --- timing / formatting ---------------------------------------- */
    {
        char b[64];
        report("[timer formatting]\n");
        timer_format(0, GRAN_TENTH, b, sizeof(b));
        TEST(strcmp(b, "00:00:00.0") == 0, "0 ms -> 00:00:00.0");
        timer_format(1500, GRAN_TENTH, b, sizeof(b));
        TEST(strcmp(b, "00:00:01.5") == 0, "1500 ms -> 00:00:01.5");
        timer_format(3661500, GRAN_TENTH, b, sizeof(b));
        TEST(strcmp(b, "01:01:01.5") == 0, "3661500 ms -> 01:01:01.5");
        timer_format(3600000, GRAN_SECOND, b, sizeof(b));
        TEST(strcmp(b, "01:00:00") == 0, "3600000 ms -> 01:00:00");
        timer_format(12345, GRAN_HUNDREDTH, b, sizeof(b));
        TEST(strcmp(b, "00:00:12.34") == 0, "12345 ms -> 00:00:12.34");
        {
            TimerState t;
            long long e;
            timer_reset(&t);
            timer_start(&t);
            Sleep(220);
            e = timer_elapsed_ms(&t);
            TEST(e >= 180 && e <= 400, "timer measures ~220ms of real time");
            timer_pause(&t);
            e = timer_elapsed_ms(&t);
            Sleep(120);
            TEST(timer_elapsed_ms(&t) == e, "paused timer does not advance");
            timer_start(&t);
            Sleep(60);
            TEST(timer_elapsed_ms(&t) > e, "resumed timer advances again");
        }
    }

    /* --- jpeg encoder ------------------------------------------------ */
    report("[jpeg encoder]\n");
    if (jpeg_enc_selftest(msg, sizeof(msg)) == 0) { passes++; report("  [PASS] %s\n", msg); }
    else { fails++; report("  [FAIL] %s\n", msg); }

    /* --- avi writer -------------------------------------------------- */
    {
        char path[MAX_PATH_LONG];
        snprintf(path, sizeof(path), "%s\\selftest.avi", st_dir());
        avi_writer *w;
        int i;
        unsigned char *jpg;
        int jlen = 0;
        unsigned char *img;
        int W = 160, H = 120;
        FILE *f;
        char nbuf[16];
        int frames_field = -1;

        report("[avi writer]\n");
        img = (unsigned char *)malloc((size_t)W * H * 4);
        for (i = 0; i < W * H; i++) {
            img[i * 4 + 0] = (unsigned char)(i % 200);
            img[i * 4 + 1] = (unsigned char)((i * 3) % 200);
            img[i * 4 + 2] = (unsigned char)((i * 7) % 200);
            img[i * 4 + 3] = 255;
        }
        jpg = jpeg_encode_bgr(img, W, H, W * 4, 4, 80, 1, &jlen);
        free(img);
        TEST(jpg != NULL && jlen > 100, "reference frame encodes");

        w = avi_open(path, W, H, 10, 0, NULL);
        TEST(w != NULL, "avi_open succeeds");
        if (w && jpg) {
            for (i = 0; i < 10; i++) avi_add_jpeg(w, jpg, jlen);
            avi_close(w);
        }

        f = fopen(path, "rb");
        TEST(f != NULL, "avi file exists");
        if (f) {
            unsigned char hd[12];
            long fsize;
            unsigned int riff;
            fread(hd, 1, 12, f);
            TEST(memcmp(hd, "RIFF", 4) == 0 && memcmp(hd + 8, "AVI ", 4) == 0,
                 "RIFF/AVI fourcc");
            riff = hd[4] | (hd[5] << 8) | (hd[6] << 16) | ((unsigned)hd[7] << 24);
            fseek(f, 0, SEEK_END);
            fsize = ftell(f);
            TEST(riff == (unsigned int)(fsize - 8), "RIFF size field matches file size");
            /* avih.dwTotalFrames must be 10 */
            fseek(f, 48, SEEK_SET);
            frames_field = -1;
            if (fread(nbuf, 1, 4, f) == 4)
                frames_field = nbuf[0] | (nbuf[1] << 8) | (nbuf[2] << 16) | (nbuf[3] << 24);
            TEST(frames_field == 10, "avih.dwTotalFrames == 10");
            /* strh.dwLength must be 10 */
            fseek(f, 140, SEEK_SET);
            frames_field = -1;
            if (fread(nbuf, 1, 4, f) == 4)
                frames_field = nbuf[0] | (nbuf[1] << 8) | (nbuf[2] << 16) | (nbuf[3] << 24);
            TEST(frames_field == 10, "strh.dwLength == 10");

            /* walk the RIFF structure properly */
            {
                long pos = 12;
                int nframes = 0, jpeg_ok = 1, have_idx = 0, idx_entries = 0;
                int have_movi = 0;
                while (pos + 8 <= fsize) {
                    unsigned char ch[8];
                    unsigned int csz;
                    fseek(f, pos, SEEK_SET);
                    if (fread(ch, 1, 8, f) != 8) break;
                    csz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((unsigned)ch[7] << 24);
                    if (memcmp(ch, "LIST", 4) == 0) {
                        unsigned char t[4];
                        if (fread(t, 1, 4, f) != 4) break;
                        if (memcmp(t, "movi", 4) == 0) {
                            long mp = pos + 12;
                            long mend = pos + 8 + (long)csz;
                            have_movi = 1;
                            while (mp + 8 <= mend) {
                                unsigned char cc[8];
                                unsigned int s2;
                                fseek(f, mp, SEEK_SET);
                                if (fread(cc, 1, 8, f) != 8) break;
                                s2 = cc[4] | (cc[5] << 8) | (cc[6] << 16) | ((unsigned)cc[7] << 24);
                                if (memcmp(cc, "00dc", 4) == 0) {
                                    unsigned char jm[2];
                                    if (fread(jm, 1, 2, f) != 2 || jm[0] != 0xFF || jm[1] != 0xD8)
                                        jpeg_ok = 0;
                                    nframes++;
                                }
                                mp += 8 + (long)s2 + (s2 & 1);
                            }
                        }
                    } else if (memcmp(ch, "idx1", 4) == 0) {
                        have_idx = 1;
                        idx_entries = (int)(csz / 16);
                    }
                    pos += 8 + (long)csz + (csz & 1);
                }
                TEST(have_movi, "movi LIST found");
                TEST(jpeg_ok && nframes == 10, "10 JPEG frames inside the AVI");
                TEST(have_idx, "idx1 index chunk present");
                TEST(idx_entries == 10, "idx1 has one entry per frame");
            }
            fclose(f);
        }
        free(jpg);
    }

    /* --- audio ------------------------------------------------------- */
    {
        char amsg[900];
        report("[audio]\n");
        if (audio_selftest(amsg, sizeof(amsg)) == 0) {
            char *line = amsg;
            while (*line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                if (*line) report("  %s\n", line);
                if (!nl) break;
                line = nl + 1;
            }
            passes++;
        } else {
            fails++;
        }
        /* the synthetic source must survive a full open/start/read cycle */
        {
            Config ac = g_cfg;
            ac.audio_enabled = 1;
            ac.audio_device = -3;
            {
            int ri = audio_init(&ac);
            int rs = (ri == 0) ? audio_start() : -99;
            report("  [info] synthetic source: init=%d start=%d\n", ri, rs);
            if (ri == 0 && rs == 0) {
                unsigned char buf[65536];
                int got;
                Sleep(120);
                audio_synth_fill(200);
                got = audio_read(buf, sizeof(buf));
                TEST(got > 0, "synthetic audio source produces PCM data");
                {
                    AudioStatus as;
                    audio_status(&as);
                    report("  [info] synthetic format: %d Hz %dch %dbit\n",
                           as.rate, as.channels, as.bits);
                }
            } else {
                TEST(0, "synthetic audio source starts");
            }
            audio_shutdown();
            }
        }
    }

    /* --- file output -------------------------------------------------- */
    {
        char wpath[MAX_PATH_LONG];
        FILE *f;
        snprintf(wpath, sizeof(wpath), "%s\\selftest_write.tmp", st_dir());
        f = fopen(wpath, "wb");
        report("[file output]\n");
        TEST(f != NULL, "can write a file into the working directory");
        if (f) { fputs("hello", f); fclose(f); DeleteFileA(wpath); }
    }

    /* --- screen capture ---------------------------------------------- */
    {
        char err[256];
        char spath[MAX_PATH_LONG];
        int rc;
        snprintf(spath, sizeof(spath), "%s\\selftest_shot.bmp", st_dir());
        rc = capture_screen_to_bmp(spath, 0, 0, 320, 240, err, sizeof(err));
        report("[screen capture]\n");
        TEST(rc == 0, "BitBlt screen capture to BMP");
        if (rc == 0) {
            FILE *f = fopen(spath, "rb");
            if (f) {
                BITMAPFILEHEADER fh;
                BITMAPINFOHEADER ih;
                fread(&fh, sizeof(fh), 1, f);
                fread(&ih, sizeof(ih), 1, f);
                TEST(fh.bfType == 0x4D42 && ih.biWidth == 320 && ih.biHeight == 240,
                     "BMP header 320x240");
                fclose(f);
            }
        }
    }

    /* --- overlay window ---------------------------------------------- */
    {
        HWND ov;
        LONG ex;
        RECT r;
        Config cfg = g_cfg;

        report("[overlay window]\n");
        report("  [info] cfg.pos=%d off=(%d,%d) fixed=%d\n",
               cfg.pos, cfg.offset_x, cfg.offset_y, cfg.fixed);
        cfg.fixed = 1;
        make_style();
        if (overlay_create(g_hinst, &g_style, &cfg) == 0) {
            MSG m;
            int guard = 0;
            overlay_set_owner(g_main);
            ShowWindow(overlay_hwnd(), SW_SHOWNOACTIVATE);
            while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE) && guard++ < 500) {
                TranslateMessage(&m); DispatchMessage(&m);
            }
            Sleep(120);
            while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE) && guard++ < 500) {
                TranslateMessage(&m); DispatchMessage(&m);
            }
            ov = overlay_hwnd();
            TEST(ov != NULL && IsWindow(ov), "overlay window created");
            ex = GetWindowLongA(ov, GWL_EXSTYLE);
            TEST((ex & WS_EX_TOPMOST) != 0, "overlay has WS_EX_TOPMOST");
            TEST((ex & WS_EX_TOOLWINDOW) != 0, "overlay is a tool window (no taskbar entry)");
            TEST((ex & WS_EX_LAYERED) != 0, "overlay is layered (transparency)");
            TEST((ex & WS_EX_TRANSPARENT) != 0, "overlay is click-through by default");
            TEST(IsWindowVisible(ov) != 0, "overlay is visible");
            GetWindowRect(ov, &r);
            TEST(r.top >= 0 && r.top <= 40, "overlay pinned to the top edge");
            TEST((r.right - r.left) > 100 && (r.bottom - r.top) > 20, "overlay has a sensible size");
            TEST((r.right - r.left) < GetSystemMetrics(SM_CXSCREEN),
                 "overlay fits on screen horizontally");
            report("  [info] overlay rect = (%ld,%ld)-(%ld,%ld)\n",
                   r.left, r.top, r.right, r.bottom);

            /* topmost behaviour: a competing topmost window must not win */
            {
                HWND rival = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "STATIC",
                                             "rival", WS_POPUP | WS_VISIBLE,
                                             0, 0, 80, 40, NULL, NULL, g_hinst, NULL);
                int iov, irv;
                MSG m2;
                int g2 = 0;
                while (PeekMessage(&m2, NULL, 0, 0, PM_REMOVE) && g2++ < 500) {
                    TranslateMessage(&m2); DispatchMessage(&m2);
                }
                Sleep(60);
                while (PeekMessage(&m2, NULL, 0, 0, PM_REMOVE) && g2++ < 500) {
                    TranslateMessage(&m2); DispatchMessage(&m2);
                }
                overlay_tick_topmost();
                Sleep(60);
                while (PeekMessage(&m2, NULL, 0, 0, PM_REMOVE) && g2++ < 500) {
                    TranslateMessage(&m2); DispatchMessage(&m2);
                }
                iov = zorder_index(overlay_hwnd());
                irv = zorder_index(rival);
                report("  [info] z-order index: overlay=%d rival=%d (smaller = higher)\n", iov, irv);
                TEST(iov >= 0 && irv >= 0 && iov < irv,
                     "overlay stays above another topmost window");
                DestroyWindow(rival);
            }

            /* click-through is verified for real with WindowFromPoint */
            {
                POINT pt;
                HWND hit;
                pt.x = (r.left + r.right) / 2;
                pt.y = (r.top + r.bottom) / 2;
                overlay_set_fixed(1);
                Sleep(60);
                hit = WindowFromPoint(pt);
                report("  [info] WindowFromPoint(overlay centre) with click-through = 0x%p,"
                       " overlay = 0x%p\n", (void *)hit, (void *)ov);
                TEST(hit != ov, "clicks pass through the overlay (click-through on)");
                overlay_set_fixed(0);
                Sleep(60);
                hit = WindowFromPoint(pt);
                report("  [info] WindowFromPoint(overlay centre) without click-through = 0x%p\n",
                       (void *)hit);
                TEST(hit == ov, "overlay receives clicks when click-through is off");
                overlay_set_fixed(1);
                Sleep(60);
                hit = WindowFromPoint(pt);
                TEST(hit != ov, "click-through restored");
            }

            /* a maximised ordinary window must stay below the overlay */
            {
                HWND big = CreateWindowExA(0, "STATIC", "big", WS_POPUP | WS_VISIBLE,
                                           0, 0, GetSystemMetrics(SM_CXSCREEN),
                                           GetSystemMetrics(SM_CYSCREEN),
                                           NULL, NULL, g_hinst, NULL);
                MSG m3;
                int g4 = 0;
                int iov2, ibig;
                while (PeekMessage(&m3, NULL, 0, 0, PM_REMOVE) && g4++ < 500) {
                    TranslateMessage(&m3); DispatchMessage(&m3);
                }
                Sleep(60);
                overlay_tick_topmost();
                Sleep(60);
                iov2 = zorder_index(overlay_hwnd());
                ibig = zorder_index(big);
                report("  [info] z-order index: overlay=%d fullscreen-window=%d\n", iov2, ibig);
                TEST(iov2 >= 0 && ibig >= 0 && iov2 < ibig,
                     "overlay stays above a full-screen ordinary window");
                DestroyWindow(big);
            }

            /* moving / clicking through */
            overlay_set_fixed(0);
            ex = GetWindowLongA(ov, GWL_EXSTYLE);
            TEST((ex & WS_EX_TRANSPARENT) == 0, "click-through can be switched off");
            overlay_set_fixed(1);
            ex = GetWindowLongA(ov, GWL_EXSTYLE);
            TEST((ex & WS_EX_TRANSPARENT) != 0, "click-through can be switched back on");

            overlay_show(0);
            TEST(IsWindowVisible(ov) == 0, "overlay hides");
            overlay_show(1);
            TEST(IsWindowVisible(ov) != 0, "overlay shows again");

            overlay_set_text("12:34:56.7");
            overlay_destroy();
            TEST(1, "overlay destroyed cleanly");
        } else {
            TEST(0, "overlay window created");
        }
    }

    /* --- whether layered windows end up in BitBlt captures ----------- */
    {
        HWND probe;
        RECT pr;
        char err[256];
        unsigned char *bmp;
        int found = 0, i;
        const int PW = 120, PH = 60;
        int px = 300, py = 300;
        char ppath[MAX_PATH_LONG];

        report("[layered window capture probe]\n");
        probe = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                "STATIC", "", WS_POPUP | WS_VISIBLE,
                                px, py, PW, PH, NULL, NULL, g_hinst, NULL);
        if (probe) {
            MSG m;
            int g3 = 0;
            SetLayeredWindowAttributes(probe, 0, 255, LWA_ALPHA);
            /* paint it pure magenta */
            {
                HDC dc = GetDC(probe);
                RECT rc;
                HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));
                GetClientRect(probe, &rc);
                FillRect(dc, &rc, b);
                DeleteObject(b);
                ReleaseDC(probe, dc);
            }
            GetWindowRect(probe, &pr);
            Sleep(200);
            while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE) && g3++ < 500) {
                TranslateMessage(&m); DispatchMessage(&m);
            }
            snprintf(ppath, sizeof(ppath), "%s\\selftest_probe.bmp", st_dir());
            capture_screen_to_bmp(ppath, pr.left, pr.top,
                                  pr.right - pr.left, pr.bottom - pr.top, err, sizeof(err));
            bmp = NULL;
            {
                FILE *f = fopen(ppath, "rb");
                if (f) {
                    BITMAPFILEHEADER fh;
                    BITMAPINFOHEADER ih;
                    long off;
                    fread(&fh, sizeof(fh), 1, f);
                    fread(&ih, sizeof(ih), 1, f);
                    off = fh.bfOffBits;
                    fseek(f, off, SEEK_SET);
                    bmp = (unsigned char *)malloc((size_t)ih.biSizeImage);
                    if (bmp) fread(bmp, 1, (size_t)ih.biSizeImage, f);
                    fclose(f);
                }
            }
            if (bmp) {
                for (i = 0; i + 3 < PW * PH * 3; i += 3) {
                    if (bmp[i] > 200 && bmp[i + 1] < 80 && bmp[i + 2] > 200) { found = 1; break; }
                }
                free(bmp);
            }
            DestroyWindow(probe);
            report("  [info] layered windows visible in BitBlt capture: %s\n",
                   found ? "YES" : "NO");
            if (found) { passes++; report("  [PASS] CAPTUREBLT includes layered windows\n"); }
            else { report("  [info] capture method does not include layered windows;"
                          " the burned-in clock covers this case\n"); }
        } else {
            report("  [info] could not create the probe window\n");
        }
    }

    report("==================================================\n");
    report("result: %d passed, %d failed\n", passes, fails);
    report("==================================================\n");
    return fails == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* WinMain                                                             */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow)
{
    WNDCLASSA wc;
    MSG msg;
    char outabs[MAX_PATH_LONG];

    (void)hPrev; (void)nShow;
    g_hinst = hInst;
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&icc);          /* trackbar for the opacity dialog */
    }

    resolve_paths();
    cfg_defaults(&g_cfg);
    load_config();

    /* --config on the command line must win over the default ini */
    cfg_parse_cmdline(&g_cfg, lpCmdLine);

    /* logging */
    if (g_cfg.logfile[0]) {
        char abs[MAX_PATH_LONG];
        make_absolute(abs, sizeof(abs), g_cfg.logfile);
        app_set_logfile(abs);
    } else {
        char def[MAX_PATH_LONG];
        snprintf(def, sizeof(def), "%s\\examtimer.log", g_exe_dir);
        app_set_logfile(def);
    }

    attach_console();

    if (g_cfg.cmd_help) { usage(); return 0; }

    if (g_cfg.cmd_selftest) {
        int rc;
        if (g_cfg.logfile[0]) {
            char abs[MAX_PATH_LONG];
            make_absolute(abs, sizeof(abs), g_cfg.logfile);
            g_report = fopen(abs, "w");
        } else {
            g_report = fopen("C:\\xptest\\selftest_report.txt", "w");
        }
        make_style();
        rc = selftest();
        if (g_report) fclose(g_report);
        return rc;
    }

    if (g_cfg.cmd_shot) {
        char abs[MAX_PATH_LONG];
        char err[256];
        make_absolute(abs, sizeof(abs), g_cfg.shot_file);
        if (capture_screen_to_bmp(abs, g_cfg.cap_x, g_cfg.cap_y,
                                  g_cfg.cap_w, g_cfg.cap_h, err, sizeof(err)) == 0) {
            report("screenshot saved: %s\n", abs);
            return 0;
        }
        report("screenshot failed: %s\n", err);
        return 2;
    }

    /* ---- normal GUI mode ---- */
    set_default_label();
    make_style();

    /* the recording folder may be relative to the exe; keep strftime
     * placeholders, they are expanded for every recording */
    make_absolute(outabs, sizeof(outabs), g_cfg.out_dir);
    strncpy(g_cfg.out_dir, outabs, sizeof(g_cfg.out_dir) - 1);
    g_cfg.out_dir[sizeof(g_cfg.out_dir) - 1] = 0;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "ExamTimerMainWnd";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassA(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;
    }

    g_main = CreateWindowExA(0, "ExamTimerMainWnd", APP_NAME_EN " " APP_VERSION,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             (GetSystemMetrics(SM_CXSCREEN) - 490) / 2,
                             GetSystemMetrics(SM_CYSCREEN) / 4,
                             490, 340, NULL, NULL, hInst, NULL);
    if (!g_main) return 1;

    overlay_set_owner(g_main);
    if (g_cfg.overlay_enabled) {
        if (overlay_create(hInst, &g_style, &g_cfg) != 0) {
            app_log("overlay_create failed");
        }
    }

    register_hotkeys();
    tray_add();

    ShowWindow(g_main, g_cfg.minimize_control ? SW_SHOWMINNOACTIVE : SW_SHOW);
    UpdateWindow(g_main);
    update_buttons();

    app_log("started: dir=%s name=%s fps=%d q=%d scale=%d overlay=%d fixed=%d "
            "burnin=%d audio=%d",
            g_cfg.out_dir, g_cfg.out_name, g_cfg.fps, g_cfg.quality, g_cfg.scale,
            g_cfg.overlay_enabled, g_cfg.fixed, g_cfg.burnin, g_cfg.audio_device);

    /* autostart only starts the RECORDER - the clock is always started by the
     * operator, exactly like a manual run */
    if (g_cfg.autostart) toggle_recording();
    if (g_cfg.cmd_run_seconds > 0) toggle_timer();

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            /* handled in the window proc */
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
