#include <stdio.h>
#include <stdlib.h>
#include "jpeg_enc.h"

int main(int argc, char **argv)
{
    int w = 320, h = 240, i, x, y, n = 0;
    unsigned char *img = malloc((size_t)w*h*3);
    unsigned char *jpg;
    char msg[256];
    FILE *f;

    /* synthetic test card: gradients + hard edges + text-like bars */
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        unsigned char *p = img + ((size_t)y*w + x)*3;
        int bar = (x/16) % 2;
        int red = (x*255)/w, grn = (y*255)/h, blu = ((x+y)*255)/(w+h);
        if (y < 40) { p[0]= (bar?255:0); p[1]=(bar?255:0); p[2]=(bar?255:0); }
        else if (y < 80) { p[0]=0; p[1]=0; p[2]= (x%8<4)?255:0; }
        else { p[0]=blu; p[1]=grn; p[2]=red; }
    }
    if (jpeg_enc_selftest(msg, sizeof msg) != 0) { printf("selftest FAIL: %s\n", msg); return 1; }
    printf("%s\n", msg);

    for (i = 0; i < 2; i++) {
        jpg = jpeg_encode_bgr(img, w, h, w*3, 3, 85, i, &n);
        if (!jpg) { printf("encode failed (sub=%d)\n", i); return 1; }
        printf("subsample=4:%d  size=%d bytes\n", i?2:4, n);
        f = fopen(i ? "test/out/enc_444.jpg" : "test/out/enc_420.jpg", "wb");
        fwrite(jpg, 1, n, f); fclose(f);
        jpeg_enc_free(jpg);
    }
    /* write the reference PPM for PSNR comparison */
    f = fopen("test/out/ref.ppm", "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        unsigned char *p = img + ((size_t)y*w + x)*3;
        fputc(p[2], f); fputc(p[1], f); fputc(p[0], f);
    }
    fclose(f);
    free(img);
    return 0;
}
