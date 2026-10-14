/* jpeg_enc.c - self-contained baseline JPEG encoder.
 *
 * ITU-T T.81 baseline sequential DCT, Annex K example quantization and
 * Huffman tables, 4:2:0 or 4:4:4 chroma sampling.
 *
 * Written for ExamTimer so screen recording works on a bare Windows XP SP3
 * installation, which has no gdiplus.dll and no MJPEG VFW codec.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "jpeg_enc.h"

/* ------------------------------------------------------------------ */
/* Annex K tables                                                      */
/* ------------------------------------------------------------------ */

/* zigzag[k] = natural (raster) index of the k-th coefficient in zigzag order */
static const unsigned char zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static const unsigned char std_luminance_quant[64] = {
    16,  11,  10,  16,  24,  40,  51,  61,
    12,  12,  14,  19,  26,  58,  60,  55,
    14,  13,  16,  24,  40,  57,  69,  56,
    14,  17,  22,  29,  51,  87,  80,  62,
    18,  22,  37,  56,  68, 109, 103,  77,
    24,  35,  55,  64,  81, 104, 113,  92,
    49,  64,  78,  87, 103, 121, 120, 101,
    72,  92,  95,  98, 112, 100, 103,  99
};

static const unsigned char std_chrominance_quant[64] = {
    17,  18,  24,  47,  99,  99,  99,  99,
    18,  21,  26,  66,  99,  99,  99,  99,
    24,  26,  56,  99,  99,  99,  99,  99,
    47,  66,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99
};

static const unsigned char bits_dc_luminance[16]  = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char bits_dc_chrominance[16]= { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const unsigned char bits_ac_luminance[16]  = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const unsigned char bits_ac_chrominance[16]= { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };

static const unsigned char val_dc_luminance[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char val_dc_chrominance[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const unsigned char val_ac_luminance[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

static const unsigned char val_ac_chrominance[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

/* ------------------------------------------------------------------ */
/* DCT                                                                 */
/* ------------------------------------------------------------------ */

static float g_dct_tab[8][8];
static int   g_dct_ready = 0;

static void dct_init(void)
{
    int u, x;
    if (g_dct_ready) return;
    for (u = 0; u < 8; u++) {
        double cu = (u == 0) ? (1.0 / sqrt(2.0)) : 1.0;
        for (x = 0; x < 8; x++)
            g_dct_tab[u][x] = (float)(cu *
                cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0) * 0.5);
    }
    g_dct_ready = 1;
}

static void fdct8x8(float *blk)
{
    float tmp[64];
    int u, v, x, y;

    for (y = 0; y < 8; y++) {
        const float *row = blk + y * 8;
        for (u = 0; u < 8; u++) {
            float s = 0.0f;
            for (x = 0; x < 8; x++) s += row[x] * g_dct_tab[u][x];
            tmp[y * 8 + u] = s;
        }
    }
    for (v = 0; v < 8; v++) {
        for (u = 0; u < 8; u++) {
            float s = 0.0f;
            for (y = 0; y < 8; y++) s += tmp[y * 8 + u] * g_dct_tab[v][y];
            blk[v * 8 + u] = s;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Huffman coding                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned short code[256];
    unsigned char  size[256];
} huff_tbl;

static huff_tbl g_dc_lum, g_ac_lum, g_dc_chr, g_ac_chr;

static void huff_build(huff_tbl *t, const unsigned char *bits,
                       const unsigned char *vals, int nvals)
{
    int len, i, k = 0;
    unsigned int code = 0;
    memset(t, 0, sizeof(*t));
    for (len = 1; len <= 16; len++) {
        for (i = 0; i < bits[len - 1]; i++) {
            if (k >= nvals) return;
            t->size[vals[k]] = (unsigned char)len;
            t->code[vals[k]] = (unsigned short)code;
            code++;
            k++;
        }
        code <<= 1;
    }
}

typedef struct {
    unsigned char *buf;
    int cap;
    int len;
    unsigned int bitbuf;
    int bitcnt;
    int overflow;
} bitw;

static void bw_put(bitw *b, unsigned char c)          /* raw byte, no stuffing */
{
    if (b->len < b->cap) b->buf[b->len++] = c;
    else b->overflow = 1;
}

static void bw_entropy_byte(bitw *b, unsigned char c) /* with 0xFF stuffing */
{
    bw_put(b, c);
    if (c == 0xff) bw_put(b, 0x00);
}

static void bw_bits(bitw *b, unsigned int code, int size)
{
    if (size <= 0) return;
    b->bitbuf = (b->bitbuf << size) | (code & ((1u << size) - 1u));
    b->bitcnt += size;
    while (b->bitcnt >= 8) {
        b->bitcnt -= 8;
        bw_entropy_byte(b, (unsigned char)((b->bitbuf >> b->bitcnt) & 0xff));
    }
}

static void bw_flush(bitw *b)
{
    if (b->bitcnt > 0) {
        int pad = 8 - b->bitcnt;
        b->bitbuf = (b->bitbuf << pad) | ((1u << pad) - 1u);
        b->bitcnt = 0;
        bw_entropy_byte(b, (unsigned char)(b->bitbuf & 0xff));
    }
}

static int val_category(int v)
{
    int a = v < 0 ? -v : v;
    int n = 0;
    while (a) { n++; a >>= 1; }
    return n;
}

static void encode_block(bitw *b, const int *zz, const huff_tbl *dct,
                         const huff_tbl *act, int *prev_dc)
{
    int i, run, diff, cat, val;

    diff = zz[0] - *prev_dc;
    *prev_dc = zz[0];
    cat = val_category(diff);
    bw_bits(b, dct->code[cat], dct->size[cat]);
    if (cat) {
        val = diff < 0 ? diff - 1 : diff;
        bw_bits(b, (unsigned int)val, cat);
    }

    run = 0;
    for (i = 1; i < 64; i++) {
        if (zz[i] == 0) { run++; continue; }
        while (run > 15) {
            bw_bits(b, act->code[0xF0], act->size[0xF0]);
            run -= 16;
        }
        cat = val_category(zz[i]);
        bw_bits(b, act->code[(run << 4) | cat], act->size[(run << 4) | cat]);
        val = zz[i] < 0 ? zz[i] - 1 : zz[i];
        bw_bits(b, (unsigned int)val, cat);
        run = 0;
    }
    if (run > 0)
        bw_bits(b, act->code[0x00], act->size[0x00]);
}

/* ------------------------------------------------------------------ */
/* scratch (used from a single thread)                                 */
/* ------------------------------------------------------------------ */

static unsigned char *g_y;               static int g_ycap;
static unsigned char *g_cb, *g_cr;       static int g_cbcap, g_crcap;
static int *g_cbacc, *g_cracc;           static int g_accap_cb, g_accap_cr;
static unsigned char *g_cnt;             static int g_cntcap;
static int g_q_lum[64], g_q_chr[64];

static int ensure_buf(void **p, int *cap, int need)
{
    if (*p && *cap >= need) return 1;
    free(*p);
    *p = malloc((size_t)need);
    *cap = *p ? need : 0;
    return *p != NULL;
}

static void build_quant(int quality)
{
    int i, scale;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (i = 0; i < 64; i++) {
        int v = (std_luminance_quant[i] * scale + 50) / 100;
        if (v < 1) v = 1; if (v > 255) v = 255;
        g_q_lum[i] = v;
        v = (std_chrominance_quant[i] * scale + 50) / 100;
        if (v < 1) v = 1; if (v > 255) v = 255;
        g_q_chr[i] = v;
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int jpeg_enc_bound(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    return w * h * 4 + 65536;
}

int jpeg_encode_bgr_to(const unsigned char *pixels, int w, int h, int stride,
                       int bpp, int quality, int subsample,
                       unsigned char *out, int outcap)
{
    int i, x, y, mx, my, nmx, nmy, ci, comp;
    int cw, ch, use420;
    const unsigned char *row;
    bitw bw;
    int dc_pred[3];
    const unsigned char *comp_plane[3];
    int comp_h[3], comp_v[3];
    float blk[64];
    int zz[64], tmp[64];

    if (!pixels || !out || w <= 0 || h <= 0 || (bpp != 3 && bpp != 4))
        return JPEGENC_ERR_ARGS;
    if (outcap < 1024)
        return JPEGENC_ERR_OVERFLOW;
    if (stride == 0) stride = w * bpp;

    dct_init();
    huff_build(&g_dc_lum, bits_dc_luminance,  val_dc_luminance,  12);
    huff_build(&g_ac_lum, bits_ac_luminance,  val_ac_luminance, 162);
    huff_build(&g_dc_chr, bits_dc_chrominance,val_dc_chrominance,12);
    huff_build(&g_ac_chr, bits_ac_chrominance,val_ac_chrominance,162);
    build_quant(quality);

    use420 = subsample ? 1 : 0;
    cw = use420 ? (w + 1) / 2 : w;
    ch = use420 ? (h + 1) / 2 : h;

    if (!ensure_buf((void **)&g_y, &g_ycap, w * h)) return JPEGENC_ERR_NOMEM;
    if (!ensure_buf((void **)&g_cb, &g_cbcap, cw * ch)) return JPEGENC_ERR_NOMEM;
    if (!ensure_buf((void **)&g_cr, &g_crcap, cw * ch)) return JPEGENC_ERR_NOMEM;

    /* ---- colour conversion ---------------------------------------- */
    if (use420) {
        if (!ensure_buf((void **)&g_cbacc, &g_accap_cb, cw * ch * (int)sizeof(int))) return JPEGENC_ERR_NOMEM;
        if (!ensure_buf((void **)&g_cracc, &g_accap_cr, cw * ch * (int)sizeof(int))) return JPEGENC_ERR_NOMEM;
        if (!ensure_buf((void **)&g_cnt, &g_cntcap, cw * ch)) return JPEGENC_ERR_NOMEM;
        memset(g_cbacc, 0, (size_t)cw * ch * sizeof(int));
        memset(g_cracc, 0, (size_t)cw * ch * sizeof(int));
        memset(g_cnt, 0, (size_t)cw * ch);

        for (y = 0; y < h; y++) {
            row = pixels + (stride >= 0 ? (size_t)y * stride : (size_t)(h - 1 - y) * (size_t)(-stride));
            for (x = 0; x < w; x++) {
                const unsigned char *px = row + (size_t)x * bpp;
                int B = px[0], G = px[1], R = px[2];
                int Y  = ( 19595 * R + 38470 * G +  7471 * B + 32768) >> 16;
                int Cb = (-11059 * R - 21709 * G + 32768 * B + 8421376) >> 16;
                int Cr = ( 32768 * R - 27439 * G -  5329 * B + 8421376) >> 16;
                int idx = (y >> 1) * cw + (x >> 1);
                if (Y < 0) Y = 0; if (Y > 255) Y = 255;
                g_y[y * w + x] = (unsigned char)Y;
                g_cbacc[idx] += Cb;
                g_cracc[idx] += Cr;
                g_cnt[idx]++;
            }
        }
        for (i = 0; i < cw * ch; i++) {
            int n = g_cnt[i] ? g_cnt[i] : 1;
            int a = g_cbacc[i] / n, b = g_cracc[i] / n;
            if (a < 0) a = 0; if (a > 255) a = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            g_cb[i] = (unsigned char)a;
            g_cr[i] = (unsigned char)b;
        }
    } else {
        for (y = 0; y < h; y++) {
            row = pixels + (stride >= 0 ? (size_t)y * stride : (size_t)(h - 1 - y) * (size_t)(-stride));
            for (x = 0; x < w; x++) {
                const unsigned char *px = row + (size_t)x * bpp;
                int B = px[0], G = px[1], R = px[2];
                int Y  = ( 19595 * R + 38470 * G +  7471 * B + 32768) >> 16;
                int Cb = (-11059 * R - 21709 * G + 32768 * B + 8421376) >> 16;
                int Cr = ( 32768 * R - 27439 * G -  5329 * B + 8421376) >> 16;
                if (Y < 0) Y = 0; if (Y > 255) Y = 255;
                if (Cb < 0) Cb = 0; if (Cb > 255) Cb = 255;
                if (Cr < 0) Cr = 0; if (Cr > 255) Cr = 255;
                g_y[y * w + x] = (unsigned char)Y;
                g_cb[y * cw + x] = (unsigned char)Cb;
                g_cr[y * cw + x] = (unsigned char)Cr;
            }
        }
    }

    /* ---- headers --------------------------------------------------- */
    memset(&bw, 0, sizeof(bw));
    bw.buf = out; bw.cap = outcap; bw.len = 0;

    bw_put(&bw, 0xFF); bw_put(&bw, 0xD8);                       /* SOI */
    /* APP0 JFIF */
    bw_put(&bw, 0xFF); bw_put(&bw, 0xE0);
    bw_put(&bw, 0x00); bw_put(&bw, 0x10);
    bw_put(&bw, 'J'); bw_put(&bw, 'F'); bw_put(&bw, 'I'); bw_put(&bw, 'F'); bw_put(&bw, 0x00);
    bw_put(&bw, 0x01); bw_put(&bw, 0x01); bw_put(&bw, 0x00);
    bw_put(&bw, 0x00); bw_put(&bw, 0x01); bw_put(&bw, 0x00); bw_put(&bw, 0x01);
    bw_put(&bw, 0x00); bw_put(&bw, 0x00);

    for (ci = 0; ci < 2; ci++) {                                 /* DQT x2 */
        const int *q = ci ? g_q_chr : g_q_lum;
        bw_put(&bw, 0xFF); bw_put(&bw, 0xDB);
        bw_put(&bw, 0x00); bw_put(&bw, 0x43);
        bw_put(&bw, (unsigned char)ci);
        for (i = 0; i < 64; i++) bw_put(&bw, (unsigned char)q[zigzag[i]]);
    }

    /* SOF0 */
    bw_put(&bw, 0xFF); bw_put(&bw, 0xC0);
    bw_put(&bw, 0x00); bw_put(&bw, 0x11);
    bw_put(&bw, 0x08);
    bw_put(&bw, (unsigned char)(h >> 8)); bw_put(&bw, (unsigned char)(h & 0xff));
    bw_put(&bw, (unsigned char)(w >> 8)); bw_put(&bw, (unsigned char)(w & 0xff));
    bw_put(&bw, 0x03);
    bw_put(&bw, 0x01); bw_put(&bw, use420 ? 0x22 : 0x11); bw_put(&bw, 0x00);
    bw_put(&bw, 0x02); bw_put(&bw, 0x11); bw_put(&bw, 0x01);
    bw_put(&bw, 0x03); bw_put(&bw, 0x11); bw_put(&bw, 0x01);

    {                                                            /* DHT x4 */
        static const unsigned char *bps[4] = { bits_dc_luminance, bits_ac_luminance,
                                               bits_dc_chrominance, bits_ac_chrominance };
        static const unsigned char *vls[4] = { val_dc_luminance, val_ac_luminance,
                                               val_dc_chrominance, val_ac_chrominance };
        static const int nv[4] = { 12, 162, 12, 162 };
        for (i = 0; i < 4; i++) {
            int j, len = 2 + 1 + 16 + nv[i];
            bw_put(&bw, 0xFF); bw_put(&bw, 0xC4);
            bw_put(&bw, (unsigned char)(len >> 8)); bw_put(&bw, (unsigned char)(len & 0xff));
            bw_put(&bw, (unsigned char)((i == 0) ? 0x00 : (i == 1) ? 0x10 : (i == 2) ? 0x01 : 0x11));
            for (j = 0; j < 16; j++) bw_put(&bw, bps[i][j]);
            for (j = 0; j < nv[i]; j++) bw_put(&bw, vls[i][j]);
        }
    }

    /* SOS */
    bw_put(&bw, 0xFF); bw_put(&bw, 0xDA);
    bw_put(&bw, 0x00); bw_put(&bw, 0x0C);
    bw_put(&bw, 0x03);
    bw_put(&bw, 0x01); bw_put(&bw, 0x00);
    bw_put(&bw, 0x02); bw_put(&bw, 0x11);
    bw_put(&bw, 0x03); bw_put(&bw, 0x11);
    bw_put(&bw, 0x00); bw_put(&bw, 0x3F); bw_put(&bw, 0x00);

    if (bw.overflow) return JPEGENC_ERR_OVERFLOW;

    /* ---- entropy coded scan ---------------------------------------- */
    comp_plane[0] = g_y;  comp_h[0] = use420 ? 2 : 1; comp_v[0] = use420 ? 2 : 1;
    comp_plane[1] = g_cb; comp_h[1] = 1;              comp_v[1] = 1;
    comp_plane[2] = g_cr; comp_h[2] = 1;              comp_v[2] = 1;

    nmx = (w + (use420 ? 15 : 7)) / (use420 ? 16 : 8);
    nmy = (h + (use420 ? 15 : 7)) / (use420 ? 16 : 8);
    dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;

    for (my = 0; my < nmy; my++) {
        for (mx = 0; mx < nmx; mx++) {
            for (comp = 0; comp < 3; comp++) {
                const huff_tbl *dct = comp ? &g_dc_chr : &g_dc_lum;
                const huff_tbl *act = comp ? &g_ac_chr : &g_ac_lum;
                const int *q = comp ? g_q_chr : g_q_lum;
                const unsigned char *pl = comp_plane[comp];
                int pw = (comp == 0) ? w : cw;
                int ph = (comp == 0) ? h : ch;
                int bx, by;

                for (by = 0; by < comp_v[comp]; by++) {
                    for (bx = 0; bx < comp_h[comp]; bx++) {
                        int ox = (mx * comp_h[comp] + bx) * 8;
                        int oy = (my * comp_v[comp] + by) * 8;

                        for (y = 0; y < 8; y++) {
                            int sy = oy + y;
                            if (sy >= ph) sy = ph - 1;
                            if (sy < 0) sy = 0;
                            for (x = 0; x < 8; x++) {
                                int sx = ox + x;
                                if (sx >= pw) sx = pw - 1;
                                if (sx < 0) sx = 0;
                                blk[y * 8 + x] = (float)pl[sy * pw + sx] - 128.0f;
                            }
                        }

                        fdct8x8(blk);

                        for (i = 0; i < 64; i++) {
                            int u = i >> 3, v = i & 7;
                            float f = blk[u * 8 + v] / (float)q[u * 8 + v];
                            int r = (int)(f < 0 ? f - 0.5f : f + 0.5f);
                            /* baseline categories are limited to 11 (DC) and 10 (AC) */
                            if (r < -1023) r = -1023;
                            if (r >  1023) r =  1023;
                            blk[u * 8 + v] = (float)r;
                        }
                        for (i = 0; i < 64; i++) zz[i] = (int)blk[i];
                        for (i = 0; i < 64; i++) tmp[i] = zz[zigzag[i]];

                        encode_block(&bw, tmp, dct, act, &dc_pred[comp]);
                        if (bw.overflow) return JPEGENC_ERR_OVERFLOW;
                    }
                }
            }
        }
    }

    bw_flush(&bw);
    if (bw.overflow) return JPEGENC_ERR_OVERFLOW;
    bw_put(&bw, 0xFF); bw_put(&bw, 0xD9);                       /* EOI */
    if (bw.overflow) return JPEGENC_ERR_OVERFLOW;

    return bw.len;
}

unsigned char *jpeg_encode_bgr(const unsigned char *pixels, int w, int h,
                               int stride, int bpp, int quality,
                               int subsample, int *out_len)
{
    int cap = jpeg_enc_bound(w, h);
    unsigned char *buf;
    int n;
    if (out_len) *out_len = 0;
    if (cap <= 0) return NULL;
    buf = (unsigned char *)malloc((size_t)cap);
    if (!buf) return NULL;
    n = jpeg_encode_bgr_to(pixels, w, h, stride, bpp, quality, subsample, buf, cap);
    if (n < 0) { free(buf); return NULL; }
    if (out_len) *out_len = n;
    return buf;
}

void jpeg_enc_free(void *p) { free(p); }

/* ------------------------------------------------------------------ */
/* selftest                                                            */
/* ------------------------------------------------------------------ */

int jpeg_enc_selftest(char *msg, int msgcap)
{
    int w = 64, h = 48, i, n = 0, stride, ok = 1;
    unsigned char *img, *jpg;

    {
        int dcn = 0, acn = 0, k;
        for (k = 0; k < 16; k++) { dcn += bits_dc_luminance[k]; acn += bits_ac_luminance[k]; }
        if (dcn != 12 || acn != 162) ok = 0;
        dcn = 0; acn = 0;
        for (k = 0; k < 16; k++) { dcn += bits_dc_chrominance[k]; acn += bits_ac_chrominance[k]; }
        if (dcn != 12 || acn != 162) ok = 0;
    }

    stride = w * 3;
    img = (unsigned char *)malloc((size_t)stride * h);
    if (!img) return -1;
    for (i = 0; i < w * h; i++) {
        unsigned char *p = img + (size_t)(i / w) * stride + (size_t)(i % w) * 3;
        p[0] = (unsigned char)(i % 251);
        p[1] = (unsigned char)((i * 3) % 253);
        p[2] = (unsigned char)((i * 7) % 249);
    }

    jpg = jpeg_encode_bgr(img, w, h, stride, 3, 75, 1, &n);
    if (!jpg || n < 100) ok = 0;
    else if (!(jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[n - 2] == 0xFF && jpg[n - 1] == 0xD9)) ok = 0;
    else {
        int pos = 2, seen_sof = 0, seen_sos = 0;
        while (pos + 4 <= n) {
            int m, len;
            if (jpg[pos] != 0xFF) { ok = 0; break; }
            m = jpg[pos + 1];
            if (m == 0xD9) break;
            if (m == 0xDA) { seen_sos = 1; break; }
            len = (jpg[pos + 2] << 8) | jpg[pos + 3];
            if (len < 2) { ok = 0; break; }
            if (m == 0xC0) seen_sof = 1;
            pos += 2 + len;
        }
        if (!seen_sof || !seen_sos) ok = 0;
    }
    if (jpg) jpeg_enc_free(jpg);

    for (i = 1; i <= 100; i += 7) {
        int k;
        build_quant(i);
        for (k = 0; k < 64; k++)
            if (g_q_lum[k] < 1 || g_q_lum[k] > 255 || g_q_chr[k] < 1 || g_q_chr[k] > 255) ok = 0;
    }

    free(img);
    if (msg && msgcap > 0)
        snprintf(msg, (size_t)msgcap, "jpeg-encoder: %s (64x48 test frame = %d bytes)",
                 ok ? "PASS" : "FAIL", n);
    return ok ? 0 : -1;
}
