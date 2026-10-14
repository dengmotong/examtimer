/* overlay.c - the always-on-top timer overlay and the shared painter.
 *
 * Requirements implemented here:
 *   * the clock sits at the very top of the screen, top-centre by default
 *   * it is a WS_EX_TOPMOST tool window, so ordinary windows cannot cover it
 *   * it re-asserts HWND_TOPMOST periodically to beat other topmost windows
 *   * it is click-through by default so it never blocks the operator
 *   * the very same painter is used to burn the clock into the recording,
 *     which guarantees the elapsed time is visible in the saved video.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "examtimer.h"

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

int text_to_wide(const char *s, wchar_t *out, int outcap)
{
    int n;
    if (!s || !out || outcap <= 0) return 0;
    out[0] = 0;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, outcap);
    if (n <= 0)
        n = MultiByteToWideChar(CP_ACP, 0, s, -1, out, outcap);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static HWND        g_hwnd = NULL;
static HWND        g_owner = NULL;
static TimerStyle  g_style;
static Config      g_anchor;              /* pos / offset_x / offset_y only */
static char        g_text[64] = "00:00:00.0";
static int         g_recording = 0;
static RECT        g_rect = { 0, 0, 0, 0 };
static int         g_visible = 1;
static int         g_fixed = 1;
static int         g_auto_topmost = 500;
static int         g_dragging = 0;
static POINT       g_drag_off;
static HFONT       g_font_big = NULL;
static HFONT       g_font_small = NULL;
static int         g_font_px = 0;
static CRITICAL_SECTION g_cs;
static LONG        g_cs_state = 0;   /* 0 = not yet, 1 = initialising, 2 = ready */

/* Every public entry point calls this, so the overlay module is safe to use
 * even when no overlay window is ever created (--no-overlay) or when window
 * creation fails.  Entering an uninitialised CRITICAL_SECTION deadlocks on
 * Windows XP, which used to hang the whole UI thread. */
static void ensure_cs(void)
{
    if (g_cs_state == 2) return;
    if (InterlockedCompareExchange(&g_cs_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        InterlockedExchange(&g_cs_state, 2);
    } else {
        while (g_cs_state != 2) Sleep(0);
    }
}

#define OVERLAY_CLASS "ExamTimerOverlayWnd"

typedef struct {
    int pad_x, pad_y, gap;
    int dot_d, label_w, time_w;
    int big_h, small_h;
    int w, h;
} Layout;

static Config *overlay_cfg_ptr(void) { return &g_anchor; }

static HFONT make_font(int px)
{
    return CreateFontA(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
}

static void ensure_fonts(int px)
{
    if (g_font_px == px && g_font_big && g_font_small) return;
    if (g_font_big) DeleteObject(g_font_big);
    if (g_font_small) DeleteObject(g_font_small);
    g_font_big = make_font(px);
    g_font_small = make_font(px > 30 ? px * 2 / 5 : 13);
    g_font_px = px;
}

/* ------------------------------------------------------------------ */
/* layout + painting                                                   */
/* ------------------------------------------------------------------ */

static void layout_compute(HDC hdc, const TimerStyle *st, const char *text,
                           Layout *L)
{
    wchar_t wtext[128], wlabel[256];
    SIZE s;
    HFONT ob;

    ensure_fonts(st->fontsize);

    L->pad_x = st->fontsize / 2; if (L->pad_x < 8) L->pad_x = 8;
    L->pad_y = st->fontsize / 4; if (L->pad_y < 5) L->pad_y = 5;
    L->gap   = st->fontsize / 3; if (L->gap   < 6) L->gap   = 6;

    ob = (HFONT)SelectObject(hdc, g_font_big);
    text_to_wide(text, wtext, 128);
    GetTextExtentPoint32W(hdc, wtext, (int)wcslen(wtext), &s);
    L->time_w = s.cx;
    L->big_h  = s.cy;

    L->label_w = 0;
    L->small_h = 0;
    if (st->show_label && st->label[0]) {
        SelectObject(hdc, g_font_small);
        text_to_wide(st->label, wlabel, 256);
        GetTextExtentPoint32W(hdc, wlabel, (int)wcslen(wlabel), &s);
        L->label_w = s.cx;
        L->small_h = s.cy;
    }
    SelectObject(hdc, ob);

    L->dot_d = st->show_rec_dot ? (st->fontsize / 3) : 0;

    L->h = L->big_h + L->pad_y * 2;
    L->w = L->pad_x * 2 + L->time_w;
    if (L->label_w) L->w += L->label_w + L->gap;
    if (L->dot_d)   L->w += L->dot_d + L->gap;
}

void timer_box_size(HDC hdc, const TimerStyle *st, const char *text,
                    int recording, int *w, int *h)
{
    Layout L;
    (void)recording;
    layout_compute(hdc, st, text, &L);
    if (w) *w = L.w;
    if (h) *h = L.h;
}

/* Paint the box shapes with the given colours.  When `mask_mode` is set the
 * caller wants a coverage mask (white on black) instead of the real colours;
 * that mask is then composited with the wanted per-element transparency. */
static void paint_shapes(HDC hdc, const TimerStyle *st, const char *text,
                         int recording, int x, int y, int w, int h,
                         int mask_mode, int solid)
{
    Layout L;
    HBRUSH br, old_br;
    HPEN pen, old_pen;
    HFONT ob;
    int cx, ty;
    wchar_t wtext[128], wlabel[256];

    layout_compute(hdc, st, text, &L);
    if (w <= 0) w = L.w;
    if (h <= 0) h = L.h;

    if (mask_mode) {
        /* The mask carries only the FOREGROUND layer (border + dot + text).
         * The background plate is a separate translucent layer, so it must
         * NOT appear here - otherwise the whole box would be painted in the
         * text colour. */
        if (st->show_border) {
            pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            old_br = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            old_pen = (HPEN)SelectObject(hdc, pen);
            Rectangle(hdc, x, y, x + w, y + h);
            SelectObject(hdc, old_br);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
        }
    } else {
        br = CreateSolidBrush(st->bg);
        pen = CreatePen(PS_SOLID, st->show_border ? 2 : 0,
                        st->show_border ? st->border : st->bg);
        old_br = (HBRUSH)SelectObject(hdc, br);
        old_pen = (HPEN)SelectObject(hdc, pen);
        Rectangle(hdc, x, y, x + w, y + h);
        SelectObject(hdc, old_br);
        SelectObject(hdc, old_pen);
        DeleteObject(br);
        DeleteObject(pen);
    }

    SetBkMode(hdc, TRANSPARENT);
    cx = x + L.pad_x;

    if (recording && st->show_rec_dot && L.dot_d > 0) {
        HBRUSH dbr;
        int on = 1;
        if (!solid) on = ((GetTickCount() / 500) & 1) ? 1 : 0;
        dbr = CreateSolidBrush(mask_mode ? RGB(255, 255, 255)
                              : (on ? RGB(232, 40, 40) : RGB(96, 24, 24)));
        old_br = (HBRUSH)SelectObject(hdc, dbr);
        old_pen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        {
            int dy = y + (h - L.dot_d) / 2;
            Ellipse(hdc, cx, dy, cx + L.dot_d, dy + L.dot_d);
        }
        SelectObject(hdc, old_br);
        SelectObject(hdc, old_pen);
        DeleteObject(dbr);
        cx += L.dot_d + L.gap;
    }

    if (L.label_w) {
        ob = (HFONT)SelectObject(hdc, g_font_small);
        SetTextColor(hdc, mask_mode ? RGB(255, 255, 255) : st->fg);
        text_to_wide(st->label, wlabel, 256);
        ty = y + (h - L.small_h) / 2;
        TextOutW(hdc, cx, ty, wlabel, (int)wcslen(wlabel));
        SelectObject(hdc, ob);
        cx += L.label_w + L.gap;
    }

    ob = (HFONT)SelectObject(hdc, g_font_big);
    SetTextColor(hdc, mask_mode ? RGB(255, 255, 255) : st->fg);
    text_to_wide(text, wtext, 128);
    ty = y + (h - L.big_h) / 2;
    TextOutW(hdc, cx, ty, wtext, (int)wcslen(wtext));
    SelectObject(hdc, ob);
}

void timer_box_paint(HDC hdc, const TimerStyle *st, const char *text,
                     int recording, int x, int y, int w, int h, int solid)
{
    paint_shapes(hdc, st, text, recording, x, y, w, h, 0, solid);
}

/* ------------------------------------------------------------------ */
/* premultiplied ARGB rendering (independent text / background alpha)  */
/* ------------------------------------------------------------------ */

static unsigned char *make_dib(HDC ref, int w, int h, HDC *dc_out, HBITMAP *bmp_out)
{
    BITMAPINFO bi;
    HDC dc;
    HBITMAP bmp;
    void *bits = NULL;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;             /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(ref);
    if (!dc) return NULL;
    bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { DeleteDC(dc); if (bmp) DeleteObject(bmp); return NULL; }
    SelectObject(dc, bmp);
    *dc_out = dc;
    *bmp_out = bmp;
    return (unsigned char *)bits;
}

unsigned char *timer_box_render_argb(const TimerStyle *st, const char *text,
                                     int recording, int *w_out, int *h_out)
{
    HDC screen, mdc = NULL;
    HBITMAP mbmp = NULL;
    unsigned char *mask = NULL, *out = NULL;
    int w, h, i;
    int fa, ba;

    screen = GetDC(NULL);
    if (!screen) return NULL;
    {
        HDC tmp = GetDC(NULL);
        timer_box_size(tmp, st, text, recording, &w, &h);
        ReleaseDC(NULL, tmp);
    }
    if (w <= 0 || h <= 0) { ReleaseDC(NULL, screen); return NULL; }

    mask = make_dib(screen, w, h, &mdc, &mbmp);
    out = (unsigned char *)calloc((size_t)w * h * 4, 1);
    if (!mask || !out) goto done;

    /* coverage mask: white shapes on black */
    {
        RECT rc = { 0, 0, w, h };
        HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(mdc, &rc, bk);
        DeleteObject(bk);
    }
    paint_shapes(mdc, st, text, recording, 0, 0, w, h, 1, 1);
    GdiFlush();

    fa = st->fg_alpha; if (fa < 0) fa = 0; if (fa > 255) fa = 255;
    ba = st->bg_alpha; if (ba < 0) ba = 0; if (ba > 255) ba = 255;

    {
        int fg_b = GetBValue(st->fg), fg_g = GetGValue(st->fg), fg_r = GetRValue(st->fg);
        int bg_b = GetBValue(st->bg), bg_g = GetGValue(st->bg), bg_r = GetRValue(st->bg);
        int bt_b = bg_b * ba / 255, bt_g = bg_g * ba / 255, bt_r = bg_r * ba / 255;

        for (i = 0; i < w * h; i++) {
            int cov = mask[i * 4];            /* text/border coverage 0..255 */
            int sa = cov * fa / 255;          /* source alpha               */
            int inv = 255 - sa;
            /* premultiplied "source over destination", destination = the
             * translucent background plate */
            out[i * 4 + 0] = (unsigned char)((fg_b * sa + bt_b * inv) / 255);
            out[i * 4 + 1] = (unsigned char)((fg_g * sa + bt_g * inv) / 255);
            out[i * 4 + 2] = (unsigned char)((fg_r * sa + bt_r * inv) / 255);
            out[i * 4 + 3] = (unsigned char)(sa + ba * inv / 255);
        }
    }

    if (w_out) *w_out = w;
    if (h_out) *h_out = h;

done:
    if (mdc) DeleteDC(mdc);
    if (mbmp) DeleteObject(mbmp);
    ReleaseDC(NULL, screen);
    return out ? out : (unsigned char *)NULL;   /* caller frees */
}

void timer_box_blit_argb(unsigned char *dst, int dst_stride, int dst_w, int dst_h,
                         const unsigned char *argb, int aw, int ah, int x, int y)
{
    int row, col;
    if (!dst || !argb) return;
    for (row = 0; row < ah; row++) {
        int dy = y + row;
        const unsigned char *sp;
        unsigned char *dp;
        if (dy < 0 || dy >= dst_h) continue;
        sp = argb + (size_t)row * aw * 4;
        dp = dst + (size_t)dy * dst_stride;
        for (col = 0; col < aw; col++) {
            int dx = x + col;
            int a, ia;
            unsigned char *p;
            if (dx < 0 || dx >= dst_w) continue;
            p = dp + (size_t)dx * 4;
            a = sp[col * 4 + 3];
            if (a == 0) continue;
            ia = 255 - a;
            /* premultiplied source over opaque destination */
            p[0] = (unsigned char)(sp[col * 4 + 0] + p[0] * ia / 255);
            p[1] = (unsigned char)(sp[col * 4 + 1] + p[1] * ia / 255);
            p[2] = (unsigned char)(sp[col * 4 + 2] + p[2] * ia / 255);
            p[3] = 255;
        }
    }
}

/* ------------------------------------------------------------------ */
/* anchoring                                                           */
/* ------------------------------------------------------------------ */

void timer_box_anchor(const Config *cfg, int box_w, int box_h, int *x, int *y)
{
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int px, py;

    switch (cfg->pos) {
        case POS_TOP_LEFT:   px = cfg->offset_x; py = cfg->offset_y; break;
        case POS_TOP_RIGHT:  px = sw - box_w - cfg->offset_x; py = cfg->offset_y; break;
        case POS_CUSTOM:     px = cfg->offset_x; py = cfg->offset_y; break;
        case POS_TOP_CENTER:
        default:             px = (sw - box_w) / 2 + cfg->offset_x; py = cfg->offset_y; break;
    }
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px + box_w > sw) px = sw - box_w;
    if (py + box_h > sh) py = sh - box_h;
    *x = px;
    *y = py;
}

/* ------------------------------------------------------------------ */
/* window                                                              */
/* ------------------------------------------------------------------ */

static unsigned char *g_pix = NULL;          /* premultiplied BGRA      */
static int      g_pw = 0, g_ph = 0;
static HDC      g_pdc = NULL;
static HBITMAP  g_pbmp = NULL, g_pold = NULL;
static void    *g_pbits = NULL;
static int      g_resizing = 0;
static int      g_resize_font0 = 0;
static POINT    g_resize_org;

#define RESIZE_GRIP 14

static void pix_free(void)
{
    if (g_pold && g_pdc) SelectObject(g_pdc, g_pold);
    if (g_pbmp) DeleteObject(g_pbmp);
    if (g_pdc) DeleteDC(g_pdc);
    g_pold = NULL; g_pbmp = NULL; g_pdc = NULL; g_pbits = NULL;
    free(g_pix); g_pix = NULL;
    g_pw = g_ph = 0;
}

static int pix_ensure(int w, int h)
{
    BITMAPINFO bi;
    HDC screen;
    if (w <= 0 || h <= 0) return -1;
    if (g_pix && g_pw == w && g_ph == h && g_pdc) return 0;
    pix_free();

    screen = GetDC(NULL);
    if (!screen) return -1;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_pdc = CreateCompatibleDC(screen);
    g_pbmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_pbits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!g_pdc || !g_pbmp || !g_pbits) { pix_free(); return -1; }
    g_pold = (HBITMAP)SelectObject(g_pdc, g_pbmp);
    g_pix = (unsigned char *)malloc((size_t)w * h * 4);
    if (!g_pix) { pix_free(); return -1; }
    g_pw = w; g_ph = h;
    return 0;
}

/* render the clock into g_pix and push it to the layered window */
static void overlay_present(HWND hwnd)
{
    unsigned char *argb;
    int w = 0, h = 0;
    HDC screen;
    POINT dst, src;
    SIZE sz;
    BLENDFUNCTION bf;

    argb = timer_box_render_argb(&g_style, g_text, g_recording, &w, &h);
    if (!argb) return;
    if (pix_ensure(w, h) != 0) { free(argb); return; }
    memcpy(g_pix, argb, (size_t)w * h * 4);
    free(argb);
    memcpy(g_pbits, g_pix, (size_t)w * h * 4);

    g_rect.right = g_rect.left + w;
    g_rect.bottom = g_rect.top + h;

    screen = GetDC(NULL);
    dst.x = g_rect.left; dst.y = g_rect.top;
    src.x = 0; src.y = 0;
    sz.cx = w; sz.cy = h;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, screen, &dst, &sz, g_pdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

/* move without re-rendering (used while dragging) */
static void overlay_push_position(HWND hwnd)
{
    HDC screen;
    POINT dst, src;
    SIZE sz;
    BLENDFUNCTION bf;
    if (!g_pdc || !g_pbits) return;
    screen = GetDC(NULL);
    dst.x = g_rect.left; dst.y = g_rect.top;
    src.x = 0; src.y = 0;
    sz.cx = g_pw; sz.cy = g_ph;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, screen, &dst, &sz, g_pdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

static void overlay_resize(HWND hwnd)
{
    HDC hdc = GetDC(hwnd);
    int w = 0, h = 0, x = 0, y = 0;
    RECT wr;

    timer_box_size(hdc, &g_style, g_text, g_recording, &w, &h);
    ReleaseDC(hwnd, hdc);

    timer_box_anchor(&g_anchor, w, h, &x, &y);

    GetWindowRect(hwnd, &wr);
    if (wr.left == x && wr.top == y &&
        wr.right - wr.left == w && wr.bottom - wr.top == h && g_pix) {
        return;                        /* nothing changed: do not touch it */
    }
    g_rect.left = x;
    g_rect.top = y;
    overlay_present(hwnd);
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == 1) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (wp == 2) {
            overlay_present(hwnd);
        }
        return 0;
    case WM_NCHITTEST:
        if (g_fixed) return HTTRANSPARENT;
        break;
    case WM_LBUTTONDOWN:
        if (!g_fixed) {
            POINT pt;
            GetCursorPos(&pt);
            g_drag_off.x = pt.x - g_rect.left;
            g_drag_off.y = pt.y - g_rect.top;
            /* near the bottom-right corner -> resize instead of move */
            if (g_drag_off.x >= (g_rect.right - g_rect.left) - RESIZE_GRIP &&
                g_drag_off.y >= (g_rect.bottom - g_rect.top) - RESIZE_GRIP) {
                g_resizing = 1;
                g_resize_font0 = g_style.fontsize;
                g_resize_org.x = g_rect.right - g_rect.left;
                g_resize_org.y = g_rect.bottom - g_rect.top;
            } else {
                g_dragging = 1;
            }
            SetCapture(hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_resizing) {
            POINT pt;
            int neww, newh, base, ns;
            GetCursorPos(&pt);
            neww = (pt.x - g_rect.left) - g_drag_off.x + (g_rect.right - g_rect.left);
            newh = (pt.y - g_rect.top) - g_drag_off.y + (g_rect.bottom - g_rect.top);
            if (neww < 20) neww = 20;
            if (newh < 12) newh = 12;
            base = g_resize_org.y > 0 ? g_resize_org.y : g_ph;
            ns = g_resize_font0 * newh / (base > 0 ? base : 1);
            if (ns < 8) ns = 8;
            if (ns > 400) ns = 400;
            if (ns != g_style.fontsize) {
                g_style.fontsize = ns;
                overlay_resize(hwnd);
            }
        } else if (g_dragging) {
            POINT pt;
            int nx, ny, w, h;
            GetCursorPos(&pt);
            nx = pt.x - g_drag_off.x;
            ny = pt.y - g_drag_off.y;
            w = g_rect.right - g_rect.left;
            h = g_rect.bottom - g_rect.top;
            g_rect.left = nx; g_rect.top = ny;
            g_rect.right = nx + w; g_rect.bottom = ny + h;
            g_anchor.pos = POS_CUSTOM;
            g_anchor.offset_x = nx;
            g_anchor.offset_y = ny;
            overlay_push_position(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging || g_resizing) {
            g_dragging = 0;
            g_resizing = 0;
            ReleaseCapture();
            overlay_resize(hwnd);
        }
        return 0;
    case WM_SETCURSOR:
        if (!g_fixed && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            if (pt.x >= g_rect.right - RESIZE_GRIP && pt.y >= g_rect.bottom - RESIZE_GRIP) {
                SetCursor(LoadCursor(NULL, IDC_SIZENWSE));
                return TRUE;
            }
        }
        break;
    case WM_COMMAND:
        /* the context menu is owned by this (topmost) window, so forward its
         * commands to the real control panel window */
        if (g_owner) return SendMessageA(g_owner, WM_COMMAND, wp, lp);
        return 0;
    case WM_RBUTTONUP:
        if (g_owner) {
            HMENU m = CreatePopupMenu();
            POINT pt;
            AppendMenuW(m, MF_STRING, IDM_START_PAUSE, L"开始 / 暂停录像");
            AppendMenuW(m, MF_STRING, IDC_TIMER, L"开始 / 暂停计时");
            AppendMenuW(m, MF_STRING, IDM_STOP_SAVE,   L"停止并保存");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING | (g_visible ? MF_CHECKED : 0), IDM_SHOW_OVERLAY,
                        L"显示计时器（隐藏时录像里也没有）");
            AppendMenuW(m, MF_STRING | (g_fixed ? MF_CHECKED : 0), IDM_TOGGLE_CLICK,
                        L"固定计时器");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDC_SETTINGS, L"设置…");
            AppendMenuW(m, MF_STRING, IDM_SHOW_CONTROL, L"打开控制面板");
            AppendMenuW(m, MF_STRING, IDM_QUIT,         L"退出程序");
            GetCursorPos(&pt);
            SetForegroundWindow(g_owner);
            /* Owned by the overlay: a popup menu inherits its owner's
             * topmost-ness, so it is drawn ABOVE the clock instead of being
             * hidden behind it.  Commands are forwarded via WM_COMMAND. */
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            PostMessage(hwnd, WM_NULL, 0, 0);
            DestroyMenu(m);
        }
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int overlay_create(HINSTANCE hi, const TimerStyle *style, const Config *cfg)
{
    WNDCLASSA wc;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;

    ensure_cs();

    g_style = *style;
    g_fixed = cfg->fixed;
    g_auto_topmost = cfg->auto_topmost_ms;
    g_anchor = *cfg;
    if (g_fixed) ex |= WS_EX_TRANSPARENT;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = OVERLAY_CLASS;
    if (!RegisterClassA(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
    }

    g_hwnd = CreateWindowExA(ex, OVERLAY_CLASS, "", WS_POPUP,
                             0, 0, 100, 60, NULL, NULL, hi, NULL);
    if (!g_hwnd) return -1;

    if (g_auto_topmost > 0) SetTimer(g_hwnd, 1, (UINT)g_auto_topmost, NULL);
    SetTimer(g_hwnd, 2, 500, NULL);

    ShowWindow(g_hwnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    overlay_resize(g_hwnd);
    return 0;
}

void overlay_destroy(void)
{
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    pix_free();
}

HWND overlay_hwnd(void) { return g_hwnd; }
void overlay_set_owner(HWND h) { g_owner = h; }

void overlay_set_text(const char *text)
{
    if (!text) return;
    ensure_cs();
    EnterCriticalSection(&g_cs);
    if (strcmp(g_text, text) != 0) {
        strncpy(g_text, text, sizeof(g_text) - 1);
        g_text[sizeof(g_text) - 1] = 0;
        if (g_hwnd && g_visible) {
            int w = 0, h = 0;
            HDC dc = GetDC(g_hwnd);
            timer_box_size(dc, &g_style, g_text, g_recording, &w, &h);
            ReleaseDC(g_hwnd, dc);
            if (w != g_rect.right - g_rect.left) overlay_resize(g_hwnd);
            else overlay_present(g_hwnd);
        }
    }
    LeaveCriticalSection(&g_cs);
}

void overlay_set_recording(int on)
{
    ensure_cs();
    EnterCriticalSection(&g_cs);
    if (g_recording != on) {
        g_recording = on;
        if (g_hwnd && g_visible) overlay_resize(g_hwnd);
    }
    LeaveCriticalSection(&g_cs);
}

int overlay_visible(void) { return g_visible; }

void overlay_show(int on)
{
    g_visible = on ? 1 : 0;
    if (!g_hwnd) return;
    if (g_visible) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        overlay_resize(g_hwnd);
        SetWindowPos(g_hwnd, HWND_TOPMOST, g_rect.left, g_rect.top,
                     g_rect.right - g_rect.left, g_rect.bottom - g_rect.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

void overlay_set_fixed(int on)
{
    LONG ex;
    g_fixed = on ? 1 : 0;
    if (!g_hwnd) return;
    ex = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
    if (g_fixed) ex |= WS_EX_TRANSPARENT;
    else         ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

int overlay_fixed(void) { return g_fixed; }

void overlay_tick_topmost(void)
{
    if (!g_hwnd) return;
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void overlay_begin_menu(void)
{
    if (!g_hwnd) return;
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void overlay_end_menu(void)
{
    if (!g_hwnd) return;
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void overlay_move_to(int pos, int offx, int offy, int screen_w, int screen_h)
{
    (void)screen_w; (void)screen_h;
    g_anchor.pos = pos;
    g_anchor.offset_x = offx;
    g_anchor.offset_y = offy;
    if (g_hwnd) overlay_resize(g_hwnd);
}

int overlay_rect(RECT *r)
{
    if (!r) return -1;
    if (!g_hwnd) return -1;
    *r = g_rect;
    return 0;
}

/* Report the anchor (position mode + offsets) and the live font size, so the
 * application can persist a drag/resize.  For POS_CUSTOM the offsets are the
 * absolute screen position, which is exactly what timer_box_anchor expects. */
void overlay_get_geometry(int *pos, int *offx, int *offy, int *fontsize)
{
    if (pos) *pos = g_anchor.pos;
    if (offx) *offx = g_anchor.offset_x;
    if (offy) *offy = g_anchor.offset_y;
    if (fontsize) *fontsize = g_style.fontsize;
}

void overlay_set_fontsize(int fontsize)
{
    if (fontsize < 8) fontsize = 8;
    if (fontsize > 400) fontsize = 400;
    ensure_cs();
    EnterCriticalSection(&g_cs);
    g_style.fontsize = fontsize;
    LeaveCriticalSection(&g_cs);
    if (g_hwnd) overlay_resize(g_hwnd);
}

void overlay_apply(const TimerStyle *st, const Config *cfg)
{
    if (!st || !cfg) return;
    ensure_cs();
    EnterCriticalSection(&g_cs);
    g_style = *st;
    g_fixed = cfg->fixed ? 1 : 0;
    g_anchor = *cfg;
    LeaveCriticalSection(&g_cs);
    if (!g_hwnd) return;
    {
        LONG ex = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
        if (g_fixed) ex |= WS_EX_TRANSPARENT;
        else         ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex);
    }
    g_rect.left = -1;
    overlay_resize(g_hwnd);
}

void overlay_style(TimerStyle *out)
{
    if (out) *out = g_style;
}
