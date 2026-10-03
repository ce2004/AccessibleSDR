/* Minimal libao replacement: see ao/ao.h */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <io.h>
#include <fcntl.h>
#include "ao/ao.h"

enum { DRV_LIVE = 1, DRV_WAV = 2, DRV_RAW = 3 };
#define NBUF 8

struct ao_device {
    int driver;
    FILE *fp;
    int to_stdout;
    long data_bytes;
    WAVEFORMATEX wf;
    HWAVEOUT wo;
    HANDLE ev;
    WAVEHDR hdr[NBUF];
    int next;
};

void ao_initialize(void) {}
void ao_shutdown(void) {}
int ao_default_driver_id(void) { return DRV_LIVE; }

int ao_driver_id(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "wav")) return DRV_WAV;
    if (!strcmp(name, "raw")) return DRV_RAW;
    if (!strcmp(name, "wmm") || !strcmp(name, "live")) return DRV_LIVE;
    return -1;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void write_wav_header(ao_device *d, uint32_t data_bytes)
{
    unsigned char h[44];
    int ch = d->wf.nChannels, rate = d->wf.nSamplesPerSec, bits = d->wf.wBitsPerSample;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data_bytes); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, ch); put32(h + 24, rate);
    put32(h + 28, rate * ch * bits / 8); put16(h + 32, ch * bits / 8); put16(h + 34, bits);
    memcpy(h + 36, "data", 4); put32(h + 40, data_bytes);
    fwrite(h, 1, 44, d->fp);
}

static ao_device *alloc_dev(int driver, ao_sample_format *f)
{
    if (!f || f->bits != 16 || f->channels < 1 || f->channels > 2) return NULL;
    ao_device *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->driver = driver;
    d->wf.wFormatTag = WAVE_FORMAT_PCM; d->wf.nChannels = (WORD)f->channels;
    d->wf.nSamplesPerSec = f->rate; d->wf.wBitsPerSample = 16;
    d->wf.nBlockAlign = (WORD)(2 * f->channels); d->wf.nAvgBytesPerSec = f->rate * d->wf.nBlockAlign;
    return d;
}

ao_device *ao_open_live(int driver_id, ao_sample_format *f, ao_option *o)
{
    (void)o;
    if (driver_id != DRV_LIVE) return NULL;
    ao_device *d = alloc_dev(DRV_LIVE, f);
    if (!d) return NULL;
    d->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&d->wo, WAVE_MAPPER, &d->wf, (DWORD_PTR)d->ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(d->ev); free(d); return NULL;
    }
    for (int i = 0; i < NBUF; i++) d->hdr[i].dwFlags = WHDR_DONE;
    return d;
}

ao_device *ao_open_file(int driver_id, const char *name, int overwrite, ao_sample_format *f, ao_option *o)
{
    (void)o;
    if (driver_id != DRV_WAV && driver_id != DRV_RAW) return NULL;
    ao_device *d = alloc_dev(driver_id, f);
    if (!d) return NULL;
    if (!strcmp(name, "-")) {
        _setmode(_fileno(stdout), _O_BINARY);
        d->fp = stdout; d->to_stdout = 1;
    } else {
        if (!overwrite) { FILE *t = fopen(name, "rb"); if (t) { fclose(t); free(d); return NULL; } }
        d->fp = fopen(name, "wb");
        if (!d->fp) { free(d); return NULL; }
    }
    if (driver_id == DRV_WAV) write_wav_header(d, 0x7FFFFFFF - 36);  /* streaming size; fixed on close for files */
    return d;
}

int ao_play(ao_device *d, char *samples, unsigned int n)
{
    if (!d) return 0;
    if (d->driver != DRV_LIVE) {
        if (fwrite(samples, 1, n, d->fp) != n) return 0;
        if (d->to_stdout) fflush(d->fp);
        d->data_bytes += n;
        return 1;
    }
    WAVEHDR *h = &d->hdr[d->next];
    while (!(h->dwFlags & WHDR_DONE)) WaitForSingleObject(d->ev, 100);
    if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(d->wo, h, sizeof *h);
    if (!h->lpData || h->dwUser < n) {   /* dwUser holds this buffer's capacity */
        char *p = realloc(h->lpData, n);
        if (!p) return 0;
        h->lpData = p; h->dwUser = n;
    }
    memcpy(h->lpData, samples, n);
    {   /* live volume, percent from SDR_VOLUME (default 30) */
        static int vol = -1;
        if (vol < 0) { const char *e = getenv("SDR_VOLUME"); vol = e ? atoi(e) : 30; if (vol < 0) vol = 0; if (vol > 100) vol = 100; }
        short *s = (short *)h->lpData;
        for (unsigned int k = 0; k < n / 2; k++) s[k] = (short)(s[k] * vol / 100);
    }
    h->dwBufferLength = n; h->dwFlags = 0;
    waveOutPrepareHeader(d->wo, h, sizeof *h);
    waveOutWrite(d->wo, h, sizeof *h);
    d->next = (d->next + 1) % NBUF;
    return 1;
}

int ao_close(ao_device *d)
{
    if (!d) return 0;
    if (d->driver == DRV_LIVE) {
        for (int i = 0; i < NBUF; i++)
            while (!(d->hdr[i].dwFlags & WHDR_DONE)) WaitForSingleObject(d->ev, 100);
        waveOutReset(d->wo);
        for (int i = 0; i < NBUF; i++) {
            if (d->hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(d->wo, &d->hdr[i], sizeof d->hdr[i]);
            free(d->hdr[i].lpData);
        }
        waveOutClose(d->wo);
        CloseHandle(d->ev);
    } else {
        if (d->driver == DRV_WAV && !d->to_stdout && fseek(d->fp, 0, SEEK_SET) == 0)
            write_wav_header(d, (uint32_t)d->data_bytes);
        if (!d->to_stdout) fclose(d->fp); else fflush(d->fp);
    }
    free(d);
    return 1;
}
