/* wfm: broadcast FM receiver for the RTL-SDR, stereo, 48 kHz, straight to the default audio device.
   usage: wfm <MHz> [-v volume_percent] [-g gain_dB] [-t seconds] [--mono] [--tee file.raw]
   Volume defaults to 30%. Every 2 seconds it prints signal level, noise and stereo/mono.
   --tee also writes the 48 kHz stereo s16 audio to a file, for measuring what you hear.

   Signal path: 960 kS/s IQ, tuned 240 kHz above the station (keeps the DC spike away)
   -> shift by fs/4 -> 100 kHz channel filter, /4 -> 240 kHz -> FM discriminator (MPX)
   -> 19 kHz pilot PLL -> L+R and L-R (38 kHz, phase 2x pilot) -> 15 kHz low-pass, /5
   -> 48 kHz -> 75 us de-emphasis -> L/R. */
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl-sdr.h"

#define FS_IN     960000
#define OFFSET    240000            /* = FS_IN / 4 */
#define D1        4
#define FS_MPX    (FS_IN / D1)      /* 240000 */
#define D2        5
#define FS_AUD    (FS_MPX / D2)     /* 48000 */
#define CH_TAPS   255               /* sharp enough to reject HD Radio sidebands from 129 kHz */
#define AU_TAPS   241
#define NBUF      24
#define BUF_FRAMES 2400             /* 50 ms */
#define PI        3.14159265358979323846

static void design_lowpass(float *h, int n, double cutoff, double fs)
{
    double fc = cutoff / fs, sum = 0;
    for (int i = 0; i < n; i++) {
        double m = i - (n - 1) / 2.0;
        double sinc = m == 0 ? 2 * fc : sin(2 * PI * fc * m) / (PI * m);
        double w = 0.42 - 0.5 * cos(2 * PI * i / (n - 1)) + 0.08 * cos(4 * PI * i / (n - 1));
        h[i] = (float)(sinc * w);
        sum += h[i];
    }
    for (int i = 0; i < n; i++) h[i] = (float)(h[i] / sum);
}

/* --- state --- */
static float ch_h[CH_TAPS], au_h[AU_TAPS];
static float ci[2 * CH_TAPS], cq[2 * CH_TAPS]; static int cpos, cphase;   /* channel delay line */
static float ml[2 * AU_TAPS], sl[2 * AU_TAPS]; static int apos, aphase;   /* audio delay lines */
static float prev_i = 1, prev_q = 0;
static double pll_theta, pll_omega, lock;
static float de_l, de_r, de_a;
static int mono_only, stereo_shown = -1;
static float volume = 0.30f;
/* automatic centring: fine NCO after the fs/4 shift, steered by the discriminator's average (= offset) */
static double afc_hz, afc_phase, afc_acc; static long afc_n;
/* DC blockers on the audio (~10 Hz high-pass) */
static float dc_xl, dc_yl, dc_xr, dc_yr;
/* quality meters: RF level of the filtered channel, and MPX noise in the empty 16.5-18 kHz guard band */
static double rf_acc, nz_i1, nz_q1, nz_i2, nz_q2, nz_acc, nz_phase; static long meter_n;
static unsigned long mix_n;

static rtlsdr_dev_t *dev;
static HWAVEOUT wo;
static WAVEHDR hdr[NBUF];
static int fill_idx, fill_frames, queued, started, dropped, underruns;
static FILE *tee;
static volatile LONG stop_flag;

static int busy(WAVEHDR *h) { return (h->dwFlags & WHDR_PREPARED) && !(h->dwFlags & WHDR_DONE); }

static void submit_current(void)
{
    WAVEHDR *h = &hdr[fill_idx];
    int pending = 0;
    for (int i = 0; i < NBUF; i++) pending += busy(&hdr[i]);
    if (started && pending == 0) {            /* playback ran dry: re-buffer before resuming */
        waveOutPause(wo); started = 0; queued = 0; underruns++;
    }
    if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
    h->dwBufferLength = BUF_FRAMES * 4; h->dwFlags = 0;
    waveOutPrepareHeader(wo, h, sizeof *h);
    waveOutWrite(wo, h, sizeof *h);
    if (!started && ++queued >= 4) { waveOutRestart(wo); started = 1; }
    fill_idx = (fill_idx + 1) % NBUF;
    fill_frames = 0;
}

static void emit(float l, float r)
{
    WAVEHDR *h = &hdr[fill_idx];
    if (fill_frames == 0 && busy(h)) { dropped++; return; }  /* output is behind: drop this frame */
    short *p = (short *)h->lpData + 2 * fill_frames;
    float sl_ = l * 18500.f * volume, sr_ = r * 18500.f * volume;   /* 18500: ~3 dB headroom at 100% */
    p[0] = (short)(sl_ > 32767 ? 32767 : sl_ < -32768 ? -32768 : sl_);
    p[1] = (short)(sr_ > 32767 ? 32767 : sr_ < -32768 ? -32768 : sr_);
    if (tee) fwrite(p, 4, 1, tee);
    if (++fill_frames == BUF_FRAMES) submit_current();
}

static void mpx_sample(float mpx)
{
    /* pilot PLL (loop ~30 Hz); phase detector -sin(theta) locks theta to the pilot's cosine phase */
    double s = sin(pll_theta), c = cos(pll_theta);
    double err = mpx * -s;
    pll_omega += 1.2e-5 * err;
    double nom = 2 * PI * 19000.0 / FS_MPX, lim = 2 * PI * 20.0 / FS_MPX;
    if (pll_omega > lim) pll_omega = lim; else if (pll_omega < -lim) pll_omega = -lim;
    pll_theta += nom + pll_omega + 0.03 * err;
    if (pll_theta > 2 * PI) pll_theta -= 2 * PI;
    lock = 0.9995 * lock + 0.0005 * (mpx * c);        /* ~ half the pilot amplitude when locked */
    float diff = (float)(mpx * 2 * cos(2 * pll_theta));

    /* guard-band noise: shift 17.25 kHz to DC, two one-pole low-passes (~1 kHz), average power */
    nz_phase += 2 * PI * 17250.0 / FS_MPX;
    if (nz_phase > 2 * PI) nz_phase -= 2 * PI;
    double a = 0.026;
    nz_i1 += a * (mpx * cos(nz_phase) - nz_i1); nz_q1 += a * (-mpx * sin(nz_phase) - nz_q1);
    nz_i2 += a * (nz_i1 - nz_i2);               nz_q2 += a * (nz_q1 - nz_q2);
    nz_acc += nz_i2 * nz_i2 + nz_q2 * nz_q2;

    /* the discriminator's long-term average is the remaining tuning error (mpx 1.0 = 75 kHz) */
    afc_acc += mpx;
    /* audio content biases short averages, so average 8 s and correct gently; real drift is slow */
    if (++afc_n == 8 * FS_MPX) {
        double err_hz = afc_acc / afc_n * 75000.0;
        afc_hz += 0.3 * err_hz;
        afc_acc = 0; afc_n = 0;
    }
    if (++meter_n == 2 * FS_MPX) {
        fprintf(stderr, "Signal %5.1f dBFS, %s, centring correction %+6.0f Hz\n", 10 * log10(rf_acc / meter_n + 1e-12),
                stereo_shown == 1 ? "stereo" : "mono", afc_hz);
        rf_acc = nz_acc = 0; meter_n = 0;
    }

    ml[apos] = ml[apos + AU_TAPS] = mpx;
    sl[apos] = sl[apos + AU_TAPS] = diff;
    apos = (apos + 1) % AU_TAPS;
    if (++aphase < D2) return;
    aphase = 0;
    float m = 0, d = 0;
    const float *pm = ml + apos, *pd = sl + apos;
    for (int k = 0; k < AU_TAPS; k++) { m += au_h[k] * pm[k]; d += au_h[k] * pd[k]; }

    /* hysteresis: switching between stereo and mono is audible, so don't flutter at the threshold */
    int stereo = !mono_only && (stereo_shown == 1 ? lock > 0.012 : lock > 0.022);
    if (stereo != stereo_shown) { fprintf(stderr, stereo ? "Stereo\n" : "Mono\n"); stereo_shown = stereo; }
    float l = stereo ? m + d : m, r = stereo ? m - d : m;
    de_l = de_a * de_l + (1 - de_a) * l;
    de_r = de_a * de_r + (1 - de_a) * r;
    const float R = 0.9987f;   /* ~10 Hz at 48 kHz: below any music, removes DC and slow wander */
    dc_yl = de_l - dc_xl + R * dc_yl; dc_xl = de_l;
    dc_yr = de_r - dc_xr + R * dc_yr; dc_xr = de_r;
    emit(dc_yl, dc_yr);
}

static void LIBUSB_CALL_cb(unsigned char *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (stop_flag) { rtlsdr_cancel_async(dev); return; }
    for (uint32_t n = 0; n + 1 < len; n += 2) {
        float i = (buf[n] - 127.5f) / 127.5f, q = (buf[n + 1] - 127.5f) / 127.5f, t;
        switch (mix_n++ & 3) {                 /* multiply by j^n: shift up by fs/4 */
        case 1: t = i; i = -q; q = t; break;
        case 2: i = -i; q = -q; break;
        case 3: t = i; i = q; q = -t; break;
        }
        if (afc_hz != 0) {                     /* fine centring: rotate by -afc_hz */
            afc_phase -= 2 * PI * afc_hz / FS_IN;
            if (afc_phase < -PI) afc_phase += 2 * PI; else if (afc_phase > PI) afc_phase -= 2 * PI;
            float c = (float)cos(afc_phase), s = (float)sin(afc_phase);
            t = i * c - q * s; q = i * s + q * c; i = t;
        }
        ci[cpos] = ci[cpos + CH_TAPS] = i;
        cq[cpos] = cq[cpos + CH_TAPS] = q;
        cpos = (cpos + 1) % CH_TAPS;
        if (++cphase < D1) continue;
        cphase = 0;
        float yi = 0, yq = 0;
        const float *pi = ci + cpos, *pq = cq + cpos;
        for (int k = 0; k < CH_TAPS; k++) { yi += ch_h[k] * pi[k]; yq += ch_h[k] * pq[k]; }
        /* discriminator: angle of y * conj(prev), scaled so 75 kHz deviation = 1.0 */
        rf_acc += yi * yi + yq * yq;
        float re = yi * prev_i + yq * prev_q, im = yq * prev_i - yi * prev_q;
        prev_i = yi; prev_q = yq;
        mpx_sample(atan2f(im, re) * (float)(FS_MPX / (2 * PI * 75000.0)));
    }
}

static BOOL WINAPI on_ctrl(DWORD t) { (void)t; InterlockedExchange(&stop_flag, 1); return TRUE; }

static DWORD WINAPI timer_thread(LPVOID p) { Sleep((DWORD)(UINT_PTR)p); InterlockedExchange(&stop_flag, 1); return 0; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: wfm <MHz> [-g gain_dB] [-t seconds] [--mono] [--tee file.raw]\n"); return 1; }
    double mhz = atof(argv[1]), gain = 20.7, secs = 0;
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "-g") && a + 1 < argc) gain = atof(argv[++a]);
        else if (!strcmp(argv[a], "-v") && a + 1 < argc) {
            double v = atof(argv[++a]);
            volume = (float)((v < 0 ? 0 : v > 100 ? 100 : v) / 100.0);
        }
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) secs = atof(argv[++a]);
        else if (!strcmp(argv[a], "--mono")) mono_only = 1;
        else if (!strcmp(argv[a], "--tee") && a + 1 < argc) tee = fopen(argv[++a], "wb");
    }
    if (mhz < 24 || mhz > 1766) { fprintf(stderr, "frequency must be in MHz, for example 89.1\n"); return 1; }

    design_lowpass(ch_h, CH_TAPS, 105000, FS_IN);
    design_lowpass(au_h, AU_TAPS, 16000, FS_MPX);
    de_a = (float)exp(-1.0 / (FS_AUD * 75e-6));

    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = FS_AUD;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = FS_AUD * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "cannot open audio output\n"); return 1;
    }
    waveOutPause(wo);
    for (int i = 0; i < NBUF; i++) { hdr[i].lpData = calloc(BUF_FRAMES, 4); hdr[i].dwFlags = WHDR_DONE; }

    int r = -1;
    for (int tries = 0; tries < 5 && r < 0; tries++) {   /* the device can be briefly busy after another program */
        r = rtlsdr_open(&dev, 0);
        if (r < 0) Sleep(400);
    }
    if (r < 0) { fprintf(stderr, "cannot open the RTL-SDR (is another radio program running?)\n"); return 1; }
    rtlsdr_set_sample_rate(dev, FS_IN);
    rtlsdr_set_center_freq(dev, (uint32_t)(mhz * 1e6 + OFFSET + 0.5));
    rtlsdr_set_tuner_gain_mode(dev, 1);
    rtlsdr_set_tuner_gain(dev, (int)(gain * 10 + 0.5));
    rtlsdr_reset_buffer(dev);
    fprintf(stderr, "Playing %.1f MHz, gain %.1f dB, volume %d%%. Ctrl+C stops.\n", mhz,
            rtlsdr_get_tuner_gain(dev) / 10.0, (int)(volume * 100 + 0.5));

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (secs > 0) CreateThread(NULL, 0, timer_thread, (LPVOID)(UINT_PTR)(secs * 1000), 0, NULL);
    rtlsdr_read_async(dev, LIBUSB_CALL_cb, NULL, 15, 65536);
    rtlsdr_close(dev);

    for (int i = 0; i < NBUF; i++) while (busy(&hdr[i])) Sleep(20);
    waveOutReset(wo);
    for (int i = 0; i < NBUF; i++) if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
    waveOutClose(wo);
    if (tee) fclose(tee);
    fprintf(stderr, "Stopped. %d audio frames dropped, %d re-buffers.\n", dropped, underruns);
    return 0;
}
