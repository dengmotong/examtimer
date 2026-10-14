/* jpeg_enc.h - self-contained baseline JPEG encoder (no GDI+ / no external libs)
 *
 * Part of ExamTimer (考核计时录屏器)
 * Plain C99, no dependencies beyond libc.  Works on Windows XP SP3 and later.
 */
#ifndef JPEG_ENC_H
#define JPEG_ENC_H

#ifdef __cplusplus
extern "C" {
#endif

#define JPEGENC_OK              0
#define JPEGENC_ERR_ARGS      (-1)
#define JPEGENC_ERR_NOMEM     (-2)
#define JPEGENC_ERR_OVERFLOW  (-3)

/* Encode a BGR/BGRA image into a caller supplied buffer.
 *   pixels   : raw image data (BGR or BGRA byte order)
 *   w,h      : image size in pixels
 *   stride   : bytes per row, may be negative for bottom-up DIBs
 *   bpp      : 3 or 4
 *   quality  : 1..100
 *   subsample: 1 = 4:2:0, 0 = 4:4:4
 * Returns the encoded byte count, or a negative JPEGENC_ERR_* code. */
int jpeg_encode_bgr_to(const unsigned char *pixels, int w, int h, int stride,
                       int bpp, int quality, int subsample,
                       unsigned char *out, int outcap);

/* Same, but allocates the output (free with jpeg_enc_free). */
unsigned char *jpeg_encode_bgr(const unsigned char *pixels, int w, int h,
                               int stride, int bpp, int quality,
                               int subsample, int *out_len);

void jpeg_enc_free(void *p);

/* Safe output buffer size for a w*h image. */
int jpeg_enc_bound(int w, int h);

/* Internal consistency check (used by --selftest); returns 0 on success. */
int jpeg_enc_selftest(char *msg, int msgcap);

#ifdef __cplusplus
}
#endif
#endif /* JPEG_ENC_H */
