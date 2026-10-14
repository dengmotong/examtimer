/* audio.c - sound capture for ExamTimer.
 *
 * Windows XP has no WASAPI loopback, so "system sound" means the sound card's
 * "Stereo Mix" / "立体声混音" input, exactly like every other XP-era recorder.
 * Devices are plain waveIn devices, which also covers microphones and line-in.
 *
 * A synthetic sine-wave source (device = -3) is included so the AVI audio
 * muxing can be tested automatically on machines without a sound card.
 */
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "examtimer.h"

#define NRING (1 << 21)          /* 2 MB ring buffer            */
#define NBUF  8                  /* waveIn buffers              */
#define BUFSZ 8192

static HWAVEIN      g_hwi = NULL;
static WAVEHDR      g_hdr[NBUF];
static unsigned char *g_bufdata[NBUF];

static CRITICAL_SECTION g_cs;
static unsigned char g_ring[NRING];
static volatile LONG g_head = 0, g_tail = 0;
static volatile LONG g_started = 0;
static volatile LONG g_paused = 0;
static long long g_total = 0;

static int   g_dev = -1;
static int   g_rate = 44100, g_ch = 2, g_bits = 16;
static char  g_name[128] = "";
static char  g_msg[128] = "";
static int   g_synth = 0;
static double g_synth_phase = 0.0;

static void ensure_cs(void)
{
    static LONG state = 0;
    if (state == 2) return;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        InterlockedExchange(&state, 2);
    } else {
        while (state != 2) Sleep(0);
    }
}

/* ------------------------------------------------------------------ */
/* ring buffer                                                         */
/* ------------------------------------------------------------------ */

static int ring_free(void)
{
    int h = g_head, t = g_tail;
    int used = (h >= t) ? (h - t) : (NRING - t + h);
    return NRING - 1 - used;
}

static void ring_push(const unsigned char *p, int n)
{
    int h = g_head;
    int first = NRING - h;
    if (n > first) {
        memcpy(g_ring + h, p, (size_t)first);
        memcpy(g_ring, p + first, (size_t)(n - first));
        h = n - first;
    } else {
        memcpy(g_ring + h, p, (size_t)n);
        h += n;
    }
    if (h >= NRING) h -= NRING;
    g_head = h;
    g_total += n;
}

int audio_read(unsigned char *buf, int maxbytes)
{
    int t, h, avail, n;

    ensure_cs();
    EnterCriticalSection(&g_cs);
    h = g_head; t = g_tail;
    avail = (h >= t) ? (h - t) : (NRING - t + h);
    n = (maxbytes < avail) ? maxbytes : avail;
    if (n > 0) {
        int first = NRING - t;
        if (n > first) {
            memcpy(buf, g_ring + t, (size_t)first);
            memcpy(buf + first, g_ring, (size_t)(n - first));
            t = n - first;
        } else {
            memcpy(buf, g_ring + t, (size_t)n);
            t += n;
        }
        if (t >= NRING) t -= NRING;
        g_tail = t;
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

static void ring_clear(void)
{
    ensure_cs();
    EnterCriticalSection(&g_cs);
    g_tail = g_head = 0;
    LeaveCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------ */
/* waveIn callback                                                     */
/* ------------------------------------------------------------------ */

static void CALLBACK wave_cb(HWAVEIN hwi, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    WAVEHDR *hdr = (WAVEHDR *)p1;
    (void)hwi; (void)inst; (void)p2;

    if (msg != WIM_DATA || !hdr) return;
    if (hdr->dwBytesRecorded > 0 && !g_paused) {
        EnterCriticalSection(&g_cs);
        if (ring_free() >= (int)hdr->dwBytesRecorded)
            ring_push((const unsigned char *)hdr->lpData, (int)hdr->dwBytesRecorded);
        LeaveCriticalSection(&g_cs);
    }
    if (g_started) {
        hdr->dwBufferLength = BUFSZ;
        hdr->dwBytesRecorded = 0;
        waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
    }
}

/* ------------------------------------------------------------------ */
/* device enumeration                                                  */
/* ------------------------------------------------------------------ */

int audio_device_count(void)
{
    return (int)waveInGetNumDevs();
}

int audio_device_name_at(int index, char *out, int outcap)
{
    WAVEINCAPSW caps;
    if (index < 0 || index >= (int)waveInGetNumDevs()) return -1;
    memset(&caps, 0, sizeof(caps));
    if (waveInGetDevCapsW((UINT)index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) return -1;
    return wide_to_utf8(caps.szPname, out, outcap);
}

/* ------------------------------------------------------------------ */
/* open / start / stop                                                 */
/* ------------------------------------------------------------------ */

static int try_open(int dev, int rate, int ch, int bits)
{
    WAVEFORMATEX wf;
    MMRESULT r;
    int i;

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)ch;
    wf.nSamplesPerSec = (DWORD)rate;
    wf.wBitsPerSample = (WORD)bits;
    wf.nBlockAlign = (WORD)(ch * bits / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = 0;

    r = waveInOpen(&g_hwi, (UINT)dev, &wf, (DWORD_PTR)wave_cb, 0, CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        g_hwi = NULL;
        return -1;
    }
    g_rate = rate; g_ch = ch; g_bits = bits;

    for (i = 0; i < NBUF; i++) {
        g_bufdata[i] = (unsigned char *)malloc(BUFSZ);
        if (!g_bufdata[i]) return -1;
        memset(&g_hdr[i], 0, sizeof(WAVEHDR));
        g_hdr[i].lpData = (LPSTR)g_bufdata[i];
        g_hdr[i].dwBufferLength = BUFSZ;
        waveInPrepareHeader(g_hwi, &g_hdr[i], sizeof(WAVEHDR));
    }
    return 0;
}

static int pick_device(const Config *cfg)
{
    int n = (int)waveInGetNumDevs();
    int i;
    char nm[128];

    if (cfg->audio_device >= 0) {
        if (cfg->audio_device < n) return cfg->audio_device;
        return -1;
    }
    if (cfg->audio_device == -3) return -1;      /* synthetic */

    /* explicit name match first */
    if (cfg->audio_match[0]) {
        for (i = 0; i < n; i++) {
            if (audio_device_name_at(i, nm, sizeof(nm)) == 0) {
                if (strstr(nm, cfg->audio_match)) return i;
            }
        }
    }
    /* otherwise prefer the loopback-ish inputs, in the order most likely to
     * carry the machine's own output ("what you hear") */
    {
        static const char *prefer[] = {
            "立体声混音", "stereo mix", "what u hear", "what you hear",
            "wave out", "waveout", "loopback", "sum", "mono mix",
            "混音", "mix", "line in", "线路输入", NULL
        };
        int p;
        for (p = 0; prefer[p]; p++) {
            for (i = 0; i < n; i++) {
                if (audio_device_name_at(i, nm, sizeof(nm)) != 0) continue;
                if (strstr(nm, prefer[p])) return i;
            }
        }
    }
    return n > 0 ? 0 : -1;                        /* or the default device */
}

int audio_init(const Config *cfg)
{
    int dev;

    ensure_cs();
    g_dev = -1; g_name[0] = 0; g_msg[0] = 0;
    g_total = 0; g_head = g_tail = 0; g_synth = 0;

    if (!cfg->audio_enabled) {
        snprintf(g_msg, sizeof(g_msg), "audio disabled");
        return 0;
    }
    if (cfg->audio_device == -1) {
        snprintf(g_msg, sizeof(g_msg), "audio off");
        return 0;
    }

    /* synthetic source (used by --selftest and automated tests) */
    if (cfg->audio_device == -3) {
        g_synth = 1;
        g_dev = -3;
        g_rate = 44100; g_ch = 2; g_bits = 16;
        snprintf(g_name, sizeof(g_name), "synthetic sine (test)");
        snprintf(g_msg, sizeof(g_msg), "synthetic source ready");
        return 0;
    }

    dev = pick_device(cfg);
    if (dev < 0) {
        snprintf(g_msg, sizeof(g_msg), "no waveIn device available");
        return -1;
    }

    /* try the asked-for format first, then progressively simpler ones */
    {
        static const int rates[] = { 44100, 48000, 22050, 16000, 11025, 8000 };
        int ri, ci;
        int ok = -1;
        for (ri = 0; ri < (int)(sizeof(rates) / sizeof(rates[0])) && ok != 0; ri++) {
            for (ci = 0; ci < 2 && ok != 0; ci++) {
                int ch = ci == 0 ? cfg->audio_channels : 1;
                ok = try_open(dev, rates[ri], ch, cfg->audio_bits);
                if (ok == 0) break;
            }
        }
        if (ok != 0) {
            snprintf(g_msg, sizeof(g_msg), "cannot open waveIn device %d", dev);
            return -1;
        }
    }

    g_dev = dev;
    audio_device_name_at(dev, g_name, sizeof(g_name));
    snprintf(g_msg, sizeof(g_msg), "audio ready %d Hz %dch %dbit",
             g_rate, g_ch, g_bits);
    return 0;
}

int audio_start(void)
{
    int i;
    if (g_synth) { InterlockedExchange(&g_started, 1); InterlockedExchange(&g_paused, 0); return 0; }
    if (!g_hwi) return -1;
    ring_clear();
    for (i = 0; i < NBUF; i++) {
        g_hdr[i].dwBufferLength = BUFSZ;
        g_hdr[i].dwBytesRecorded = 0;
        waveInAddBuffer(g_hwi, &g_hdr[i], sizeof(WAVEHDR));
    }
    InterlockedExchange(&g_paused, 0);
    if (waveInStart(g_hwi) != MMSYSERR_NOERROR) {
        snprintf(g_msg, sizeof(g_msg), "waveInStart failed");
        return -1;
    }
    InterlockedExchange(&g_started, 1);
    return 0;
}

void audio_pause(int paused)
{
    InterlockedExchange(&g_paused, paused ? 1 : 0);
}

void audio_shutdown(void)
{
    int i;
    InterlockedExchange(&g_started, 0);
    if (g_hwi) {
        waveInStop(g_hwi);
        waveInReset(g_hwi);
        for (i = 0; i < NBUF; i++) {
            if (g_hdr[i].lpData) waveInUnprepareHeader(g_hwi, &g_hdr[i], sizeof(WAVEHDR));
        }
        waveInClose(g_hwi);
        g_hwi = NULL;
    }
    for (i = 0; i < NBUF; i++) {
        free(g_bufdata[i]);
        g_bufdata[i] = NULL;
    }
    g_synth = 0;
    g_dev = -1;
}

/* ------------------------------------------------------------------ */
/* synthetic generator - keeps the muxing path testable without sound  */
/* ------------------------------------------------------------------ */

void audio_synth_fill(int ms)
{
    /* produce `ms` milliseconds of a 440 Hz sine into the ring buffer */
    int frames, i;
    int bytes;
    unsigned char *tmp;

    if (!g_synth || g_paused || !g_started) return;
    frames = g_rate * ms / 1000;
    bytes = frames * g_ch * (g_bits / 8);
    if (bytes <= 0 || bytes > (1 << 20)) return;
    tmp = (unsigned char *)malloc((size_t)bytes);
    if (!tmp) return;
    for (i = 0; i < frames; i++) {
        short s = (short)(sin(g_synth_phase) * 12000.0);
        int c;
        g_synth_phase += 2.0 * 3.14159265358979 * 440.0 / g_rate;
        if (g_synth_phase > 6.283185307) g_synth_phase -= 6.283185307;
        for (c = 0; c < g_ch; c++) {
            tmp[(i * g_ch + c) * 2 + 0] = (unsigned char)(s & 0xff);
            tmp[(i * g_ch + c) * 2 + 1] = (unsigned char)((s >> 8) & 0xff);
        }
    }
    EnterCriticalSection(&g_cs);
    if (ring_free() >= bytes) ring_push(tmp, bytes);
    LeaveCriticalSection(&g_cs);
    free(tmp);
}

long long audio_total_bytes(void) { return g_total; }

void audio_status(AudioStatus *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = g_started ? 1 : 0;
    out->device = g_dev;
    out->rate = g_rate;
    out->channels = g_ch;
    out->bits = g_bits;
    out->bytes = g_total;
    strncpy(out->name, g_name, sizeof(out->name) - 1);
    strncpy(out->message, g_msg, sizeof(out->message) - 1);
}

const char *audio_format_desc(void)
{
    static char b[96];
    snprintf(b, sizeof(b), "%d Hz / %d ch / %d bit", g_rate, g_ch, g_bits);
    return b;
}

int audio_selftest(char *msg, int msgcap)
{
    int n = audio_device_count();
    int i, ok = 1;
    char nm[128];
    size_t off = 0;

    if (msgcap > 0) msg[0] = 0;
    off += (size_t)snprintf(msg + off, (size_t)(msgcap - (int)off),
                            "waveIn devices: %d\n", n);
    for (i = 0; i < n && off < (size_t)msgcap - 40; i++) {
        if (audio_device_name_at(i, nm, sizeof(nm)) == 0)
            off += (size_t)snprintf(msg + off, (size_t)(msgcap - (int)off),
                                    "    [%d] %s\n", i, nm);
    }
    if (n == 0) {
        off += (size_t)snprintf(msg + off, (size_t)(msgcap - (int)off),
                                "    (no capture device - audio can still be tested with the synthetic source)\n");
    }
    return ok ? 0 : -1;
}
