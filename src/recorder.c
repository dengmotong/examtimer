/* recorder.c - screen capture -> burn-in -> JPEG -> MJPEG AVI pipeline.
 *
 * Runs on its own thread so the timer overlay stays perfectly smooth while
 * frames are captured and compressed.  The clock is painted straight into the
 * captured frame with exactly the same painter the live overlay uses, so the
 * saved video always shows the elapsed time even if a capture method refuses
 * to include layered windows.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "examtimer.h"
#include "avi.h"
#include "jpeg_enc.h"

/* ------------------------------------------------------------------ */
/* one-shot BMP screenshot (also used by the automated tests)          */
/* ------------------------------------------------------------------ */

int capture_screen_to_bmp(const char *file, int x, int y, int w, int h,
                          char *err, int errcap)
{
    HDC screen = NULL, mem = NULL;
    HBITMAP bmp = NULL, oldbmp = NULL;
    BITMAPINFO bi;
    void *bits = NULL;
    FILE *f = NULL;
    int row, stride, pad, ok = -1;

    if (w <= 0) {
        x = 0; y = 0;
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }
    if (w <= 0 || h <= 0) { if (err) snprintf(err, errcap, "invalid size"); return -1; }

    screen = GetDC(NULL);
    if (!screen) { if (err) snprintf(err, errcap, "GetDC failed"); return -1; }
    mem = CreateCompatibleDC(screen);
    if (!mem) { if (err) snprintf(err, errcap, "CreateCompatibleDC failed"); goto done; }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { if (err) snprintf(err, errcap, "CreateDIBSection failed"); goto done; }
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    if (!BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT)) {
        if (err) snprintf(err, errcap, "BitBlt failed (%lu)", GetLastError());
        goto done;
    }
    GdiFlush();

    f = fopen(file, "wb");
    if (!f) { if (err) snprintf(err, errcap, "cannot create %s", file); goto done; }

    stride = w * 4;
    pad = (4 - ((w * 3) % 4)) % 4;
    {
        BITMAPFILEHEADER fh;
        BITMAPINFOHEADER ih;
        unsigned int datasize = (unsigned int)((w * 3 + pad) * h);
        memset(&fh, 0, sizeof(fh));
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + datasize;
        memset(&ih, 0, sizeof(ih));
        ih.biSize = sizeof(ih);
        ih.biWidth = w;
        ih.biHeight = h;
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        ih.biCompression = BI_RGB;
        ih.biSizeImage = datasize;
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        for (row = h - 1; row >= 0; row--) {
            const unsigned char *src = (const unsigned char *)bits + (size_t)row * stride;
            int col;
            for (col = 0; col < w; col++) {
                fputc(src[col * 4 + 0], f);
                fputc(src[col * 4 + 1], f);
                fputc(src[col * 4 + 2], f);
            }
            for (col = 0; col < pad; col++) fputc(0, f);
        }
    }
    ok = 0;
    if (err) snprintf(err, errcap, "ok");

done:
    if (f) fclose(f);
    if (oldbmp) SelectObject(mem, oldbmp);
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(NULL, screen);
    return ok;
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static Config      g_cfg;
static TimerStyle  g_style;
static avi_writer *g_avi = NULL;

static HANDLE      g_thread = NULL;
static volatile LONG g_run = 0;
static volatile LONG g_pause = 0;
static int         g_want_audio = 0;
static char        g_out_path[MAX_PATH_LONG] = "";

static CRITICAL_SECTION g_cs;
static int         g_cs_ready = 0;
static char        g_timer_text[64] = "00:00:00.0";
static RecStatus   g_status;

/* capture objects, owned by the recorder thread */
static HDC         t_screen = NULL, t_mem = NULL, t_scale_dc = NULL;
static HBITMAP     t_bmp = NULL, t_oldbmp = NULL;
static HBITMAP     t_scale_bmp = NULL, t_scale_old = NULL;
static unsigned char *t_bits = NULL, *t_scale_bits = NULL;
static int         t_w = 0, t_h = 0, t_stride = 0;
static int         t_out_w = 0, t_out_h = 0, t_out_stride = 0;
static unsigned char *t_jpg = NULL, *t_prev_jpg = NULL;
static unsigned char *t_abuf = NULL;
static int         t_audio = 0;
static int         t_synth = 0;
static int         t_total_audio = 0;
static int         t_jpg_cap = 0, t_prev_len = 0;
static unsigned int t_prev_hash = 0;
static char        t_prev_text[64] = "";
static int         t_have_prev = 0;

static unsigned long tick_ms(void)
{
    static int init = 0;
    static double freq = 0.0;
    if (!init) {
        LARGE_INTEGER f;
        init = 1;
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) freq = (double)f.QuadPart;
    }
    if (freq > 0.0) {
        LARGE_INTEGER c;
        if (QueryPerformanceCounter(&c))
            return (unsigned long)((double)c.QuadPart * 1000.0 / freq);
    }
    return GetTickCount();
}

static unsigned int hash_frame(const unsigned char *p, int w, int h, int stride)
{
    unsigned int hh = 2166136261u;
    int y;
    for (y = 0; y < h; y++) {
        const unsigned int *q = (const unsigned int *)(p + (size_t)y * stride);
        int n = w;
        while (n-- > 0) {
            hh ^= *q++;
            hh *= 16777619u;
        }
    }
    return hh;
}

/* box-average / nearest-neighbour downscale (BGRA -> BGRA) */
static void scale_frame(void)
{
    int x, y;
    int fx = t_w / t_out_w;
    int fy = t_h / t_out_h;

    if (fx < 1) fx = 1;
    if (fy < 1) fy = 1;

    if (fx == 1 && fy == 1) {
        for (y = 0; y < t_out_h; y++) {
            int sy = (int)((long long)y * t_h / t_out_h);
            const unsigned char *src = t_bits + (size_t)sy * t_stride;
            unsigned char *dst = t_scale_bits + (size_t)y * t_out_stride;
            for (x = 0; x < t_out_w; x++) {
                int sx = (int)((long long)x * t_w / t_out_w);
                memcpy(dst + x * 4, src + (size_t)sx * 4, 4);
            }
        }
        return;
    }

    for (y = 0; y < t_out_h; y++) {
        int sy0 = y * fy;
        unsigned char *dst = t_scale_bits + (size_t)y * t_out_stride;
        for (x = 0; x < t_out_w; x++) {
            int sx0 = x * fx;
            unsigned int b = 0, g = 0, r = 0, n = 0;
            int dy, dx;
            for (dy = 0; dy < fy; dy++) {
                const unsigned char *s = t_bits + (size_t)(sy0 + dy) * t_stride + (size_t)sx0 * 4;
                for (dx = 0; dx < fx; dx++) {
                    b += s[0]; g += s[1]; r += s[2]; n++;
                    s += 4;
                }
            }
            dst[x * 4 + 0] = (unsigned char)(b / n);
            dst[x * 4 + 1] = (unsigned char)(g / n);
            dst[x * 4 + 2] = (unsigned char)(r / n);
            dst[x * 4 + 3] = 255;
        }
    }
}

/* Paint the clock into the captured frame at exactly the place the live
 * overlay occupies.  We ask the overlay for its real rectangle instead of
 * recomputing it from the configuration, so a dragged or resized window is
 * reproduced faithfully - and hiding the overlay also removes it from the
 * recording. */
static void burn_in(unsigned char *frame, int stride, int fw, int fh, const char *text)
{
    TimerStyle st = g_style;
    unsigned char *argb;
    int aw = 0, ah = 0, bx = 0, by = 0;
    RECT r;

    if (!g_cfg.burnin) return;
    if (!overlay_visible()) return;          /* 隐藏计时器 = 录像里也不出现 */

    {
        /* the operator may resize the clock while recording */
        int live = 0;
        overlay_get_geometry(NULL, NULL, NULL, &live);
        if (live > 0) st.fontsize = live;
    }
    if (g_cfg.scale != 100) {
        st.fontsize = st.fontsize * g_cfg.scale / 100;
        if (st.fontsize < 8) st.fontsize = 8;
    }

    argb = timer_box_render_argb(&st, text, 1, &aw, &ah);
    if (!argb) return;

    if (overlay_hwnd() != NULL && overlay_rect(&r) == 0) {
        bx = r.left - g_cfg.cap_x;
        by = r.top - g_cfg.cap_y;
    } else {
        int w2 = aw, h2 = ah;
        timer_box_anchor(&g_cfg, w2, h2, &bx, &by);
        bx -= g_cfg.cap_x;
        by -= g_cfg.cap_y;
    }
    if (g_cfg.scale != 100) {
        bx = bx * g_cfg.scale / 100;
        by = by * g_cfg.scale / 100;
    }
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (aw < fw && bx + aw > fw) bx = fw - aw;
    if (ah < fh && by + ah > fh) by = fh - ah;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;

    timer_box_blit_argb(frame, stride, fw, fh, argb, aw, ah, bx, by);
    free(argb);
}

static int setup_capture(void)
{
    BITMAPINFO bi;

    if (g_cfg.cap_w <= 0 || g_cfg.cap_h <= 0) {
        g_cfg.cap_x = 0;
        g_cfg.cap_y = 0;
        g_cfg.cap_w = GetSystemMetrics(SM_CXSCREEN);
        g_cfg.cap_h = GetSystemMetrics(SM_CYSCREEN);
    }
    t_w = g_cfg.cap_w;
    t_h = g_cfg.cap_h;
    t_out_w = t_w * g_cfg.scale / 100;
    t_out_h = t_h * g_cfg.scale / 100;
    if (t_out_w < 16) t_out_w = 16;
    if (t_out_h < 16) t_out_h = 16;
    if (t_out_w & 1) t_out_w--;
    if (t_out_h & 1) t_out_h--;
    t_stride = t_w * 4;
    t_out_stride = t_out_w * 4;

    t_screen = GetDC(NULL);
    if (!t_screen) return -1;
    t_mem = CreateCompatibleDC(t_screen);
    if (!t_mem) return -1;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = t_w;
    bi.bmiHeader.biHeight = -t_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    t_bmp = CreateDIBSection(t_screen, &bi, DIB_RGB_COLORS, (void **)&t_bits, NULL, 0);
    if (!t_bmp || !t_bits) return -1;
    t_oldbmp = (HBITMAP)SelectObject(t_mem, t_bmp);

    if (g_cfg.scale != 100) {
        bi.bmiHeader.biWidth = t_out_w;
        bi.bmiHeader.biHeight = -t_out_h;
        t_scale_bmp = CreateDIBSection(t_screen, &bi, DIB_RGB_COLORS,
                                       (void **)&t_scale_bits, NULL, 0);
        if (!t_scale_bmp || !t_scale_bits) return -1;
        t_scale_dc = CreateCompatibleDC(t_screen);
        if (!t_scale_dc) return -1;
        t_scale_old = (HBITMAP)SelectObject(t_scale_dc, t_scale_bmp);
    }

    t_jpg_cap = jpeg_enc_bound(t_out_w, t_out_h);
    t_jpg = (unsigned char *)malloc((size_t)t_jpg_cap);
    t_prev_jpg = (unsigned char *)malloc((size_t)t_jpg_cap);
    if (!t_jpg || !t_prev_jpg) return -1;
    t_prev_len = 0;
    t_have_prev = 0;
    return 0;
}

static void teardown_capture(void)
{
    if (t_scale_old) SelectObject(t_scale_dc, t_scale_old);
    if (t_scale_bmp) DeleteObject(t_scale_bmp);
    if (t_scale_dc) DeleteDC(t_scale_dc);
    if (t_oldbmp) SelectObject(t_mem, t_oldbmp);
    if (t_bmp) DeleteObject(t_bmp);
    if (t_mem) DeleteDC(t_mem);
    if (t_screen) ReleaseDC(NULL, t_screen);
    free(t_jpg); t_jpg = NULL;
    free(t_prev_jpg); t_prev_jpg = NULL;
    t_oldbmp = NULL; t_bmp = NULL; t_mem = NULL; t_screen = NULL;
    t_scale_old = NULL; t_scale_bmp = NULL; t_scale_dc = NULL;
    t_bits = NULL; t_scale_bits = NULL;
}

/* Move every captured PCM byte into the AVI.  Called just before each video
 * frame so the two streams stay interleaved in real time. */
static int pump_audio(int interval_ms)
{
    int n;
    if (!t_audio) return 0;
    if (t_synth) audio_synth_fill(interval_ms > 0 ? interval_ms : 200);
    while ((n = audio_read(t_abuf, 65536)) > 0) {
        if (avi_add_audio(g_avi, t_abuf, n) != 0) return -1;
        t_total_audio += n;
        if (n < 65536) break;
    }
    return 0;
}

static void set_status_error(const char *msg)
{
    EnterCriticalSection(&g_cs);
    g_status.error = 1;
    snprintf(g_status.message, sizeof(g_status.message), "%s", msg ? msg : "error");
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI rec_thread(LPVOID param)
{
    char path[MAX_PATH_LONG];
    int interval;
    WAVEFORMATEX afmt;
    const WAVEFORMATEX *pafmt = NULL;
    unsigned long next, started;
    int frames_written = 0;
    long long total_process_ms = 0, total_write_ms = 0;

    (void)param;

    t_audio = 0;
    t_synth = 0;
    t_total_audio = 0;
    if (g_want_audio) {
        if (audio_init(&g_cfg) == 0 && g_cfg.audio_device != -1) {
            AudioStatus as;
            audio_status(&as);
            t_audio = 1;
            t_synth = (g_cfg.audio_device == -3);
            memset(&afmt, 0, sizeof(afmt));
            afmt.wFormatTag = WAVE_FORMAT_PCM;
            afmt.nChannels = (WORD)as.channels;
            afmt.nSamplesPerSec = (DWORD)as.rate;
            afmt.wBitsPerSample = (WORD)as.bits;
            afmt.nBlockAlign = (WORD)(as.channels * as.bits / 8);
            afmt.nAvgBytesPerSec = afmt.nSamplesPerSec * afmt.nBlockAlign;
            pafmt = &afmt;
            audio_start();
            cfg_log(&g_cfg, "audio: %s (%s) %d Hz %dch %dbit",
                    as.name, t_synth ? "synthetic" : "device",
                    as.rate, as.channels, as.bits);
        } else {
            AudioStatus as;
            audio_status(&as);
            cfg_log(&g_cfg, "audio unavailable: %s", as.message);
        }
    }

    if (setup_capture() != 0) {
        if (t_audio) audio_shutdown();
        set_status_error("初始化屏幕捕获失败");
        app_request_stop("capture-init");
        return 1;
    }

    /* main.c already expanded the timestamp placeholders for this run */
    snprintf(path, sizeof(path), "%s", g_out_path);
    path_ensure_dir(path);
    {
        long long split = (long long)g_cfg.split_mb * 1024LL * 1024LL;
        if (split <= 0 || split > 1800LL * 1024 * 1024)
            split = 1800LL * 1024 * 1024;
        g_avi = avi_open(path, t_out_w, t_out_h, g_cfg.fps, split, pafmt);
    }
    if (!g_avi) {
        set_status_error("无法创建录像文件");
        if (t_audio) audio_shutdown();
        teardown_capture();
        app_request_stop("avi-open");
        return 1;
    }
    t_abuf = (unsigned char *)malloc(65536);

    EnterCriticalSection(&g_cs);
    snprintf(g_status.file, sizeof(g_status.file), "%s", avi_current_path(g_avi));
    g_status.capture_w = t_out_w;
    g_status.capture_h = t_out_h;
    g_status.active = 1;
    g_status.error = 0;
    snprintf(g_status.message, sizeof(g_status.message), "recording");
    LeaveCriticalSection(&g_cs);

    cfg_log(&g_cfg, "recorder started: %s %dx%d fps=%d q=%d scale=%d burnin=%d blt=%d",
            avi_current_path(g_avi), t_out_w, t_out_h, g_cfg.fps, g_cfg.quality,
            g_cfg.scale, g_cfg.burnin, g_cfg.captureblt);

    interval = 1000 / (g_cfg.fps > 0 ? g_cfg.fps : 5);
    if (interval < 1) interval = 1;
    next = tick_ms();
    started = next;

    while (InterlockedCompareExchange(&g_run, 1, 1) == 1) {
        unsigned long t0, t1, t2;
        int enc_len;
        unsigned int h;
        unsigned char *enc_src;
        HDC enc_dc;
        int enc_stride;
        char txt[64];

        if (InterlockedCompareExchange(&g_pause, 1, 1) == 1) {
            if (t_audio) audio_pause(1);
            Sleep(50);
            next = tick_ms() + interval;
            continue;
        }
        if (t_audio) audio_pause(0);

        t0 = tick_ms();

        if (!BitBlt(t_mem, 0, 0, t_w, t_h, t_screen, g_cfg.cap_x, g_cfg.cap_y,
                    SRCCOPY | (g_cfg.captureblt ? CAPTUREBLT : 0))) {
            set_status_error("屏幕捕获 BitBlt 失败");
            break;
        }
        GdiFlush();

        if (g_cfg.scale != 100) {
            scale_frame();
            enc_src = t_scale_bits;
            enc_dc = t_scale_dc;
            enc_stride = t_out_stride;
        } else {
            enc_src = t_bits;
            enc_dc = t_mem;
            enc_stride = t_stride;
        }

        EnterCriticalSection(&g_cs);
        strncpy(txt, g_timer_text, sizeof(txt) - 1);
        txt[sizeof(txt) - 1] = 0;
        LeaveCriticalSection(&g_cs);

        h = hash_frame(enc_src, t_out_w, t_out_h, enc_stride);

        if (t_have_prev && g_cfg.skip_identical &&
            strcmp(txt, t_prev_text) == 0 && h == t_prev_hash) {
            memcpy(t_jpg, t_prev_jpg, (size_t)t_prev_len);
            enc_len = t_prev_len;
        } else {
            if (g_cfg.burnin && overlay_visible())
                burn_in(enc_src, enc_stride, t_out_w, t_out_h, txt);

            enc_len = jpeg_encode_bgr_to(enc_src, t_out_w, t_out_h, enc_stride, 4,
                                         g_cfg.quality, g_cfg.subsample,
                                         t_jpg, t_jpg_cap);
            if (enc_len < 0) {
                set_status_error("JPEG 编码失败");
                break;
            }
            memcpy(t_prev_jpg, t_jpg, (size_t)enc_len);
            t_prev_len = enc_len;
            t_prev_hash = h;
            strncpy(t_prev_text, txt, sizeof(t_prev_text) - 1);
            t_prev_text[sizeof(t_prev_text) - 1] = 0;
            t_have_prev = 1;
        }

        if (pump_audio(interval) != 0) {
            set_status_error("写入音频失败（磁盘已满？）");
            break;
        }

        t1 = tick_ms();

        if (avi_add_jpeg(g_avi, t_jpg, enc_len) != 0) {
            set_status_error("写入录像文件失败（磁盘已满？）");
            break;
        }
        frames_written++;
        t2 = tick_ms();
        total_process_ms += (long long)(t1 - t0);
        total_write_ms += (long long)(t2 - t1);

        EnterCriticalSection(&g_cs);
        g_status.frames = frames_written;
        g_status.bytes = avi_total_bytes(g_avi);
        g_status.parts = avi_part_count(g_avi);
        g_status.encode_ms_last = (int)(t1 - t0);
        g_status.elapsed_sec = (int)((t2 - started) / 1000);
        snprintf(g_status.file, sizeof(g_status.file), "%s", avi_current_path(g_avi));
        g_status.fps_actual_x10 =
            (int)((unsigned long long)frames_written * 10000ull /
                  (unsigned long long)(t2 - started + 1));
        g_status.audio_bytes = t_total_audio;
        g_status.audio_active = t_audio;
        LeaveCriticalSection(&g_cs);

        next += (unsigned long)interval;
        {
            unsigned long now = tick_ms();
            if (next > now) Sleep(next - now);
            else next = now;
        }
    }

    {
        long long tb = g_avi ? avi_total_bytes(g_avi) : 0;
        int tf = g_avi ? avi_total_frames(g_avi) : 0;
        int tp = g_avi ? avi_part_count(g_avi) : 0;
        if (g_avi) { avi_close(g_avi); g_avi = NULL; }
        teardown_capture();

        {
            unsigned long endt = tick_ms();
            long long wall = (long long)(endt - started);
            cfg_log(&g_cfg,
                    "recorder stopped: frames=%d parts=%d bytes=%I64d wall_ms=%I64d "
                    "avg_capture_encode_ms=%.1f avg_write_ms=%.1f real_fps=%.2f kbps=%.0f",
                    tf, tp, tb, wall,
                    frames_written ? (double)(total_process_ms) / frames_written : 0.0,
                    frames_written ? (double)(total_write_ms) / frames_written : 0.0,
                    wall > 0 ? frames_written * 1000.0 / (double)wall : 0.0,
                    wall > 0 ? tb * 8.0 / (double)wall : 0.0);
        }
    }

    EnterCriticalSection(&g_cs);
    g_status.active = 0;
    snprintf(g_status.message, sizeof(g_status.message), "stopped");
    LeaveCriticalSection(&g_cs);

    app_request_stop("stopped");
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int recorder_start(const Config *cfg, const TimerStyle *style, const char *initial_text,
                   const char *output_path, int with_audio)
{
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = 1; }
    if (InterlockedCompareExchange(&g_run, 1, 1) == 1) return -1;

    g_cfg = *cfg;
    g_want_audio = with_audio;
    if (output_path && output_path[0]) {
        snprintf(g_out_path, sizeof(g_out_path), "%s", output_path);
    } else {
        g_out_path[0] = 0;
    }
    g_style = *style;
    memset(&g_status, 0, sizeof(g_status));
    /* set the clock text *before* the thread starts, so even the very first
     * frame of the recording carries the right time */
    if (initial_text && initial_text[0]) {
        strncpy(g_timer_text, initial_text, sizeof(g_timer_text) - 1);
        g_timer_text[sizeof(g_timer_text) - 1] = 0;
    } else {
        strcpy(g_timer_text, "00:00:00.0");
    }

    InterlockedExchange(&g_pause, 0);
    InterlockedExchange(&g_run, 1);
    g_thread = CreateThread(NULL, 0, rec_thread, NULL, 0, NULL);
    if (!g_thread) {
        InterlockedExchange(&g_run, 0);
        return -1;
    }
    return 0;
}

void recorder_stop(void)
{
    if (InterlockedCompareExchange(&g_run, 1, 1) != 1) return;
    InterlockedExchange(&g_run, 0);
    if (g_thread) {
        WaitForSingleObject(g_thread, 20000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

void recorder_set_paused(int paused)
{
    InterlockedExchange(&g_pause, paused ? 1 : 0);
}

int recorder_set_timer_text(const char *text)
{
    if (!text || !g_cs_ready) return -1;
    EnterCriticalSection(&g_cs);
    strncpy(g_timer_text, text, sizeof(g_timer_text) - 1);
    g_timer_text[sizeof(g_timer_text) - 1] = 0;
    LeaveCriticalSection(&g_cs);
    return 0;
}

void recorder_get_status(RecStatus *out)
{
    if (!out) return;
    if (!g_cs_ready) { memset(out, 0, sizeof(*out)); return; }
    EnterCriticalSection(&g_cs);
    *out = g_status;
    LeaveCriticalSection(&g_cs);
}
