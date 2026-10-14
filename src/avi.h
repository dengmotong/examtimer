/* avi.h - minimal AVI (RIFF) writer used by ExamTimer.
 *
 * Video stream : MJPEG, fixed frame rate, '00dc' chunks.
 * Audio stream : optional PCM (WAVEFORMATEX), '01wb' chunks.
 */
#ifndef AVI_H
#define AVI_H

#include <stdio.h>
#include <windows.h>
#include <mmsystem.h>

typedef struct avi_writer avi_writer;

/* Opens "path" (UTF-8) for writing.  audio_fmt may be NULL for video only.
 * When the file would exceed split_bytes the writer finalises it and
 * continues in "<base>_partNN.avi".  split_bytes <= 0 disables splitting. */
avi_writer *avi_open(const char *path, int width, int height, int fps,
                     long long split_bytes, const WAVEFORMATEX *audio_fmt);

/* Appends one JPEG frame.  Returns 0 on success, -1 on failure. */
int avi_add_jpeg(avi_writer *w, const unsigned char *data, int len);

/* Appends PCM audio bytes ('01wb').  Returns 0 on success. */
int avi_add_audio(avi_writer *w, const unsigned char *data, int len);

/* Finalises the current part.  Returns 0 on success. */
int avi_close(avi_writer *w);

/* Status ------------------------------------------------------------- */
const char *avi_current_path(const avi_writer *w);
long long   avi_total_bytes(const avi_writer *w);
long long   avi_current_bytes(const avi_writer *w);
int         avi_total_frames(const avi_writer *w);
int         avi_total_audio_bytes(const avi_writer *w);
int         avi_part_count(const avi_writer *w);
int         avi_has_error(const avi_writer *w);
int         avi_has_audio(const avi_writer *w);

#endif /* AVI_H */
