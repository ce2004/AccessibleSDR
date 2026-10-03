/* play: raw signed 16-bit little-endian PCM from stdin to the default audio output.
   usage: play <rate> [channels]
   Example: rtl_fm -M wbfm -f 89.1M - | play 32000 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>

#define NBUF 8

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: play <rate> [channels]\n"); return 1; }
    int rate = atoi(argv[1]), ch = argc > 2 ? atoi(argv[2]) : 1;
    if (rate < 1000 || ch < 1 || ch > 2) { fprintf(stderr, "bad rate or channels\n"); return 1; }
    _setmode(_fileno(stdin), _O_BINARY);

    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = (WORD)ch; wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16; wf.nBlockAlign = (WORD)(2 * ch); wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    HWAVEOUT wo;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "cannot open audio output\n"); return 1;
    }
    /* ~50 ms per buffer, 8 buffers: ~400 ms of slack */
    DWORD bytes = (rate / 20) * wf.nBlockAlign;
    WAVEHDR hdr[NBUF] = {0};
    for (int i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)malloc(bytes);
        hdr[i].dwFlags = WHDR_DONE;
    }
    int i = 0, eof = 0;
    while (!eof) {
        WAVEHDR *h = &hdr[i];
        while (!(h->dwFlags & WHDR_DONE)) WaitForSingleObject(ev, 100);
        if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
        size_t got = 0;
        while (got < bytes) {
            size_t n = fread(h->lpData + got, 1, bytes - got, stdin);
            if (n == 0) { eof = 1; break; }
            got += n;
        }
        got -= got % wf.nBlockAlign;
        if (got == 0) break;
        h->dwBufferLength = (DWORD)got; h->dwFlags = 0;
        waveOutPrepareHeader(wo, h, sizeof *h);
        waveOutWrite(wo, h, sizeof *h);
        i = (i + 1) % NBUF;
    }
    for (int k = 0; k < NBUF; k++)
        while (!(hdr[k].dwFlags & WHDR_DONE)) WaitForSingleObject(ev, 100);
    waveOutReset(wo);
    for (int k = 0; k < NBUF; k++) if (hdr[k].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[k], sizeof hdr[k]);
    waveOutClose(wo);
    return 0;
}
