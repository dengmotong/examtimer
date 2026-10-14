/* avi.c - minimal AVI (RIFF/AVI) writer with optional PCM audio.
 *
 * Produces a plain, widely compatible AVI:
 *   stream 0 : vids / MJPG, fixed frame rate, '00dc' chunks
 *   stream 1 : auds / PCM , '01wb' chunks            (optional)
 * plus a complete 'idx1' index.  Header field offsets are remembered while
 * writing instead of being hard coded, so adding the audio stream cannot
 * silently corrupt the size fields.
 *
 * Files are split automatically so we never approach the 2 GB limit of
 * 32-bit stdio (which is what MSVCRT on Windows XP provides).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "avi.h"
#include "examtimer.h"

typedef struct {
    unsigned int id;
    unsigned int flags;
    unsigned int offset;
    unsigned int size;
} avi_index_entry;

struct avi_writer {
    FILE *fp;
    char  base[1024];       /* path without the ".avi" suffix (UTF-8) */
    char  cur[1200];        /* current file path (UTF-8) */
    int   part;             /* 1 based */
    int   width, height, fps;

    int   has_audio;
    WAVEFORMATEX afmt;

    long long split_bytes;
    long long total_bytes;
    long long part_bytes;
    int       total_frames;
    int       part_frames;
    int       total_audio;      /* bytes over all parts */
    int       part_audio;       /* bytes in the current part */

    long      movi_start;
    long      movi_fourcc;
    long      movi_end;

    /* remembered header field offsets, filled in by write_headers() */
    long      off_avih_maxbps;
    long      off_avih_frames;
    long      off_avih_suggest;
    long      off_vstrh_length;
    long      off_vstrh_suggest;
    long      off_astrh_length;
    long      off_astrh_suggest;

    avi_index_entry *index;
    int index_count, index_cap;

    unsigned int max_frame;
    unsigned int max_audio;
    int error;
};

/* ------------------------------------------------------------------ */
/* little endian helpers                                               */
/* ------------------------------------------------------------------ */

static void put_u16(FILE *f, unsigned int v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
}

static void put_u32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
    fputc((int)((v >> 16) & 0xff), f);
    fputc((int)((v >> 24) & 0xff), f);
}

static void put_fourcc(FILE *f, const char *s)
{
    fputc(s[0], f); fputc(s[1], f); fputc(s[2], f); fputc(s[3], f);
}

static void patch_u32(FILE *f, long off, unsigned long v)
{
    long cur;
    if (off <= 0) return;
    cur = ftell(f);
    if (cur < 0) return;
    if (fseek(f, off, SEEK_SET) != 0) { fseek(f, cur, SEEK_SET); return; }
    put_u32(f, v);
    fseek(f, cur, SEEK_SET);
}

static long here(FILE *f) { return ftell(f); }

static void make_part_name(avi_writer *w)
{
    if (w->part <= 1)
        snprintf(w->cur, sizeof(w->cur), "%s.avi", w->base);
    else
        snprintf(w->cur, sizeof(w->cur), "%s_part%02d.avi", w->base, w->part);
}

/* ------------------------------------------------------------------ */
/* header                                                              */
/* ------------------------------------------------------------------ */

static int write_strh_video(avi_writer *w)
{
    FILE *f = w->fp;
    fwrite("strh", 1, 4, f);
    put_u32(f, 56);
    put_fourcc(f, "vids");
    put_fourcc(f, "MJPG");
    put_u32(f, 0);
    put_u16(f, 0); put_u16(f, 0);
    put_u32(f, 0);
    put_u32(f, 1);                                         /* dwScale */
    put_u32(f, (unsigned long)(w->fps > 0 ? w->fps : 1));   /* dwRate  */
    put_u32(f, 0);                                         /* dwStart */
    w->off_vstrh_length = here(f);                          /* dwLength */
    put_u32(f, 0);
    w->off_vstrh_suggest = here(f);
    put_u32(f, 0);
    put_u32(f, 0xFFFFFFFFu);                               /* dwQuality */
    put_u32(f, 0);                                          /* dwSampleSize */
    put_u16(f, 0); put_u16(f, 0);
    put_u16(f, (unsigned int)w->width); put_u16(f, (unsigned int)w->height);
    return 0;
}

static int write_strf_video(avi_writer *w)
{
    FILE *f = w->fp;
    fwrite("strf", 1, 4, f);
    put_u32(f, 40);
    put_u32(f, 40);
    put_u32(f, (unsigned long)w->width);
    put_u32(f, (unsigned long)w->height);
    put_u16(f, 1);
    put_u16(f, 24);
    put_fourcc(f, "MJPG");
    put_u32(f, (unsigned long)(w->width * w->height * 3));
    put_u32(f, 0); put_u32(f, 0); put_u32(f, 0); put_u32(f, 0);
    return 0;
}

static int write_strh_audio(avi_writer *w)
{
    FILE *f = w->fp;
    fwrite("strh", 1, 4, f);
    put_u32(f, 56);
    put_fourcc(f, "auds");
    put_u32(f, 1);                                    /* fccHandler: PCM */
    put_u32(f, 0);
    put_u16(f, 0); put_u16(f, 0);
    put_u32(f, 0);
    /* dwScale = block align, dwRate = bytes per second:
     * seconds = dwLength * dwScale / dwRate                     */
    put_u32(f, w->afmt.nBlockAlign ? w->afmt.nBlockAlign : 1);
    put_u32(f, w->afmt.nAvgBytesPerSec ? w->afmt.nAvgBytesPerSec : 1);
    put_u32(f, 0);
    w->off_astrh_length = here(f);                    /* dwLength = samples */
    put_u32(f, 0);
    w->off_astrh_suggest = here(f);
    put_u32(f, 0);
    put_u32(f, 0xFFFFFFFFu);
    put_u32(f, w->afmt.nBlockAlign);                  /* dwSampleSize */
    put_u16(f, 0); put_u16(f, 0);
    put_u16(f, 0); put_u16(f, 0);
    return 0;
}

static int write_strf_audio(avi_writer *w)
{
    FILE *f = w->fp;
    fwrite("strf", 1, 4, f);
    put_u32(f, 18);                                   /* WAVEFORMATEX without cbSize */
    put_u16(f, w->afmt.wFormatTag);
    put_u16(f, w->afmt.nChannels);
    put_u32(f, w->afmt.nSamplesPerSec);
    put_u32(f, w->afmt.nAvgBytesPerSec);
    put_u16(f, w->afmt.nBlockAlign);
    put_u16(f, w->afmt.wBitsPerSample);
    put_u16(f, 0);                                    /* cbSize */
    return 0;
}

static int write_headers(avi_writer *w)
{
    FILE *f = w->fp;
    long hdrl_size_pos, strl1_size_pos, strl2_size_pos = 0, strl2_payload = 0;

    fwrite("RIFF", 1, 4, f);
    put_u32(f, 0);                                    /* patched at close */
    fwrite("AVI ", 1, 4, f);

    /* ---- hdrl ---- */
    fwrite("LIST", 1, 4, f);
    hdrl_size_pos = here(f);
    put_u32(f, 0);                                    /* patched below */
    fwrite("hdrl", 1, 4, f);

    /* avih */
    fwrite("avih", 1, 4, f);
    put_u32(f, 56);
    put_u32(f, (unsigned long)(1000000 / (w->fps > 0 ? w->fps : 1)));
    w->off_avih_maxbps = here(f);
    put_u32(f, 0);
    put_u32(f, 0);                                    /* padding granularity */
    put_u32(f, 0x10);                                 /* AVIF_HASINDEX */
    w->off_avih_frames = here(f);
    put_u32(f, 0);                                    /* total frames */
    put_u32(f, 0);                                    /* initial frames */
    put_u32(f, w->has_audio ? 2 : 1);                 /* streams */
    w->off_avih_suggest = here(f);
    put_u32(f, 0);                                    /* suggested buffer */
    put_u32(f, (unsigned long)w->width);
    put_u32(f, (unsigned long)w->height);
    put_u32(f, 0); put_u32(f, 0); put_u32(f, 0); put_u32(f, 0);

    /* ---- strl : video ---- */
    fwrite("LIST", 1, 4, f);
    strl1_size_pos = here(f);
    put_u32(f, 0);
    fwrite("strl", 1, 4, f);
    write_strh_video(w);
    write_strf_video(w);

    /* ---- strl : audio ---- */
    if (w->has_audio) {
        fwrite("LIST", 1, 4, f);
        strl2_size_pos = here(f);
        put_u32(f, 0);
        strl2_payload = here(f);
        fwrite("strl", 1, 4, f);
        write_strh_audio(w);
        write_strf_audio(w);
    }

    /* patch the LIST sizes we now know */
    {
        long end = here(f);
        patch_u32(f, hdrl_size_pos, (unsigned long)(end - hdrl_size_pos - 4));
        /* strl size = total - (4 for LIST + 4 for size) ... computed per list */
        patch_u32(f, strl1_size_pos, (unsigned long)(strl2_size_pos
                 ? (strl2_size_pos - strl1_size_pos - 4) : (end - strl1_size_pos - 4)));
        if (strl2_size_pos)
            patch_u32(f, strl2_size_pos, (unsigned long)(end - strl2_size_pos - 4));
    }

    /* ---- movi ---- */
    w->movi_start = here(f);
    fwrite("LIST", 1, 4, f);
    put_u32(f, 0);                                    /* patched at close */
    w->movi_fourcc = here(f);
    fwrite("movi", 1, 4, f);

    return ferror(f) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

avi_writer *avi_open(const char *path, int width, int height, int fps,
                     long long split_bytes, const WAVEFORMATEX *audio_fmt)
{
    avi_writer *w;
    const char *dot;

    if (!path || width <= 0 || height <= 0 || width > 65535 || height > 65535)
        return NULL;
    if (fps <= 0) fps = 5;

    w = (avi_writer *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    strncpy(w->base, path, sizeof(w->base) - 1);
    dot = strrchr(w->base, '.');
    if (dot && (strcmp(dot, ".avi") == 0 || strcmp(dot, ".AVI") == 0))
        w->base[dot - w->base] = 0;

    w->width = width;
    w->height = height;
    w->fps = fps;
    w->split_bytes = split_bytes;
    w->part = 1;
    if (audio_fmt && audio_fmt->nAvgBytesPerSec > 0) {
        w->has_audio = 1;
        w->afmt = *audio_fmt;
    }
    make_part_name(w);

    w->fp = u8_fopen(w->cur, "wb");
    if (!w->fp) { free(w); return NULL; }
    if (write_headers(w) != 0) { fclose(w->fp); free(w); return NULL; }
    return w;
}

static int finalize_part(avi_writer *w)
{
    FILE *f = w->fp;
    long end;
    unsigned long riff_size, movi_size;
    int i;

    if (!f) return 0;
    w->movi_end = here(f);

    fwrite("idx1", 1, 4, f);
    put_u32(f, (unsigned long)(w->index_count * 16));
    for (i = 0; i < w->index_count; i++) {
        unsigned int id = w->index[i].id;
        put_fourcc(f, (const char *)&id);
        put_u32(f, w->index[i].flags);
        put_u32(f, w->index[i].offset);
        put_u32(f, w->index[i].size);
    }
    end = here(f);

    {
        unsigned fps = (unsigned)(w->fps > 0 ? w->fps : 1);
        unsigned long long dur = (unsigned long long)w->index_count * 1000000ull / fps;
        unsigned long mbps = dur ? (unsigned long)((unsigned long long)w->part_bytes * 1000000ull / dur) : 0;
        unsigned long sugg = w->max_frame;
        if (w->max_audio > sugg) sugg = w->max_audio;

        patch_u32(f, w->off_avih_maxbps, mbps);
        patch_u32(f, w->off_avih_frames, (unsigned long)w->part_frames);
        patch_u32(f, w->off_avih_suggest, sugg);
        patch_u32(f, w->off_vstrh_length, (unsigned long)w->part_frames);
        patch_u32(f, w->off_vstrh_suggest, w->max_frame);
        if (w->has_audio) {
            unsigned block = w->afmt.nBlockAlign ? w->afmt.nBlockAlign : 1;
            patch_u32(f, w->off_astrh_length, (unsigned long)(w->part_audio / (int)block));
            patch_u32(f, w->off_astrh_suggest, w->max_audio);
        }
    }
    movi_size = (unsigned long)(w->movi_end - w->movi_fourcc);
    patch_u32(f, w->movi_start + 4, movi_size);
    riff_size = (unsigned long)(end - 8);
    patch_u32(f, 4, riff_size);

    if (fflush(f) != 0) w->error = 1;
    if (fclose(f) != 0) w->error = 1;
    w->fp = NULL;
    return w->error ? -1 : 0;
}

static int add_index(avi_writer *w, const char *fourcc, unsigned int offset, int len)
{
    if (w->index_count >= w->index_cap) {
        int ncap = w->index_cap ? w->index_cap * 2 : 4096;
        avi_index_entry *ni = (avi_index_entry *)realloc(w->index, (size_t)ncap * sizeof(*ni));
        if (!ni) { w->error = 1; return -1; }
        w->index = ni;
        w->index_cap = ncap;
    }
    memcpy(&w->index[w->index_count].id, fourcc, 4);
    w->index[w->index_count].flags = 0x10;            /* AVIIF_KEYFRAME */
    w->index[w->index_count].offset = offset;
    w->index[w->index_count].size = (unsigned int)len;
    w->index_count++;
    return 0;
}

static int write_chunk(avi_writer *w, const char *fourcc,
                       const unsigned char *data, int len)
{
    long pos;
    unsigned int offset;

    pos = here(w->fp);
    if (pos < 0) { w->error = 1; return -1; }
    offset = (unsigned int)(pos - w->movi_fourcc);
    fwrite(fourcc, 1, 4, w->fp);
    put_u32(w->fp, (unsigned long)len);
    if (add_index(w, fourcc, offset + 8, len) != 0) return -1;
    if (len > 0 && fwrite(data, 1, (size_t)len, w->fp) != (size_t)len) {
        w->error = 1;
        return -1;
    }
    if (len & 1) fputc(0, w->fp);
    if (ferror(w->fp)) { w->error = 1; return -1; }
    w->part_bytes = here(w->fp);
    return 0;
}

static int split_if_needed(avi_writer *w, int incoming)
{
    if (w->split_bytes <= 0) return 0;
    if (w->part_frames == 0) return 0;                 /* never split an empty part */
    if (w->part_bytes + incoming + 64 <= w->split_bytes) return 0;

    if (finalize_part(w) != 0) { w->error = 1; return -1; }
    w->total_bytes += w->part_bytes;
    w->part_bytes = 0;
    w->part_frames = 0;
    w->part_audio = 0;
    w->index_count = 0;
    w->max_frame = 0;
    w->max_audio = 0;
    w->part++;
    make_part_name(w);
    w->fp = u8_fopen(w->cur, "wb");
    if (!w->fp) { w->error = 1; return -1; }
    if (write_headers(w) != 0) { w->error = 1; return -1; }
    return 0;
}

int avi_add_jpeg(avi_writer *w, const unsigned char *data, int len)
{
    if (!w || !w->fp || !data || len <= 0 || w->error) return -1;
    if (split_if_needed(w, len) != 0) return -1;
    if (write_chunk(w, "00dc", data, len) != 0) return -1;
    w->part_frames++;
    w->total_frames++;
    if ((unsigned int)len > w->max_frame) w->max_frame = (unsigned int)len;
    return 0;
}

int avi_add_audio(avi_writer *w, const unsigned char *data, int len)
{
    if (!w || !w->fp || !data || len <= 0 || w->error) return -1;
    if (!w->has_audio) return 0;
    /* audio chunks are small and frequent: only split on video frames so the
     * audio stream stays continuous inside one file */
    if (write_chunk(w, "01wb", data, len) != 0) return -1;
    w->part_audio += len;
    w->total_audio += len;
    if ((unsigned int)len > w->max_audio) w->max_audio = (unsigned int)len;
    return 0;
}

int avi_close(avi_writer *w)
{
    int rc = 0;
    if (!w) return -1;
    if (w->fp) {
        w->total_bytes += w->part_bytes;
        rc = finalize_part(w);
    } else {
        w->total_bytes += w->part_bytes;
    }
    free(w->index);
    free(w);
    return rc;
}

const char *avi_current_path(const avi_writer *w) { return w ? w->cur : ""; }
long long   avi_total_bytes(const avi_writer *w)  { return w ? w->total_bytes + (w->fp ? w->part_bytes : 0) : 0; }
long long   avi_current_bytes(const avi_writer *w){ return w ? w->part_bytes : 0; }
int         avi_total_frames(const avi_writer *w) { return w ? w->total_frames : 0; }
int         avi_total_audio_bytes(const avi_writer *w) { return w ? w->total_audio : 0; }
int         avi_part_count(const avi_writer *w)   { return w ? w->part : 0; }
int         avi_has_error(const avi_writer *w)    { return w ? w->error : 1; }
int         avi_has_audio(const avi_writer *w)    { return w ? w->has_audio : 0; }
