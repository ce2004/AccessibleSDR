/* tuner: prototype "analog dial" receiver window. Nothing is decoded: every signal is heard raw.
   Tab / Shift+Tab: next / previous category (from presets.txt next to the exe).
   PageUp / PageDown: next / previous preset in the category.
   Up / Down: tap a small step, hold to glide (speeds up). Shift: finer.
   M / Shift+M: receiver mode (wide FM, AM, narrow FM, upper sideband, lower sideband, CW).
   Enter (wide FM): look for HD and switch to it. On HD, Up/Down pick HD1..HD4, Escape = analog.
   N: HD song. Space: frequency. Left/Right: volume. Alt+F4 closes.
   The dongle captures 1.488 MHz and the station is picked out digitally, so tuning is seamless;
   the hardware only retunes near the edge of that window. If the dongle is missing or
   unplugged it retries every 2 seconds. */
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setupapi.h>
#include <newdev.h>
#include <winhttp.h>
#include <shellapi.h>
#include "rtl-sdr.h"
#include "nrsc5.h"

#define WSTR2(x) L##x
#define WSTR(x) WSTR2(x)
#define TUNER_VERSION "1.0.0"
#define UPDATE_REPO   "ce2004/AccessibleSDR"
#if defined(_M_ARM64)
#define UPDATE_ASSET  "win-arm64.zip"
#else
#define UPDATE_ASSET  "win-x64.zip"
#endif

#define FS_IN      1488375
#define D1         6
#define FS_MPX     (FS_IN / (double)D1)     /* 248062.5 */
#define D2         5
#define FS_AUD     49612                    /* 248062.5 / 5, rounded for waveOut */
#define CH_TAPS    383
#define AU_TAPS    241
#define HB_TAPS    47
#define NBUF       24
#define BUF_FRAMES 744                      /* 15 ms */
#define USB_BUFS   16
#define USB_LEN    16384
#define PI         3.14159265358979323846
#define OFF_MIN    (-595000.0)             /* usable station positions below the LO */
#define OFF_MAX    (-120000.0)
#define OFF_MID    (-357500.0)
#define HD_RING    (1 << 21)               /* floats: ~1.4 s of complex samples at 744 kHz */
#define D_N        31                      /* narrow modes: 1488375 / 31 = 48012 Hz */
#define FS_N       (FS_IN / (double)D_N)
#define ND_TAPS    767
#define NCH_TAPS   129
#define SSB_TAPS   255
#define F_MIN      0.5e6
#define F_MAX      1766e6

/* ---------- modes ---------- */
enum { M_WFM, M_AM, M_NFM, M_USB, M_LSB, M_CW, M_COUNT };
static const char *mode_names[M_COUNT] = { "wide FM", "AM", "narrow FM", "upper sideband", "lower sideband", "CW" };
static const char *mode_keys[M_COUNT] = { "WFM", "AM", "NFM", "USB", "LSB", "CW" };

/* ---------- categories and presets ---------- */
typedef struct { char name[96]; double hz; int mode; } preset_t;
typedef struct { char name[64]; double lo, hi; int mode, first, count; double last; int last_mode; } cat_t;
#define MAX_CATS 64
#define MAX_PRESETS 4000
static cat_t cats[MAX_CATS];
static preset_t presets[MAX_PRESETS];
static int ncats, npresets;
static volatile int g_cat, g_mode = M_WFM;

static int parse_mode(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    for (int m = 0; m < M_COUNT; m++)
        if (!_strnicmp(s, mode_keys[m], strlen(mode_keys[m]))) return m;
    return M_NFM;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

static int cmp_preset(const void *a, const void *b)
{
    double d = ((const preset_t *)a)->hz - ((const preset_t *)b)->hz;
    return d < 0 ? -1 : d > 0;
}

static void add_cat(const char *name, double lo, double hi, int mode)
{
    if (ncats >= MAX_CATS) return;
    cat_t *c = &cats[ncats++];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->lo = lo; c->hi = hi; c->mode = mode; c->first = npresets; c->count = 0; c->last = 0; c->last_mode = mode;
}

static void add_preset(const char *name, double hz, int mode)
{
    if (!ncats || npresets >= MAX_PRESETS) return;
    preset_t *p = &presets[npresets++];
    snprintf(p->name, sizeof p->name, "%s", name);
    p->hz = hz; p->mode = mode;
    cats[ncats - 1].count++;
}

#define P(name, mhz, mode) add_preset(name, (mhz) * 1e6, mode)
static void builtin_presets(void)
{
    char n[96];
    add_cat("FM radio", 87.5e6, 108e6, M_WFM);
    add_cat("AM radio", 0.53e6, 1.71e6, M_AM);          /* FM and AM stations come from Explore (X) */

    add_cat("Weather", 162.4e6, 162.55e6, M_NFM);
    P("Weather 162.4", 162.4, M_NFM); P("Weather 162.425", 162.425, M_NFM); P("Weather 162.45", 162.45, M_NFM);
    P("Weather 162.475", 162.475, M_NFM); P("Weather 162.5", 162.5, M_NFM); P("Weather 162.525", 162.525, M_NFM);
    P("Weather 162.55", 162.55, M_NFM);

    add_cat("Aircraft", 118e6, 137e6, M_AM);
    P("Guard, emergency", 121.5, M_AM); P("Air to air", 122.75, M_AM); P("Unicom", 122.8, M_AM);
    P("Multicom", 122.9, M_AM); P("Unicom 123.0", 123.0, M_AM); P("Air to air, 123.45", 123.45, M_AM);
    P("ACARS data", 129.125, M_AM); P("ACARS data", 130.025, M_AM); P("ACARS data", 131.55, M_AM);

    add_cat("Military aircraft", 225e6, 400e6, M_AM);
    P("Military guard, emergency", 243.0, M_AM);

    add_cat("Shortwave", 2.3e6, 26.1e6, M_AM);
    P("90 meter band", 3.2, M_AM); P("60 meter band", 4.75, M_AM); P("49 meter band", 5.9, M_AM);
    P("41 meter band", 7.2, M_AM); P("31 meter band", 9.4, M_AM); P("25 meter band", 11.6, M_AM);
    P("22 meter band", 13.57, M_AM); P("19 meter band", 15.1, M_AM); P("16 meter band", 17.48, M_AM);
    P("13 meter band", 21.45, M_AM); P("11 meter band", 25.67, M_AM);

    add_cat("Time and odd signals", 2.4e6, 25.1e6, M_AM);
    P("WWV 2.5", 2.5, M_AM); P("CHU Canada 3330", 3.33, M_USB); P("The Buzzer, Russia", 4.625, M_USB);
    P("WWV 5", 5.0, M_AM); P("CHU Canada 7850", 7.85, M_USB); P("Air Force HF, 8992", 8.992, M_USB);
    P("WWV 10", 10.0, M_AM); P("Air Force HF, 11175", 11.175, M_USB); P("CHU Canada 14670", 14.67, M_USB);
    P("WWV 15", 15.0, M_AM); P("WWV 20", 20.0, M_AM); P("WWV 25", 25.0, M_AM);

    add_cat("Ham HF", 1.8e6, 29.7e6, M_LSB);
    P("160 meters", 1.9, M_LSB); P("80 meters", 3.85, M_LSB); P("FT8 80 meters", 3.573, M_USB);
    P("40 meters", 7.2, M_LSB); P("FT8 40 meters", 7.074, M_USB); P("CW 40 meters", 7.03, M_CW);
    P("FT8 30 meters", 10.136, M_USB); P("20 meters", 14.2, M_USB); P("FT8 20 meters", 14.074, M_USB);
    P("CW 20 meters", 14.03, M_CW); P("17 meters", 18.13, M_USB); P("15 meters", 21.3, M_USB);
    P("FT8 15 meters", 21.074, M_USB); P("12 meters", 24.95, M_USB); P("10 meters", 28.4, M_USB);
    P("FT8 10 meters", 28.074, M_USB);

    add_cat("Ham VHF and UHF", 50e6, 450e6, M_NFM);
    P("6 meters calling", 50.125, M_USB); P("FT8 6 meters", 50.313, M_USB); P("FT8 2 meters", 144.174, M_USB);
    P("APRS data", 144.39, M_NFM); P("Space station voice", 145.8, M_NFM); P("Space station packet", 145.825, M_NFM);
    P("2 meter calling", 146.52, M_NFM); P("1.25 meter calling", 223.5, M_NFM); P("70 centimeter calling", 446.0, M_NFM);

    add_cat("CB radio", 26.965e6, 27.405e6, M_AM);
    {
        static const double cb[40] = { 26.965, 26.975, 26.985, 27.005, 27.015, 27.025, 27.035, 27.055, 27.065, 27.075,
            27.085, 27.105, 27.115, 27.125, 27.135, 27.155, 27.165, 27.175, 27.185, 27.205, 27.215, 27.225, 27.255,
            27.235, 27.245, 27.265, 27.275, 27.285, 27.295, 27.305, 27.315, 27.325, 27.335, 27.345, 27.355, 27.365,
            27.375, 27.385, 27.395, 27.405 };
        for (int i = 0; i < 40; i++) {
            snprintf(n, sizeof n, "CB channel %d%s", i + 1, i == 8 ? ", emergency" : i == 18 ? ", truckers" : "");
            add_preset(n, cb[i] * 1e6, M_AM);
        }
    }

    add_cat("FRS and GMRS", 462.55e6, 467.725e6, M_NFM);
    for (int i = 0; i < 22; i++) {
        double f = i < 7 ? 462.5625 + 0.025 * i : i < 14 ? 467.5625 + 0.025 * (i - 7) : 462.55 + 0.025 * (i - 14);
        snprintf(n, sizeof n, "FRS channel %d", i + 1);
        add_preset(n, f * 1e6, M_NFM);
    }

    add_cat("MURS", 151.82e6, 154.6e6, M_NFM);
    P("MURS channel 1", 151.82, M_NFM); P("MURS channel 2", 151.88, M_NFM); P("MURS channel 3", 151.94, M_NFM);
    P("MURS blue dot", 154.57, M_NFM); P("MURS green dot", 154.6, M_NFM);

    add_cat("Marine", 156e6, 162.025e6, M_NFM);
    P("Marine channel 6, safety", 156.3, M_NFM); P("Marine channel 9, calling", 156.45, M_NFM);
    P("Marine channel 13, bridge to bridge", 156.65, M_NFM); P("Marine channel 16, distress", 156.8, M_NFM);
    P("Marine channel 22A, Coast Guard", 157.1, M_NFM); P("AIS ship data", 161.975, M_NFM); P("AIS ship data", 162.025, M_NFM);

    add_cat("Railroad", 160.215e6, 161.565e6, M_NFM);
    for (int i = 0; i <= 90; i++) {
        snprintf(n, sizeof n, "Railroad %.3f", 160.215 + 0.015 * i);
        add_preset(n, (160.215 + 0.015 * i) * 1e6, M_NFM);
    }

    add_cat("Fire, EMS and public safety", 150e6, 860e6, M_NFM);
    P("Interop VTAC 11", 151.1375, M_NFM); P("Fire mutual aid", 154.265, M_NFM); P("Fire mutual aid", 154.28, M_NFM);
    P("Fire mutual aid", 154.295, M_NFM); P("Interop VTAC 12", 154.4525, M_NFM); P("Hospital HEAR", 155.34, M_NFM);
    P("Interop VCALL 10", 155.7525, M_NFM); P("Interop VTAC 13", 158.7375, M_NFM); P("Interop VTAC 14", 159.4725, M_NFM);
    P("Interop UCALL 40", 453.2125, M_NFM);
    for (int i = 0; i < 8; i++) { snprintf(n, sizeof n, "EMS med channel %d", i + 1); add_preset(n, (463.0 + 0.025 * i) * 1e6, M_NFM); }
    P("Interop 8CALL 90", 851.0125, M_NFM);

    add_cat("Tune anywhere", F_MIN, F_MAX, M_NFM);
    P("Low end", 1.0, M_AM); P("VHF low", 40.0, M_NFM); P("VHF high", 150.0, M_NFM); P("UHF", 450.0, M_NFM);
    P("900 megahertz", 915.0, M_NFM); P("L band", 1500.0, M_NFM);
}
#undef P

/* ---------- saved presets (Shift+Enter), the last Tab category, kept in saved.txt ---------- */
#define MAX_SAVED 1000
static preset_t saved[MAX_SAVED];
static int nsaved, saved_cat = -1;
static char saved_path[MAX_PATH];

/* "Found here": what Explore (X) identified at this location, kept in found.txt */
static preset_t found[MAX_SAVED];
static int nfound, found_cat = -1;
static char found_path[MAX_PATH];

static preset_t *item(int c, int i)
{
    return c == saved_cat ? &saved[i] : c == found_cat ? &found[i] : &presets[cats[c].first + i];
}

static void write_found(void)
{
    FILE *fp = fopen(found_path, "w");
    if (!fp) return;
    fprintf(fp, "# Signals identified by Explore (X) at this location. Name | MHz | mode\n");
    for (int i = 0; i < nfound; i++)
        fprintf(fp, "%s | %.6f | %s\n", found[i].name, found[i].hz / 1e6, mode_keys[found[i].mode]);
    fclose(fp);
}

static void write_saved(void)
{
    FILE *fp = fopen(saved_path, "w");
    if (!fp) return;
    fprintf(fp, "# Presets saved with Shift+Enter. Name | MHz | mode\n");
    for (int i = 0; i < nsaved; i++)
        fprintf(fp, "%s | %.6f | %s\n", saved[i].name, saved[i].hz / 1e6, mode_keys[saved[i].mode]);
    fclose(fp);
}

/* presets_path is the presets.txt that was used (or tried); saved.txt sits beside it */
static void load_saved(const char *presets_path)
{
    char line[512];
    snprintf(saved_path, sizeof saved_path, "%s", presets_path);
    char *slash = strrchr(saved_path, '\\');
    if (slash) strcpy(slash + 1, "saved.txt");
    FILE *fp = fopen(saved_path, "r");
    if (fp) {
        while (fgets(line, sizeof line, fp) && nsaved < MAX_SAVED) {
            char *s = trim(line);
            if (!*s || *s == '#') continue;
            char *f[3] = {0}; int k = 0;
            for (char *t = strtok(s, "|"); t && k < 3; t = strtok(NULL, "|")) f[k++] = trim(t);
            if (k >= 2 && atof(f[1]) > 0) {
                preset_t *p = &saved[nsaved++];
                snprintf(p->name, sizeof p->name, "%s", f[0]);
                p->hz = atof(f[1]) * 1e6; p->mode = k == 3 ? parse_mode(f[2]) : M_NFM;
            }
        }
        fclose(fp);
    }
    qsort(saved, nsaved, sizeof(preset_t), cmp_preset);
    /* Found here comes first in Tab order: it's what is actually on the air where you are */
    snprintf(found_path, sizeof found_path, "%s", saved_path);
    strcpy(strrchr(found_path, '\\') + 1, "found.txt");
    fp = fopen(found_path, "r");
    if (fp) {
        while (fgets(line, sizeof line, fp) && nfound < MAX_SAVED) {
            char *s = trim(line);
            if (!*s || *s == '#') continue;
            char *f[3] = {0}; int k = 0;
            for (char *t = strtok(s, "|"); t && k < 3; t = strtok(NULL, "|")) f[k++] = trim(t);
            if (k >= 2 && atof(f[1]) > 0) {
                preset_t *p = &found[nfound++];
                snprintf(p->name, sizeof p->name, "%s", f[0]);
                p->hz = atof(f[1]) * 1e6; p->mode = k == 3 ? parse_mode(f[2]) : M_NFM;
            }
        }
        fclose(fp);
    }
    qsort(found, nfound, sizeof(preset_t), cmp_preset);
    if (ncats < MAX_CATS) {
        found_cat = ncats;
        cat_t *c = &cats[ncats++];
        snprintf(c->name, sizeof c->name, "Found here");
        c->lo = F_MIN; c->hi = F_MAX; c->mode = M_NFM; c->count = nfound;
        c->last = nfound ? found[0].hz : 100e6; c->last_mode = nfound ? found[0].mode : M_NFM;
    }
    if (ncats < MAX_CATS) {
        saved_cat = ncats;
        cat_t *c = &cats[ncats++];
        snprintf(c->name, sizeof c->name, "Saved");
        c->lo = F_MIN; c->hi = F_MAX; c->mode = M_NFM; c->count = nsaved;
        c->last = nsaved ? saved[0].hz : 100e6; c->last_mode = nsaved ? saved[0].mode : M_NFM;
    }
}

static void load_presets(void)
{
    char path[MAX_PATH], line[512];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "presets.txt");
    FILE *fp = fopen(path, "r");
    if (!fp && slash) {                                       /* SDR\presets.txt */
        strcpy(slash + 1, "..\\presets.txt");
        fp = fopen(path, "r");
        if (!fp) strcpy(slash + 1, "presets.txt");            /* none: keep saved.txt beside the exe */
    }
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            char *s = trim(line);
            if (!*s || *s == '#') continue;
            if (*s == '[') {                         /* [Name | low MHz | high MHz | mode] */
                char *e = strchr(s, ']'); if (e) *e = 0;
                char *f[4] = {0}; int k = 0;
                for (char *t = strtok(s + 1, "|"); t && k < 4; t = strtok(NULL, "|")) f[k++] = trim(t);
                if (k >= 3) {
                    double lo = atof(f[1]) * 1e6, hi = atof(f[2]) * 1e6;
                    if (lo < F_MIN) lo = F_MIN; if (hi > F_MAX) hi = F_MAX;
                    if (hi > lo) add_cat(f[0], lo, hi, k == 4 ? parse_mode(f[3]) : M_NFM);
                }
                continue;
            }
            char *f[3] = {0}; int k = 0;                /* Name | MHz | mode */
            for (char *t = strtok(s, "|"); t && k < 3; t = strtok(NULL, "|")) f[k++] = trim(t);
            if (k >= 2 && atof(f[1]) > 0)
                add_preset(f[0], atof(f[1]) * 1e6, k == 3 ? parse_mode(f[2]) : (ncats ? cats[ncats - 1].mode : M_NFM));
        }
        fclose(fp);
    }
    if (!ncats) builtin_presets();                    /* no presets.txt: standard national frequencies */
    for (int c = 0; c < ncats; c++) {
        qsort(&presets[cats[c].first], cats[c].count, sizeof(preset_t), cmp_preset);
        /* widen the glide range if a preset lies outside it */
        for (int i = 0; i < cats[c].count; i++) {
            double hz = presets[cats[c].first + i].hz;
            if (hz < cats[c].lo) cats[c].lo = hz;
            if (hz > cats[c].hi) cats[c].hi = hz;
        }
        cats[c].last = cats[c].count ? presets[cats[c].first].hz : cats[c].lo;
        cats[c].last_mode = cats[c].count ? presets[cats[c].first].mode : cats[c].mode;
    }
    load_saved(path);
}

/* ---------- shared state ---------- */
static volatile double g_station = 93.5e6;
static volatile int g_dir, g_fine, g_at_edge;
static volatile LONG g_edge_ticks;          /* band edges crossed while gliding (UI ticks for each) */
static volatile LONGLONG g_hold_start;
static volatile float g_volume = 0.30f;
static volatile LONG g_stop;
static double g_qpf;
static HWND g_wnd;
static rtlsdr_dev_t *volatile dev;

#define CAT_LO (cats[g_cat].lo)
#define CAT_HI (cats[g_cat].hi)

static LONGLONG now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* per-mode tuning feel: tap step, fine step, starting glide speed, top glide speed (Hz, Hz/s) */
static void mode_params(int mode, double f, double *tap, double *fine, double *glide, double *gmax)
{
    switch (mode) {
    /* glide = the constant hold speed in Hz/s: about one channel per second, like a dial */
    case M_WFM: *tap = 10000; *fine = 1000; *glide = 200000; *gmax = *glide; break;
    case M_USB: case M_LSB: case M_CW: *tap = 100; *fine = 10; *glide = 1000; *gmax = *glide; break;
    default:
        *tap = 1000; *fine = 100;
        *glide = f < 30e6 ? 10000 : 20000; *gmax = *glide;
    }
}

static int gain_for(int mode, double f) { return f < 30e6 ? 400 : mode == M_WFM ? 166 : 340; }

/* automatic gain: the tuner's gain steps, and which one is in use. The processing thread
   watches the 8-bit input level and moves one step at a time (see agc_check). */
static int gains[64], ngains;
static volatile int g_gain_idx = -1;
static int nearest_gain(int tenths)
{
    int best = 0;
    for (int i = 1; i < ngains; i++) if (abs(gains[i] - tenths) < abs(gains[best] - tenths)) best = i;
    return best;
}

/* ---------- speech ---------- */
typedef unsigned long (__stdcall *speak_fn)(const wchar_t *);
typedef unsigned long (__stdcall *cancel_fn)(void);
static speak_fn nvda_speak;
static cancel_fn nvda_cancel;
static void say(const char *text)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    if (nvda_cancel) nvda_cancel();
    if (nvda_speak) nvda_speak(w);
}

/* below 30 MHz in kilohertz (up to 2 decimals), above in megahertz (up to 5 decimals), trimmed */
static void freq_text(char *out, size_t n, double hz)
{
    char s[32];
    if (hz < 30e6) snprintf(s, sizeof s, "%.2f", floor(hz / 10 + 0.5) / 100);
    else snprintf(s, sizeof s, "%.5f", floor(hz / 10 + 0.5) / 1e5);
    size_t L = strlen(s);
    while (L > 0 && s[L - 1] == '0') s[--L] = 0;
    if (L > 0 && s[L - 1] == '.') s[--L] = 0;
    snprintf(out, n, "%s", s);
}

/* ---------- beeps (own thread so nothing blocks) ---------- */
/* ---------- sound effects: small piano phrases, synthesised ----------
   Each note: a soft hammer attack, inharmonic partials (piano strings run slightly sharp:
   f_n = n f sqrt(1 + B n^2)), upper partials dying faster than the fundamental, a gentle pan.
   A phrase is rendered into one buffer and played on its own waveOut handle so Windows mixes it
   with the radio. Loudness follows the radio volume. */
#define SFX_RATE 48000
static volatile float g_volume;                         /* (defined with the shared state) */

/* three instrument voices; g_sfx_style picks one (0 electric piano, 1 marimba, 2 soft chime) */
static volatile int g_sfx_style = 2;                    /* Conner chose the soft chime (2026-10-03) */

static void piano_note(float *lr, int len, double t0, double freq, double amp, double pan, double length)
{
    int start = (int)(t0 * SFX_RATE), n_s = (int)(length * SFX_RATE);
    double gl = amp * sqrt(1 - pan), gr = amp * sqrt(pan);
    double w = 2 * PI * freq / SFX_RATE;
    int style = g_sfx_style;
    for (int i = 0; i < n_s && start + i < len; i++) {
        double t = (double)i / SFX_RATE, s;
        if (style == 0) {
            /* electric piano (Rhodes-like FM): a sine whose brightness (modulation index) fades fast,
               leaving a round bell tone, plus a faint short "tine" overtone */
            double index = 1.4 * exp(-t / 0.35);
            s = sin(w * i + index * sin(w * i)) * exp(-t / (1.3 * pow(262.0 / freq, 0.3)));
            s += 0.06 * sin(w * 7.0 * i) * exp(-t / 0.04);
            s *= 1 - exp(-t / 0.004);
        } else if (style == 1) {
            /* marimba: fundamental plus the bar's 4x partial, which dies away quickly */
            s = (sin(w * i) * exp(-t / 0.45) + 0.35 * sin(w * 3.93 * i) * exp(-t / 0.08)) * (1 - exp(-t / 0.002));
        } else {
            /* soft chime: bell partials (1, 2.76, 5.4) with the upper ones fading first */
            s = (sin(w * i) * exp(-t / 1.6) + 0.25 * sin(w * 2.76 * i) * exp(-t / 0.6)
                 + 0.08 * sin(w * 5.4 * i) * exp(-t / 0.25)) * (1 - exp(-t / 0.006));
        }
        if (i > n_s - 4800) s *= (n_s - i) / 4800.0;          /* 100 ms release: no clicks */
        lr[2 * (start + i)] += (float)(s * gl);
        lr[2 * (start + i) + 1] += (float)(s * gr);
    }
}

/* a small, soft room (Schroeder): per side, four damped feedback combs in parallel, then two
   all-pass diffusers; left and right use different delays so it sounds wide */
static void sfx_reverb(float *lr, int len)
{
    static const int comb_l[4] = { 1557, 1617, 1491, 1422 }, comb_r[4] = { 1580, 1640, 1514, 1445 };
    static const int ap[2] = { 556, 441 };
    float *x = malloc((size_t)len * sizeof(float)), *y = malloc((size_t)len * sizeof(float));
    float *cb = malloc(1700 * sizeof(float)), *ab = malloc(600 * sizeof(float));
    if (!x || !y || !cb || !ab) { free(x); free(y); free(cb); free(ab); return; }
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < len; i++) { x[i] = lr[2 * i + ch]; y[i] = 0; }
        for (int c = 0; c < 4; c++) {                          /* combs: y += comb(x) */
            int d = (ch ? comb_r : comb_l)[c], p = 0; float damp = 0;
            memset(cb, 0, 1700 * sizeof(float));
            for (int i = 0; i < len; i++) {
                float out = cb[p];
                damp = out * 0.6f + damp * 0.4f;              /* each repeat a little darker */
                cb[p] = x[i] + damp * 0.78f;
                p = (p + 1) % d;
                y[i] += out * 0.25f;
            }
        }
        for (int a = 0; a < 2; a++) {                          /* all-pass diffusion */
            int d = ap[a], p = 0;
            memset(ab, 0, 600 * sizeof(float));
            for (int i = 0; i < len; i++) {
                float bo = ab[p], in = y[i];
                y[i] = -in + bo;
                ab[p] = in + bo * 0.5f;
                p = (p + 1) % d;
            }
        }
        for (int i = 0; i < len; i++) lr[2 * i + ch] = x[i] * 0.8f + y[i] * 0.35f;
    }
    free(x); free(y); free(cb); free(ab);
}

static double note_hz(int midi) { return 440.0 * pow(2.0, (midi - 69) / 12.0); }

static DWORD WINAPI sfx_thread(LPVOID p)
{
    int kind = (int)(INT_PTR)p;
    /* phrases: (midi note, start seconds, loudness, pan 0..1, length seconds) */
    struct { int m; double t, a, pan, len; } ph[6]; int n = 0;
#define N(M, T, A, P, L) (ph[n].m = (M), ph[n].t = (T), ph[n].a = (A), ph[n].pan = (P), ph[n].len = (L), n++)
    switch (kind) {
    case 0: N(55, 0, .45, .45, .8); break;                                            /* edge: soft G3 */
    case 1: N(60, 0, .4, .35, 1.6); N(64, .12, .38, .45, 1.5); N(67, .24, .38, .55, 1.4); N(72, .36, .42, .65, 1.8); break; /* HD: C-E-G-C */
    case 2: N(64, 0, .4, .55, .9); N(60, .18, .4, .45, 1.4); break;                   /* no HD: E falling to C */
    case 3: N(55, 0, .42, .4, 1.2); N(62, .16, .42, .6, 1.5); break;                  /* connected: G up to D */
    case 4: N(62, 0, .42, .6, 1.0); N(55, .2, .42, .4, 1.5); break;                   /* lost: D down to G */
    case 5: N(67, 0, .3, .5, 1.0); break;                                             /* starting: soft G4 */
    case 6: N(76, 0, .25, .45, .6); N(79, .09, .25, .55, .8); break;                  /* decoded: light E-G */
    case 7: N(84, 0, .12, .5, .25); break;                                            /* band-edge tick: tiny C6 */
    default: return 0;
    }
#undef N
    double total = 0;
    for (int i = 0; i < n; i++) if (ph[i].t + ph[i].len > total) total = ph[i].t + ph[i].len;
    int len = (int)((total + 0.6) * SFX_RATE) + 1;          /* room for the reverb tail */
    float *lr = calloc((size_t)len * 2, sizeof(float));
    short *pcm = malloc((size_t)len * 4);
    if (!lr || !pcm) { free(lr); free(pcm); return 0; }
    for (int i = 0; i < n; i++) piano_note(lr, len, ph[i].t, note_hz(ph[i].m), ph[i].a, ph[i].pan, ph[i].len);
    sfx_reverb(lr, len);
    float g = 6000.f * (0.25f + 0.75f * g_volume);       /* follows the radio volume, never silent */
    for (int i = 0; i < 2 * len; i++) { float v = lr[i] * g; pcm[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    free(lr);
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = SFX_RATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = SFX_RATE * 4;
    HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
    HWAVEOUT wo;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)done, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
        WAVEHDR h = {0};
        h.lpData = (LPSTR)pcm; h.dwBufferLength = (DWORD)len * 4;
        waveOutPrepareHeader(wo, &h, sizeof h);
        waveOutWrite(wo, &h, sizeof h);
        while (!(h.dwFlags & WHDR_DONE)) WaitForSingleObject(done, 100);
        waveOutUnprepareHeader(wo, &h, sizeof h);
        waveOutClose(wo);
    }
    CloseHandle(done);
    free(pcm);
    return 0;
}
static void beep(int kind) { CloseHandle(CreateThread(NULL, 0, sfx_thread, (LPVOID)(INT_PTR)kind, 0, NULL)); }

/* ---------- audio out (wide FM, HD, narrow modes each get a device) ---------- */
typedef struct {
    HWAVEOUT wo; WAVEHDR hdr[NBUF]; int frames, fill_idx, fill_frames, queued, started;
    int max_queue, start_after;     /* latency cap: buffers beyond this are dropped */
    int underruns, drops;           /* counted for tests */
} out_t;
static volatile LONG g_retunes;
static out_t ana, hda, nar;

static int busy(WAVEHDR *h) { return (h->dwFlags & WHDR_PREPARED) && !(h->dwFlags & WHDR_DONE); }

static int out_open(out_t *o, int rate, int frames, int max_queue, int start_after)
{
    o->max_queue = max_queue; o->start_after = start_after;
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = rate * 4;
    if (waveOutOpen(&o->wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return 0;
    waveOutPause(o->wo);
    o->frames = frames;
    for (int i = 0; i < NBUF; i++) { o->hdr[i].lpData = calloc(frames, 4); o->hdr[i].dwFlags = WHDR_DONE; }
    return 1;
}

static void out_frame(out_t *o, short l, short r)
{
    WAVEHDR *h = &o->hdr[o->fill_idx];
    if (o->fill_frames == 0 && busy(h)) return;            /* output is behind: drop */
    short *p = (short *)h->lpData + 2 * o->fill_frames;
    p[0] = l; p[1] = r;
    if (++o->fill_frames < o->frames) return;
    int pending = 0;
    for (int i = 0; i < NBUF; i++) pending += busy(&o->hdr[i]);
    if (o->started && pending == 0) { waveOutPause(o->wo); o->started = 0; o->queued = 0; o->underruns++; }
    if (o->started && pending >= o->max_queue) { o->fill_frames = 0; o->drops++; return; }   /* keep latency low */
    if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(o->wo, h, sizeof *h);
    h->dwBufferLength = o->frames * 4; h->dwFlags = 0;
    waveOutPrepareHeader(o->wo, h, sizeof *h);
    waveOutWrite(o->wo, h, sizeof *h);
    if (!o->started && ++o->queued >= o->start_after) { waveOutRestart(o->wo); o->started = 1; }
    o->fill_idx = (o->fill_idx + 1) % NBUF;
    o->fill_frames = 0;
}

static short clip16(float v) { return (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* ---------- HD (libnrsc5, loaded at run time) ---------- */
typedef int  (*p_open_pipe)(nrsc5_t **);
typedef void (*p_close)(nrsc5_t *);
typedef void (*p_set_cb)(nrsc5_t *, nrsc5_callback_t, void *);
typedef int  (*p_pipe_cf32)(nrsc5_t *, const float *, unsigned int);
static p_open_pipe hd_open_pipe; static p_close hd_close; static p_set_cb hd_set_cb; static p_pipe_cf32 hd_pipe;

enum { HD_OFF, HD_SEARCHING, HD_PLAYING };
static volatile LONG g_hd_state;
static volatile LONG g_hd_audio;          /* HD audio is flowing: analog is muted */
static volatile LONGLONG g_hd_started;
static char g_hd_name[64], g_hd_title[4][128], g_hd_artist[4][128];
static volatile LONG g_hd_program;        /* 0 = HD1 ... 3 = HD4 */
static volatile LONG g_hd_seen;           /* bit per program that has delivered audio */
static HANDLE hd_event;
static float *hd_ring;
static volatile LONG hd_w, hd_r;
static float hb_h[HB_TAPS], hb_i[2 * HB_TAPS], hb_q[2 * HB_TAPS]; static int hb_pos, hb_phase;

static void hd_callback(const nrsc5_event_t *e, void *ctx)
{
    (void)ctx;
    char t[300];
    switch (e->event) {
    case NRSC5_EVENT_LOST_SYNC:
        if (g_hd_audio) say("HD signal lost");
        g_hd_audio = 0;
        break;
    case NRSC5_EVENT_STATION_NAME:
        if (e->station_name.name) snprintf(g_hd_name, sizeof g_hd_name, "%s", e->station_name.name);
        break;
    case NRSC5_EVENT_ID3:
        if (e->id3.program < 4) {
            if (e->id3.title) snprintf(g_hd_title[e->id3.program], 128, "%s", e->id3.title);
            if (e->id3.artist) snprintf(g_hd_artist[e->id3.program], 128, "%s", e->id3.artist);
        }
        break;
    case NRSC5_EVENT_AUDIO:
        if (e->audio.program < 4) InterlockedOr(&g_hd_seen, 1 << e->audio.program);
        if (e->audio.program != (unsigned)g_hd_program || g_hd_state == HD_OFF) break;
        if (!g_hd_audio) {
            g_hd_audio = 1;
            if (g_hd_state != HD_PLAYING) {
                g_hd_state = HD_PLAYING;
                beep(1);
                snprintf(t, sizeof t, "HD%s%s", g_hd_name[0] ? ", " : "", g_hd_name);
                say(t);
            }
        }
        {
            float v = 18500.f / 32768.f * g_volume;
            for (size_t k = 0; k + 1 < e->audio.count; k += 2)
                out_frame(&hda, clip16(e->audio.data[k] * v), clip16(e->audio.data[k + 1] * v));
        }
        break;
    }
}

static DWORD WINAPI hd_thread(LPVOID p)
{
    (void)p;
    nrsc5_t *radio = NULL;
    static float chunk[16384];
    while (!g_stop) {
        if (g_hd_state == HD_OFF) {
            if (radio) { hd_close(radio); radio = NULL; }
            WaitForSingleObject(hd_event, 100);
            continue;
        }
        if (!radio) {
            if (!hd_open_pipe || hd_open_pipe(&radio) != 0) {
                radio = NULL; g_hd_state = HD_OFF; say("HD decoder is not available"); continue;
            }
            hd_set_cb(radio, hd_callback, NULL);
            hd_r = hd_w;                                   /* start from fresh samples */
        }
        LONG avail = (hd_w - hd_r) & (HD_RING - 1);
        if (avail < 4096) WaitForSingleObject(hd_event, 20);
        else {
            LONG n = (avail > 16384 ? 16384 : avail) & ~1;
            for (LONG k = 0; k < n; k++) chunk[k] = hd_ring[(hd_r + k) & (HD_RING - 1)];
            hd_r = (hd_r + n) & (HD_RING - 1);
            hd_pipe(radio, chunk, (unsigned int)n);
        }
        if (g_hd_state == HD_SEARCHING && (now() - g_hd_started) / g_qpf > 8.0) {
            g_hd_state = HD_OFF;
            beep(2);
            say("No HD on this station");
        }
    }
    if (radio) hd_close(radio);
    return 0;
}

/* ---------- wide FM DSP ---------- */
static float ch_h[CH_TAPS], au_h[AU_TAPS];
static float ci[2 * CH_TAPS], cq[2 * CH_TAPS]; static int cpos, cphase;
static float ml[2 * AU_TAPS], sl[2 * AU_TAPS]; static int apos, aphase;
static float prev_i = 1, prev_q = 0;
static double pll_theta, pll_omega, lock;
static float de_l, de_r, de_a, dc_xl, dc_yl, dc_xr, dc_yr;
static int stereo_on;
static double rot_i = 1, rot_q = 0, cur_off;
static float fade = 1, fade_target = 1;
static double lo_used;                     /* LO the incoming samples were captured with */
static volatile LONGLONG g_samples, g_switch_at;
static volatile double g_lo_next;
static volatile LONG g_retune_pending;
static HANDLE retune_event;

static void design_lowpass(float *h, int n, double cutoff, double fs)
{
    double fc = cutoff / fs, sum = 0;
    for (int i = 0; i < n; i++) {
        double m = i - (n - 1) / 2.0;
        double s = m == 0 ? 2 * fc : sin(2 * PI * fc * m) / (PI * m);
        h[i] = (float)(s * (0.42 - 0.5 * cos(2 * PI * i / (n - 1)) + 0.08 * cos(4 * PI * i / (n - 1))));
        sum += h[i];
    }
    for (int i = 0; i < n; i++) h[i] = (float)(h[i] / sum);
}

/* soft band-limited noise, like the static between stations on an analog dial */
static float dial_static(void)
{
    static unsigned int seed = 2463534242u; static float lp;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    float white = (int)(seed >> 8) / 8388608.0f - 1.0f;
    lp += 0.35f * (white - lp);                              /* take the harsh top off */
    return lp;
}

/* squelch: 0 off, 1 low, 2 medium, 3 high. sq_open is decided from the spectrum (see
   squelch_check); sq_gate follows it smoothly so opening and closing don't click. */
static volatile int g_squelch;
static const char *squelch_names[4] = { "Squelch off", "Squelch low", "Squelch medium", "Squelch high" };
static volatile int sq_open = 1;
static float sq_gate = 1;
static float gate_step(void) { sq_gate += ((sq_open ? 1.f : 0.f) - sq_gate) * 0.003f; return sq_gate; }

/* ---------- RDS: station name and RadioText from the 57 kHz subcarrier ----------
   57 kHz = 3 x the pilot, so the pilot PLL gives the carrier. Baseband is low-passed and
   decimated by 13 (19082 Hz), resampled to 19000 Hz = 16 samples per bit, matched-filtered
   for the biphase symbol, sampled at the best of 16 timing phases, differentially decoded,
   then 26-bit blocks are found by their checkword (remainder mod g(x) = offset word). */
#define RDS_TAPS 255
static float rds_h[RDS_TAPS], rds_i[2 * RDS_TAPS], rds_q[2 * RDS_TAPS]; static int rds_pos, rds_ph;
static float rds_hist[16]; static int rds_hn;          /* last 16 samples at 19000 Hz */
static double rds_mu, rds_prev, rds_cur;              /* resampler 19082 -> 19000 */
static float rds_energy[16]; static int rds_n16;
static double rds_pr, rds_pi;                         /* averaged (I+jQ)^2: carrier phase */
static int rds_last_sym;
static uint32_t rds_reg; static int rds_bits, rds_synced, rds_block, rds_errs;
static uint16_t rds_blk[4];
static char rds_ps[9], rds_ps_shown[9], rds_rt[65];
static int rds_ps_mask, rds_rt_mask, rds_rt_ab = -1;
static volatile char g_rds_ps[9], g_rds_rt[65];
static volatile int g_rds_announce;                   /* announce the name once after tuning */

static void rds_reset(void)
{
    rds_synced = 0; rds_bits = 0; rds_ps_mask = rds_rt_mask = 0; rds_rt_ab = -1;
    memset(rds_ps, ' ', 8); rds_ps[8] = 0; memset(rds_rt, ' ', 64); rds_rt[64] = 0;
    ((char *)g_rds_ps)[0] = 0; ((char *)g_rds_rt)[0] = 0; rds_ps_shown[0] = 0;
    g_rds_announce = 1;
}

static uint32_t rds_rem(uint32_t x)                   /* 26-bit block mod g(x) = x^10+x^8+x^7+x^5+x^4+x^3+1 */
{
    uint32_t reg = 0;
    for (int i = 25; i >= 0; i--) { reg = (reg << 1) | ((x >> i) & 1); if (reg & 0x400) reg ^= 0x5B9; }
    return reg & 0x3FF;
}

static char rds_char(int c) { return c >= 0x20 && c < 0x7F ? (char)c : ' '; }

static int rds_ok[4];                                 /* which blocks of this group passed their check */

static void rds_group(void)
{
    uint16_t b = rds_blk[1], c = rds_blk[2], d = rds_blk[3];
    int type = b >> 12, ver_b = (b >> 11) & 1;
    if (!rds_ok[1] || !rds_ok[3]) return;             /* never use a stale or corrupt block */
    if (type == 2 && !ver_b && !rds_ok[2]) return;
    if (type == 0) {                                  /* 0A/0B: programme service name */
        int seg = b & 3;
        rds_ps[seg * 2] = rds_char(d >> 8); rds_ps[seg * 2 + 1] = rds_char(d & 0xFF);
        rds_ps_mask |= 1 << seg;
        if (rds_ps_mask == 15) {
            char t[9]; const char *s = rds_ps; while (*s == ' ') s++;
            snprintf(t, sizeof t, "%s", s);
            for (int i = (int)strlen(t) - 1; i >= 0 && t[i] == ' '; i--) t[i] = 0;
            snprintf((char *)g_rds_ps, 9, "%s", t);
            rds_ps_mask = 0;
            if (g_rds_announce && t[0] && !strcmp(t, rds_ps_shown)) {   /* same name twice: trust it */
                g_rds_announce = 0;
                PostMessageW(g_wnd, WM_APP + 4, 0, 0);
            }
            snprintf(rds_ps_shown, sizeof rds_ps_shown, "%s", t);
        }
    } else if (type == 2) {                           /* 2A/2B: RadioText */
        int seg = b & 15, ab = (b >> 4) & 1;
        static int rt_last = 15;                      /* segment holding the carriage return */
        if (ab != rds_rt_ab) { memset(rds_rt, ' ', 64); rds_rt_mask = 0; rds_rt_ab = ab; rt_last = 15; }
        int raw[4] = { c >> 8, c & 0xFF, d >> 8, d & 0xFF }, n = ver_b ? 2 : 4, base = ver_b ? seg * 2 : seg * 4;
        for (int k = 0; k < n; k++) {
            int ch = ver_b ? raw[k + 2] : raw[k];
            if (ch == 0x0D) { rt_last = seg; rds_rt[base + k] = '\r'; }
            else rds_rt[base + k] = rds_char(ch);
        }
        rds_rt_mask |= 1 << seg;
        /* complete when every segment up to the carriage return (or all 16) has arrived */
        int need = (1 << (rt_last + 1)) - 1;
        if ((rds_rt_mask & need) == need || rds_rt_mask == 0xFFFF) {
            char t[65]; const char *s = rds_rt; while (*s == ' ') s++;
            snprintf(t, sizeof t, "%s", s);
            char *cr = strchr(t, '\r'); if (cr) *cr = 0;
            for (int i = (int)strlen(t) - 1; i >= 0 && t[i] == ' '; i--) t[i] = 0;
            if (t[0]) snprintf((char *)g_rds_rt, 65, "%s", t);
        }
    }
}

static void rds_bit(int bit)
{
    static const uint16_t offs[5] = { 0x0FC, 0x198, 0x168, 0x1B4, 0x350 };   /* A B C D C' */
    rds_reg = ((rds_reg << 1) | bit) & 0x3FFFFFF;
    if (!rds_synced) {
        if (++rds_bits < 26) return;
        if (rds_rem(rds_reg) == offs[0]) { rds_synced = 1; rds_block = 1; rds_bits = 0; rds_errs = 0; rds_blk[0] = rds_reg >> 10; }
        return;
    }
    if (++rds_bits < 26) return;
    rds_bits = 0;
    uint32_t r = rds_rem(rds_reg);
    int ok = r == offs[rds_block] || (rds_block == 2 && r == offs[4]);
    if (rds_block == 0) memset(rds_ok, 0, sizeof rds_ok);
    rds_ok[rds_block] = ok;
    if (ok) { rds_blk[rds_block] = rds_reg >> 10; if (rds_errs) rds_errs--; }
    else if (++rds_errs > 20) { rds_synced = 0; rds_bits = 0; return; }
    if (rds_block == 3) { rds_group(); rds_block = 0; }
    else rds_block++;
}

/* one MPX sample; c, s = cos and sin of the pilot PLL's phase (triple angle gives 57 kHz) */
static void rds_sample(float mpx, double c, double s)
{
    double c3 = 4 * c * c * c - 3 * c, s3 = 3 * s - 4 * s * s * s;
    rds_i[rds_pos] = rds_i[rds_pos + RDS_TAPS] = (float)(mpx * c3);
    rds_q[rds_pos] = rds_q[rds_pos + RDS_TAPS] = (float)(mpx * s3);
    rds_pos = (rds_pos + 1) % RDS_TAPS;
    if (++rds_ph < 13) return;
    rds_ph = 0;
    float a = 0, b = 0;
    const float *pi = rds_i + rds_pos, *pq = rds_q + rds_pos;
    for (int k = 0; k < RDS_TAPS; k++) { a += rds_h[k] * pi[k]; b += rds_h[k] * pq[k]; }
    /* carrier phase from the BPSK-squared average, then rotate onto the real axis */
    rds_pr = 0.999 * rds_pr + 0.001 * (a * a - b * b);
    rds_pi = 0.999 * rds_pi + 0.001 * (2 * a * b);
    double ph = atan2(rds_pi, rds_pr) / 2;
    double x = a * cos(ph) + b * sin(ph);
    /* resample 19082 -> 19000 Hz by linear interpolation */
    rds_prev = rds_cur; rds_cur = x;
    const double step = (FS_MPX / 13.0) / 19000.0;     /* output samples are this far apart in input samples */
    while (rds_mu <= 1.0) {
        float y = (float)(rds_prev + (rds_cur - rds_prev) * rds_mu);
        rds_mu += step;
        rds_hist[rds_hn] = y; rds_hn = (rds_hn + 1) & 15;
        /* biphase matched filter over the last bit: first half minus second half */
        float m = 0;
        for (int k = 0; k < 8; k++) m += rds_hist[(rds_hn + k) & 15];
        for (int k = 8; k < 16; k++) m -= rds_hist[(rds_hn + k) & 15];
        int phase = rds_n16;
        rds_energy[phase] = rds_energy[phase] * 0.995f + m * m;
        int best = 0;
        for (int k = 1; k < 16; k++) if (rds_energy[k] > rds_energy[best]) best = k;
        if (phase == best) {
            int sym = m > 0;
            rds_bit(sym ^ rds_last_sym);
            rds_last_sym = sym;
        }
        rds_n16 = (rds_n16 + 1) & 15;
    }
    rds_mu -= 1.0;
}

static void mpx_sample(float mpx)
{
    double s = sin(pll_theta), c = cos(pll_theta), err = mpx * -s;
    rds_sample(mpx, c, s);
    pll_omega += 1.2e-5 * err;
    double nom = 2 * PI * 19000.0 / FS_MPX, lim = 2 * PI * 20.0 / FS_MPX;
    if (pll_omega > lim) pll_omega = lim; else if (pll_omega < -lim) pll_omega = -lim;
    pll_theta += nom + pll_omega + 0.03 * err;
    if (pll_theta > 2 * PI) pll_theta -= 2 * PI;
    lock = 0.9995 * lock + 0.0005 * (mpx * c);
    float diff = (float)(mpx * 2 * cos(2 * pll_theta));
    ml[apos] = ml[apos + AU_TAPS] = mpx;
    sl[apos] = sl[apos + AU_TAPS] = diff;
    apos = (apos + 1) % AU_TAPS;
    if (++aphase < D2) return;
    aphase = 0;
    float m = 0, d = 0;
    const float *pm = ml + apos, *pd = sl + apos;
    for (int k = 0; k < AU_TAPS; k++) { m += au_h[k] * pm[k]; d += au_h[k] * pd[k]; }
    stereo_on = g_dir ? 0 : stereo_on ? lock > 0.012 : lock > 0.022;   /* mono while gliding: no stereo pops */
    float l = stereo_on ? m + d : m, r = stereo_on ? m - d : m;
    de_l = de_a * de_l + (1 - de_a) * l;
    de_r = de_a * de_r + (1 - de_a) * r;
    const float R = 0.9987f;
    dc_yl = de_l - dc_xl + R * dc_yl; dc_xl = de_l;
    dc_yr = de_r - dc_xr + R * dc_yr; dc_xr = de_r;
    fade += (fade_target - fade) * 0.02f;                   /* ~1 ms ramps around retunes */
    /* while the hardware moves, fill with dial static instead of silence, so it sounds analog */
    float hiss = (1 - fade) * dial_static() * 0.25f;
    float v = 18500.f * g_volume * gate_step() * (g_hd_audio ? 0.f : 1.f);
    out_frame(&ana, clip16((dc_yl * fade + hiss) * v), clip16((dc_yr * fade + hiss) * v));
}

/* ---------- narrow modes at 48 kHz: AM, narrow FM, USB, LSB, CW ---------- */
static float nd_h[ND_TAPS], nd_i[2 * ND_TAPS], nd_q[2 * ND_TAPS]; static int nd_pos, nd_phase;
static float am_h[NCH_TAPS], nfm_h[NCH_TAPS], nc_i[2 * NCH_TAPS], nc_q[2 * NCH_TAPS]; static int nc_pos;
static float ssb_h[SSB_TAPS], cw_h[SSB_TAPS], sc_i[2 * SSB_TAPS], sc_q[2 * SSB_TAPS]; static int sc_pos;
static float n_prev_i = 1, n_prev_q = 0, am_avg = 1e-3f, n_lp, n_dcx, n_dcy, agc_pk = 1e-3f;
static double sh_i = 1, sh_q = 0, bk_i = 1, bk_q = 0;  /* sideband shift rotators */
static int sh_n;

static void mm_feed(float x, int mode);
static void ds_feed(float d, int mode);
static volatile LONGLONG g_digital_until;
static void acars_feed(float x);
static int is_acars(double f);
static void ft8_feed(float x);
static int is_ft8(double f);
static void acars_stop_if_running(void);
static volatile int g_decode_on, g_decode_speak;   /* defined with the decoders below */
static int g_listen_secs;
static char dec_log_path[MAX_PATH];
static volatile int g_trunk;
static volatile LONGLONG g_voice_until;

static void narrow_sample(float i, float q, int mode)
{
    float feed;                                           /* what the decoders hear */
    nd_i[nd_pos] = nd_i[nd_pos + ND_TAPS] = i;
    nd_q[nd_pos] = nd_q[nd_pos + ND_TAPS] = q;
    nd_pos = (nd_pos + 1) % ND_TAPS;
    if (++nd_phase < D_N) return;
    nd_phase = 0;
    float yi = 0, yq = 0;
    const float *pi = nd_i + nd_pos, *pq = nd_q + nd_pos;
    for (int k = 0; k < ND_TAPS; k++) { yi += nd_h[k] * pi[k]; yq += nd_h[k] * pq[k]; }
    float out;
    if (mode == M_AM || mode == M_NFM) {
        nc_i[nc_pos] = nc_i[nc_pos + NCH_TAPS] = yi;
        nc_q[nc_pos] = nc_q[nc_pos + NCH_TAPS] = yq;
        nc_pos = (nc_pos + 1) % NCH_TAPS;
        const float *h = mode == M_AM ? am_h : nfm_h, *ci_ = nc_i + nc_pos, *cq_ = nc_q + nc_pos;
        float a = 0, b = 0;
        for (int k = 0; k < NCH_TAPS; k++) { a += h[k] * ci_[k]; b += h[k] * cq_[k]; }
        if (mode == M_AM) {
            /* envelope, normalised by the carrier level so every station is equally loud */
            float env = sqrtf(a * a + b * b);
            am_avg += 0.0008f * (env - am_avg);
            out = (env / (am_avg + 1e-7f) - 1.f) * 0.6f;
        } else {
            float re = a * n_prev_i + b * n_prev_q, im = b * n_prev_i - a * n_prev_q;
            n_prev_i = a; n_prev_q = b;
            float d = atan2f(im, re) * (float)(FS_N / (2 * PI * 5000.0)) * 0.6f;
            n_lp += 0.33f * (d - n_lp);                      /* ~3 kHz: tames the hiss */
            out = n_lp;
            feed = d;                                        /* data needs the full bandwidth */
        }
        if (mode == M_AM) {
            feed = out;
            if (g_decode_on && is_acars(g_station)) acars_feed(out); else acars_stop_if_running();
        }
    } else {
        /* sidebands: move the wanted side to 0 Hz, low-pass it, move it back, keep the real part.
           USB: +300..2700 Hz, shift -1500. LSB: -2700..-300, shift +1500. CW: 0 Hz, tone at 700 Hz. */
        double shift = mode == M_USB ? -1500 : mode == M_LSB ? 1500 : 0;
        double back = mode == M_CW ? 700 : -shift;
        double ws = 2 * PI * shift / FS_N, wb = 2 * PI * back / FS_N;
        float a = (float)(yi * sh_i - yq * sh_q), b = (float)(yi * sh_q + yq * sh_i);
        double t = sh_i * cos(ws) - sh_q * sin(ws); sh_q = sh_i * sin(ws) + sh_q * cos(ws); sh_i = t;
        sc_i[sc_pos] = sc_i[sc_pos + SSB_TAPS] = a;
        sc_q[sc_pos] = sc_q[sc_pos + SSB_TAPS] = b;
        sc_pos = (sc_pos + 1) % SSB_TAPS;
        const float *h = mode == M_CW ? cw_h : ssb_h, *ci_ = sc_i + sc_pos, *cq_ = sc_q + sc_pos;
        float fa = 0, fb = 0;
        for (int k = 0; k < SSB_TAPS; k++) { fa += h[k] * ci_[k]; fb += h[k] * cq_[k]; }
        out = (float)(fa * bk_i - fb * bk_q);
        t = bk_i * cos(wb) - bk_q * sin(wb); bk_q = bk_i * sin(wb) + bk_q * cos(wb); bk_i = t;
        if (++sh_n == 4096) {                                /* keep rotators on the unit circle */
            sh_n = 0;
            double m1 = sqrt(sh_i * sh_i + sh_q * sh_q), m2 = sqrt(bk_i * bk_i + bk_q * bk_q);
            sh_i /= m1; sh_q /= m1; bk_i /= m2; bk_q /= m2;
        }
        /* AGC: fast attack, slow release */
        float env = sqrtf(fa * fa + fb * fb);
        agc_pk = env > agc_pk ? env : agc_pk * 0.99997f + 1e-9f;
        out *= 0.3f / (agc_pk + 1e-6f);
        feed = out;
        if (g_decode_on && mode == M_USB && is_ft8(g_station)) ft8_feed(out);
    }
    mm_feed(feed, mode);
    ds_feed(feed, mode);
    if (g_voice_until && now() < g_voice_until) out = 0;      /* decoded voice is playing instead */
    if (g_decode_on && now() < g_digital_until) out = 0;      /* digital buzz: only decoded voice is heard (D for raw) */
    n_dcy = out - n_dcx + 0.9987f * n_dcy; n_dcx = out;      /* DC block */
    fade += (fade_target - fade) * 0.02f;
    float v = 18500.f * g_volume * gate_step();
    short s = clip16((n_dcy * fade + (1 - fade) * dial_static() * 0.2f) * v);   /* static, not silence, while retuning */
    out_frame(&nar, s, s);
}

/* ---------- spectrum: squelch decisions and the scanner ---------- */
#define FFT_N 4096
#define BIN_HZ ((double)FS_IN / FFT_N)
static float fft_win[FFT_N], tw_c[FFT_N / 2], tw_s[FFT_N / 2];

static void fft_init(void)
{
    for (int i = 0; i < FFT_N; i++)                     /* Blackman-Harris: low leakage */
        fft_win[i] = (float)(0.35875 - 0.48829 * cos(2 * PI * i / FFT_N) + 0.14128 * cos(4 * PI * i / FFT_N)
                             - 0.01168 * cos(6 * PI * i / FFT_N));
    for (int i = 0; i < FFT_N / 2; i++) { tw_c[i] = (float)cos(2 * PI * i / FFT_N); tw_s[i] = (float)-sin(2 * PI * i / FFT_N); }
}

static void fft(float *re, float *im)
{
    for (int i = 1, j = 0; i < FFT_N; i++) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = tw_c[k * step], wi = tw_s[k * step];
                float xr = re[i + k + len / 2], xi = im[i + k + len / 2];
                float tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                re[i + k + len / 2] = re[i + k] - tr; im[i + k + len / 2] = im[i + k] - ti;
                re[i + k] += tr; im[i + k] += ti;
            }
    }
}

/* adds the power spectrum of FFT_N cu8 samples to acc[], ordered from -fs/2 to +fs/2 around the LO */
static void spectrum_add(const unsigned char *buf, float *acc)
{
    static float re[FFT_N], im[FFT_N];
    for (int i = 0; i < FFT_N; i++) {
        re[i] = (buf[2 * i] - 127.5f) * fft_win[i];
        im[i] = (buf[2 * i + 1] - 127.5f) * fft_win[i];
    }
    fft(re, im);
    for (int i = 0; i < FFT_N; i++) {
        int k = (i + FFT_N / 2) % FFT_N;
        acc[k] += re[i] * re[i] + im[i] * im[i];
    }
}

static int cmp_float(const void *a, const void *b) { float d = *(const float *)a - *(const float *)b; return d < 0 ? -1 : d > 0; }

/* mean noise power per bin: the median of the usable bins (noise power is exponentially
   distributed, so mean = median / ln 2); signals barely move a median */
static float noise_floor(const float *p)
{
    static float tmp[FFT_N];
    int n = 0;
    for (int k = 0; k < FFT_N; k++) {
        double f = (k - FFT_N / 2) * BIN_HZ;
        if (fabs(f) > 2000 && fabs(f) < 650000) tmp[n++] = p[k];
    }
    qsort(tmp, n, sizeof(float), cmp_float);
    return tmp[n / 2] * 1.4427f;
}

/* the channel the current mode listens to, in Hz relative to the station */
static void channel_span(int mode, double *lo, double *hi)
{
    switch (mode) {
    case M_WFM: *lo = -100000; *hi = 100000; break;
    case M_AM:  *lo = -5000;   *hi = 5000;   break;
    case M_USB: *lo = 300;     *hi = 2700;   break;
    case M_LSB: *lo = -2700;   *hi = -300;   break;
    case M_CW:  *lo = -200;    *hi = 200;    break;
    default:    *lo = -6500;   *hi = 6500;
    }
}

/* squelch: channel power against the noise in the same bandwidth, with a half-second hang */
static void squelch_check(const unsigned char *buf, double station_off, int mode)
{
    static const float open_db[4] = { -99, 3, 6, 12 };
    static LONGLONG last_above;
    static float p[FFT_N];
    if ((g_squelch == 0 || g_trunk) && !g_listen_secs) { sq_open = 1; return; }   /* trunk following: squelch is always off */
    memset(p, 0, sizeof p);
    spectrum_add(buf, p);
    spectrum_add(buf + 2 * FFT_N, p);
    float floor_ = noise_floor(p);
    double lo, hi; channel_span(mode, &lo, &hi);
    int k1 = (int)floor((station_off + lo) / BIN_HZ) + FFT_N / 2, k2 = (int)ceil((station_off + hi) / BIN_HZ) + FFT_N / 2;
    if (k1 < 0) k1 = 0; if (k2 >= FFT_N) k2 = FFT_N - 1;
    double sum = 0; int nb = 0;
    for (int k = k1; k <= k2; k++) { sum += p[k]; nb++; }
    double snr = nb ? 10 * log10(sum / (floor_ * nb + 1e-9) + 1e-9) : -99;
    if (g_listen_secs) {                                   /* test runs: log the channel's level over time */
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s", dec_log_path);
        strcpy(strrchr(path, '\\') + 1, "level_test.log");
        FILE *fp = fopen(path, "a"); if (fp) { fprintf(fp, "%.2f %.1f\n", now() / g_qpf, snr); fclose(fp); }
    }
    if (g_squelch == 0 || g_trunk) { sq_open = 1; return; }
    if (snr > open_db[g_squelch]) { sq_open = 1; last_above = now(); }
    else if ((now() - last_above) / g_qpf > 0.5) sq_open = 0;
}

/* ---------- lockouts (L): frequencies the scanner skips, kept in lockouts.txt ---------- */
#define MAX_LOCK 2000
#define LOCK_HZ 50000
static double lockouts[MAX_LOCK];
static volatile int nlock;
static char lock_path[MAX_PATH];

static int is_locked_out(double f)
{
    for (int i = 0; i < nlock; i++) if (fabs(lockouts[i] - f) < LOCK_HZ) return 1;
    return 0;
}

static void write_lockouts(void)
{
    FILE *fp = fopen(lock_path, "w");
    if (!fp) return;
    fprintf(fp, "# Frequencies the scanner skips (MHz), added with L\n");
    for (int i = 0; i < nlock; i++) fprintf(fp, "%.6f\n", lockouts[i] / 1e6);
    fclose(fp);
}

static void load_lockouts(void)
{
    char line[128];
    snprintf(lock_path, sizeof lock_path, "%s", saved_path);
    char *slash = strrchr(lock_path, '\\');
    if (slash) strcpy(slash + 1, "lockouts.txt");
    FILE *fp = fopen(lock_path, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp) && nlock < MAX_LOCK)
        if (line[0] != '#' && atof(line) > 0) lockouts[nlock++] = atof(line) * 1e6;
    fclose(fp);
}

/* ---------- scanner ---------- */
static volatile int g_scan_req;            /* UI: 1 = scan, 0 = stop */
static volatile int g_explore_lap, g_explore_wrapped;     /* Explore: a single lap of the category */
static volatile double g_explore_origin;
static int scan_active, scan_measuring, scan_blocks, scan_wrapped, scan_confirming, scan_in_wide;
static double scan_lo, scan_from, scan_start, scan_candidate, scan_skip_hz;
static float scan_ref;
static volatile double g_dbg_avg, g_dbg_flat, g_dbg_w;     /* last stop's measurements, for --scantest */
static float scan_acc[FFT_N];
static volatile double g_found_hz;
static int tick_left; static unsigned int noise_seed = 12345;

/* the soft "searching" hiss with a tick on every step, so you can hear the scanner working */
static void scan_audio(int frames)
{
    float v = 18500.f * g_volume;
    for (int i = 0; i < frames; i++) {
        noise_seed = noise_seed * 1664525u + 1013904223u;
        float s = ((int)(noise_seed >> 9) / 4194304.f - 1.f) * 0.025f;
        if (tick_left > 0) { s += (tick_left & 8 ? 0.12f : -0.12f) * tick_left / 120.f; tick_left--; }
        short x = clip16(s * v);
        out_frame(&nar, x, x);
    }
}

static int guess_mode(double f, double width)
{
    if (f < 30e6) {
        if (width > 5000 || (f >= 0.53e6 && f <= 1.71e6)) return M_AM;
        return f < 10e6 ? M_LSB : M_USB;
    }
    if (width > 100000 && f >= 87.5e6 && f <= 108e6) return M_WFM;
    if ((f >= 108e6 && f < 137e6) || (f >= 225e6 && f < 400e6)) return M_AM;
    return M_NFM;
}

static double round_to(double f, double grid) { return floor(f / grid + 0.5) * grid; }

/* ---------- broadcast survey (Explore in FM or AM): signal strength at every channel ----------
   One pass of windows across the band; each grid channel keeps its best strength over the
   window's quiet level; afterwards local peaks above a threshold are the stations. */
#define SV_MAX 2400
static volatile int g_survey_req;
static int sv_active, sv_blocks, sv_n, sv_mode;
static double sv_ch[SV_MAX], sv_win, sv_half, sv_bleed;
static float sv_snr[SV_MAX];
static const char *sv_name[SV_MAX];                          /* official channel name, if from presets */
static volatile int sv_found_n;
static double sv_found[400];
static const char *sv_found_name[400];

/* the real channel map for the current category: FM broadcast (87.9-107.9 every 200 kHz),
   AM broadcast (540-1700 every 10 kHz), or the category's own official channels */
static int survey_build(void)
{
    const cat_t *c = &cats[g_cat];
    sv_n = 0; sv_mode = c->mode;
    if (c->mode == M_WFM) {
        for (double f = 87.9e6; f <= 107.95e6 && sv_n < SV_MAX; f += 200000) { sv_name[sv_n] = NULL; sv_ch[sv_n++] = f; }
        sv_half = 12000; sv_bleed = 210000;       /* right at the carrier: a neighbour's HD sideband barely reaches */
    } else if (c->mode == M_AM && c->hi <= 1.8e6) {
        for (double f = 540e3; f <= 1700e3 && sv_n < SV_MAX; f += 10000) { sv_name[sv_n] = NULL; sv_ch[sv_n++] = f; }
        sv_half = 3000; sv_bleed = 10500;
    } else if (c->count >= 2 && g_cat != found_cat && g_cat != saved_cat) {
        for (int i = 0; i < c->count && sv_n < SV_MAX; i++) {
            const preset_t *p = item(g_cat, i);
            if (sv_n && fabs(p->hz - sv_ch[sv_n - 1]) < 100) continue;   /* duplicates */
            sv_name[sv_n] = p->name; sv_ch[sv_n++] = p->hz;
        }
        sv_half = c->mode == M_AM ? 4000 : 5000; sv_bleed = 13000;     /* bleed: a stronger channel within ~2 channels */
    } else return 0;                                                   /* no channel map: use the signal scanner */
    for (int i = 0; i < sv_n; i++) sv_snr[i] = -99;
    return 1;
}

static int survey_step(unsigned char *buf, uint32_t len)
{
    if (!sv_active) {
        if (!g_survey_req) return 0;
        sv_active = 1; sv_blocks = 0;
        sv_win = sv_ch[0] + 50000;                             /* window centre; usable +-550 kHz */
        if (ngains) { g_gain_idx = nearest_gain(sv_mode == M_WFM ? 166 : sv_ch[0] < 30e6 ? 400 : 340); SetEvent(retune_event); }
        fade_target = 0;
    }
    if (!g_survey_req) { sv_active = 0; fade_target = 1; return 0; }
    scan_audio((int)(len / 2 / D_N));
    if (g_retune_pending) return 1;
    if (lo_used != sv_win) {
        g_lo_next = sv_win; InterlockedExchange(&g_retune_pending, 1); SetEvent(retune_event);
        tick_left = 120; sv_blocks = 0; memset(scan_acc, 0, sizeof scan_acc);
        return 1;
    }
    /* skip two blocks after each retune: data still in flight was captured at the old position,
       and would put a strong station's energy on the wrong channel */
    if (++sv_blocks <= 1) return 1;
    for (uint32_t off = 0; off + 2 * FFT_N <= len; off += 2 * FFT_N) spectrum_add(buf + off, scan_acc);
    if (sv_blocks < 3) return 1;                               /* two blocks averaged: steadier on weak stations */
    /* window quiet level: 20th percentile of its bins */
    static float tmp[FFT_N];
    for (int k = 0; k < FFT_N; k++) tmp[k] = scan_acc[k];
    qsort(tmp, FFT_N, sizeof(float), cmp_float);
    float quiet = tmp[FFT_N / 5] + 1e-9f;
    double next = 0;
    for (int c = 0; c < sv_n; c++) {
        double rel = sv_ch[c] - sv_win;
        if (rel > 550000 && !next) next = sv_ch[c];             /* first channel beyond this window */
        if (fabs(rel) > 550000 || fabs(rel) < 3000) continue;   /* outside the flat part, or on DC */
        int k1 = (int)((rel - sv_half) / BIN_HZ) + FFT_N / 2, k2 = (int)((rel + sv_half) / BIN_HZ) + FFT_N / 2;
        float pk = 0; double sum = 0; int nb = 0;
        for (int k = k1; k <= k2; k++)
            if (k >= 0 && k < FFT_N && fabs((k - FFT_N / 2) * BIN_HZ) > 3000) { if (scan_acc[k] > pk) pk = scan_acc[k]; sum += scan_acc[k]; nb++; }
        /* the tuner mirrors strong signals across the window's centre: if the mirror position holds
           something 10 dB stronger, this reading may be that mirror, so don't trust it. Otherwise keep
           the best reading (a station near a window edge reads low there, but fine from another window). */
        double mrel = -rel;
        int m1 = (int)((mrel - sv_half) / BIN_HZ) + FFT_N / 2, m2 = (int)((mrel + sv_half) / BIN_HZ) + FFT_N / 2;
        float mpk = 0;
        for (int k = m1; k <= m2; k++) if (k >= 0 && k < FFT_N && scan_acc[k] > mpk) mpk = scan_acc[k];
        if (mpk > pk * 10) continue;
        if (sv_mode == M_WFM) {
            /* shape: a station is a hump centred on its channel; a spot between two stations only
               catches their spill, so it dips in the middle and rises toward both sides */
            double cen = 0, lft = 0, rgt = 0; int nc = 0, nl = 0, nr = 0;
            for (int k = (int)((rel - 70000) / BIN_HZ) + FFT_N / 2; k <= (int)((rel + 70000) / BIN_HZ) + FFT_N / 2; k++) {
                if (k < 0 || k >= FFT_N) continue;
                double o = (k - FFT_N / 2) * BIN_HZ - rel;
                if (fabs(o) <= 10000) { cen += scan_acc[k]; nc++; }
                else if (o < -50000) { lft += scan_acc[k]; nl++; }
                else if (o > 50000) { rgt += scan_acc[k]; nr++; }
            }
            if (nc && nl && nr) {
                cen /= nc; lft /= nl; rgt /= nr;
                if (cen < 0.5 * (lft > rgt ? lft : rgt)) continue;   /* dips in the middle: spill, not a station */
            }
        }
        float snr = 10 * log10f(pk / quiet);
        if (snr > sv_snr[c]) sv_snr[c] = snr;
    }
    /* next window half a megahertz on, so every channel is seen from two positions; jump straight
       over empty stretches of a sparse channel list */
    double step_to = sv_win + 500000;
    if (next && next - 450000 > step_to) step_to = next + 450000;
    int more = 0;
    for (int c = 0; c < sv_n; c++) if (sv_ch[c] > sv_win + 50000) { more = 1; break; }
    if (more) { sv_win = step_to; return 1; }
    /* done: a channel counts if it is well above the quiet level and nothing stronger is close
       enough that this could just be its bleed */
    float need = sv_mode == M_WFM ? 9 : sv_mode == M_AM ? 12 : 10;
    int n = 0;
    for (int c = 0; c < sv_n && n < 400; c++) {
        if (sv_snr[c] < need) continue;
        int bleed = 0;
        for (int d = 0; d < sv_n; d++)
            if (d != c && fabs(sv_ch[d] - sv_ch[c]) < sv_bleed &&
                (sv_snr[d] > sv_snr[c] || (sv_snr[d] == sv_snr[c] && d < c))) bleed = 1;
        if (!bleed) { sv_found_name[n] = sv_name[c]; sv_found[n++] = sv_ch[c]; }
    }
    sv_found_n = n;
    if (g_listen_secs) {                                     /* test runs: every channel's reading */
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s", dec_log_path);
        strcpy(strrchr(path, '\\') + 1, "survey_test.log");
        FILE *fp = fopen(path, "w");
        for (int c = 0; fp && c < sv_n; c++) fprintf(fp, "%.4f %.1f\n", sv_ch[c] / 1e6, sv_snr[c]);
        if (fp) fclose(fp);
    }
    sv_active = 0; g_survey_req = 0; fade_target = 1;
    PostMessageW(g_wnd, WM_APP + 6, 0, 0);
    return 0;
}

/* runs in the processing thread; returns 1 while scanning (normal listening is suspended) */
static int scan_step(unsigned char *buf, uint32_t len)
{
    if (g_survey_req || sv_active) return survey_step(buf, len);
    static const float stop_db[4] = { 10, 10, 14, 20 };   /* squelch off scans like "low" */
    double lo = cats[g_cat].lo, hi = cats[g_cat].hi;
    if (!scan_active) {
        if (!g_scan_req) return 0;
        double tap, fine, glide, gmax;
        mode_params(g_mode, g_station, &tap, &fine, &glide, &gmax);
        double skip = g_mode == M_WFM ? 150000 : g_station < 30e6 ? 5000 : 15000;
        if (g_station == g_found_hz && scan_skip_hz > skip) skip = scan_skip_hz;   /* continuing past a find */
        else { scan_in_wide = 0; scan_ref = 0; }
        scan_active = 1; scan_wrapped = 0; scan_measuring = 0; scan_confirming = 0;
        if (g_explore_lap) { scan_wrapped = g_explore_wrapped; }   /* Explore: one lap from where it began */
        /* scanning outside the crowded FM band: listen with plenty of gain (AGC is paused) */
        if (ngains && !(g_station >= 87e6 && g_station <= 108.5e6)) { g_gain_idx = nearest_gain(420); SetEvent(retune_event); }
        scan_start = g_explore_lap ? g_explore_origin : g_station;
        scan_from = g_station + skip;
        if (scan_from >= hi) { scan_from = lo; scan_wrapped = 1; g_explore_wrapped = 1; }
        scan_lo = scan_from + 600000;
        fade_target = 0;
    }
    if (!g_scan_req) { scan_active = 0; fade_target = 1; return 0; }   /* stopped: normal tuning retunes back */
    scan_audio((int)(len / 2 / D_N));
    if (g_retune_pending) return 1;                         /* waiting for the hardware to move */
    if (!scan_measuring) {
        if (lo_used != scan_lo) {
            g_lo_next = scan_lo;
            InterlockedExchange(&g_retune_pending, 1);
            SetEvent(retune_event);
            tick_left = 120;
            return 1;
        }
        scan_measuring = 1; scan_blocks = 0;
        memset(scan_acc, 0, sizeof scan_acc);
    }
    for (uint32_t off = 0; off + 2 * FFT_N <= len; off += 2 * FFT_N) spectrum_add(buf + off, scan_acc);
    if (++scan_blocks < 2) return 1;
    scan_measuring = 0;

    /* local noise floor: median of each 64-bin (23 kHz) segment, then the lowest of the segments
       within +-6 (about +-140 kHz). The tuner's response isn't flat across the window, so a
       single window-wide floor made the middle look like a signal. */
    enum { SEG = 64, NSEG = FFT_N / SEG };
    static float seg_med[NSEG], local[NSEG], tmp[SEG];
    for (int s = 0; s < NSEG; s++) {
        memcpy(tmp, scan_acc + s * SEG, sizeof tmp);
        qsort(tmp, SEG, sizeof(float), cmp_float);
        seg_med[s] = tmp[SEG / 2];
    }
    for (int s = 0; s < NSEG; s++) {
        float m = seg_med[s];
        for (int d = -6; d <= 6; d++) if (s + d >= 0 && s + d < NSEG && seg_med[s + d] < m) m = seg_med[s + d];
        local[s] = m;
    }
    /* learned quiet profile: per segment, the lowest level seen in recent windows (it captures
       the tuner's uneven response). A segment 3 dB above it is occupied by something wide,
       like a digital TV channel (6 MHz), whose lumps must not count as separate signals. */
    static float ref_prof[NSEG];
    static int occupied[NSEG];
    {
        int n_occ = 0;
        /* uses the neighbourhood floor (local), which narrow signals don't raise */
        if (scan_ref <= 0) { memcpy(ref_prof, local, sizeof ref_prof); scan_ref = 1; }
        for (int s = 0; s < NSEG; s++) {
            occupied[s] = local[s] > ref_prof[s] * 2;
            n_occ += occupied[s];
            ref_prof[s] = occupied[s] ? ref_prof[s] * 1.03f : fminf(ref_prof[s] * 1.03f, local[s]);
        }
        if (n_occ * 4 >= NSEG * 3) {                   /* three quarters of the window is one wide signal */
            if (!scan_in_wide && !scan_confirming) {
                scan_in_wide = 1;
                g_found_hz = round_to(scan_lo, 1000);
                scan_active = 0; g_scan_req = 0;
                g_station = g_found_hz;
                scan_skip_hz = 600000;
                fade_target = 1;
                PostMessageW(g_wnd, WM_APP, (WPARAM)guess_mode(scan_lo, 1e6), 2);
                return 0;
            }
            goto next_window;                          /* still inside it: keep moving */
        }
        scan_in_wide = 0;
    }
    float gain_thr = powf(10, stop_db[g_squelch] / 10);
    int found = -1, k1 = 0, k2 = 0;
    for (int k = 1; k < FFT_N - 1 && found < 0; k++) {
        double rel = (k - FFT_N / 2) * BIN_HZ, f = scan_lo + rel;
        if (fabs(rel) < 3000 || fabs(rel) > 600000 || f <= scan_from || f > hi) continue;
        double birdie = fmod(f, 9.6e6);                /* the laptop's 19.2 MHz clock (and half of it) */
        if (birdie < 4000 || birdie > 9.6e6 - 4000) continue;
        if (occupied[k / SEG]) continue;               /* inside a wide signal already reported */
        float fl = local[k / SEG];
        if (scan_acc[k] < fl * gain_thr || scan_acc[k + 1] < fl * gain_thr / 4) continue;   /* 2 bins: skips spurs */
        if (is_locked_out(f)) continue;
        k1 = k; k2 = k;                                     /* grow to the signal's width */
        /* 4 dB above the neighbourhood floor, bridging gaps of up to 3 bins, so a lumpy wide
           signal is measured as one thing instead of several pieces */
        for (int gap = 0; k1 > 0 && gap <= 3; k1--) gap = scan_acc[k1 - 1] > local[(k1 - 1) / SEG] * 2.5f ? 0 : gap + 1;
        for (int gap = 0; k2 < FFT_N - 1 && gap <= 3; k2++) gap = scan_acc[k2 + 1] > local[(k2 + 1) / SEG] * 2.5f ? 0 : gap + 1;
        while (k1 < k && scan_acc[k1] <= local[k1 / SEG] * 2.5f) k1++;     /* trim the bridged tails */
        while (k2 > k && scan_acc[k2] <= local[k2 / SEG] * 2.5f) k2--;
        /* judge the whole signal, not its peak: weak noise humps from nearby electronics have
           peaks but a low average. Hiss-like (flat) candidates must be stronger still. */
        {
            static const float min_avg_db[4] = { 4, 4, 6, 10 };
            double sum = 0, sum_db = 0, fl_sum = 0;
            for (int j = k1; j <= k2; j++) { sum += scan_acc[j]; sum_db += 10 * log10(scan_acc[j] + 1e-12); fl_sum += local[j / SEG]; }
            int n = k2 - k1 + 1;
            double avg_db = 10 * log10(sum / fl_sum), flat = 10 * log10(sum / n) - sum_db / n;
            double need = min_avg_db[g_squelch] + ((k2 - k1) * BIN_HZ >= 5000 && flat < 1.5 ? 3 : 0);
            if (avg_db < need) { k = k2; continue; }        /* not convincing: skip past it */
            g_dbg_avg = avg_db; g_dbg_flat = flat; g_dbg_w = (k2 - k1 + 1) * BIN_HZ;
        }
        found = k;
    }
    if (found >= 0) {
        double centre = scan_lo + ((k1 + k2) / 2.0 - FFT_N / 2) * BIN_HZ;
        /* only stop if a second measurement sees it again: random noise flickers don't repeat */
        if (!scan_confirming || fabs(centre - scan_candidate) > 4 * BIN_HZ + (k2 - k1) * BIN_HZ / 2) {
            scan_confirming = 1; scan_candidate = centre;
            return 1;                                       /* measure this window again */
        }
        scan_confirming = 0;
        double width = (k2 - k1 + 1) * BIN_HZ;
        /* flatness: noise-like digital signals have an even, flat spectrum; voices and carriers don't */
        double sum = 0, sum_db = 0;
        for (int k = k1; k <= k2; k++) { sum += scan_acc[k]; sum_db += 10 * log10(scan_acc[k] + 1e-12); }
        int n = k2 - k1 + 1;
        double flat_db = 10 * log10(sum / n) - sum_db / n;
        int digital = width >= 5000 && flat_db < 1.5;
        int m = guess_mode(centre, width);
        /* snap to the band's channel grid: FM 100 kHz, AM 10 kHz, two-way radio 6.25 kHz */
        double grid = m == M_WFM ? 100000 : (centre >= 0.53e6 && centre <= 1.71e6) ? 10000 : (m == M_USB || m == M_LSB) ? 100
                    : (m == M_NFM && centre > 30e6) ? 6250 : 1000;
        g_found_hz = round_to(centre, grid);
        scan_active = 0; g_scan_req = 0;
        g_station = g_found_hz;
        scan_skip_hz = width / 2 + 5000;                    /* next scan starts past all of it */
        if (width > 100000) scan_in_wide = 1;
        fade_target = 1;
        PostMessageW(g_wnd, WM_APP, (WPARAM)m, (LPARAM)digital);
        return 0;
    }
    if (scan_confirming) {                                  /* the candidate vanished: keep going past it */
        scan_confirming = 0;
        scan_from = scan_candidate + 5000;
        return 1;
    }
next_window:
    /* nothing here: next window, wrapping once around the category */
    scan_from = scan_lo + 600000;
    if (scan_wrapped && scan_from >= scan_start) {
        scan_active = 0; g_scan_req = 0; fade_target = 1;
        PostMessageW(g_wnd, WM_APP + 1, 0, 0);
        return 0;
    }
    if (scan_from >= hi) { scan_from = lo; scan_wrapped = 1; g_explore_wrapped = 1; PostMessageW(g_wnd, WM_APP + 2, 0, 0); }
    scan_lo = scan_from + 600000;
    return 1;
}

/* ---------- decoders: multimon-ng (pagers, EAS alerts, APRS, DTMF, Morse) ----------
   The narrow-mode audio is resampled to 22050 Hz and piped into multimon-ng; a reader thread
   turns its output into plain-English lines that are spoken and logged. */
static volatile int g_decode_on = 1, g_decode_speak = 1;
static HANDLE mm_proc, mm_in, mm_out;
static int mm_cw;                          /* running with the Morse decoder (CW mode) */
static double mm_pos;                      /* resampler position */
static short mm_buf[4096]; static int mm_n;
#define DEC_HIST 200
static char dec_hist[DEC_HIST][400];
static volatile LONG dec_count;
static volatile int dec_read_pos;          /* R steps back from the newest */
static char dec_log_path[MAX_PATH];
static void say(const char *text);
static void beep(int kind);
static CRITICAL_SECTION dec_lock;

static void dec_add(const char *text)
{
    EnterCriticalSection(&dec_lock);
    snprintf(dec_hist[dec_count % DEC_HIST], 400, "%s", text);
    dec_count++;
    dec_read_pos = 0;
    LeaveCriticalSection(&dec_lock);
    FILE *fp = fopen(dec_log_path, "a");
    if (fp) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d  %.3f MHz  %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                t.wSecond, g_station / 1e6, text);
        fclose(fp);
    }
    if (g_decode_speak) { beep(6); say(text); }
}

static const char *same_event(const char *c)
{
    static const char *tab[][2] = {
        {"TOR","Tornado Warning"},{"TOA","Tornado Watch"},{"SVR","Severe Thunderstorm Warning"},
        {"SVA","Severe Thunderstorm Watch"},{"SVS","Severe Weather Statement"},{"FFW","Flash Flood Warning"},
        {"FFA","Flash Flood Watch"},{"FFS","Flash Flood Statement"},{"FLW","Flood Warning"},{"FLA","Flood Watch"},
        {"FLS","Flood Statement"},{"WSW","Winter Storm Warning"},{"WSA","Winter Storm Watch"},{"BZW","Blizzard Warning"},
        {"HWW","High Wind Warning"},{"HWA","High Wind Watch"},{"EWW","Extreme Wind Warning"},{"SPS","Special Weather Statement"},
        {"RWT","Required Weekly Test"},{"RMT","Required Monthly Test"},{"NPT","National Periodic Test"},{"DMO","Demonstration"},
        {"EAN","Emergency Action Notification"},{"CAE","Child Abduction Emergency"},{"CEM","Civil Emergency Message"},
        {"FRW","Fire Warning"},{"EVI","Evacuate Immediately"},{"SPW","Shelter in Place Warning"},{"HUW","Hurricane Warning"},
        {"HUA","Hurricane Watch"},{"ADR","Administrative Message"},{"TOE","911 Telephone Outage"},{"LAE","Local Area Emergency"},
        {"HMW","Hazardous Materials Warning"},{"NUW","Nuclear Power Plant Warning"},{"DSW","Dust Storm Warning"},
        {"ISW","Ice Storm Warning"},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strncmp(c, tab[i][0], 3)) return tab[i][1];
    return NULL;
}

/* PSSCCC (P = part of county, SS state, CCC county) via counties.txt, the Census list of every
   US county: "SSCCC|County name|State" (CCC 000 = the whole state) */
static void same_place(const char *p, char *out, size_t n)
{
    char path[MAX_PATH], line[160], state[64] = "";
    snprintf(path, sizeof path, "%s", dec_log_path); strcpy(strrchr(path, '\\') + 1, "counties.txt");
    FILE *fp = fopen(path, "r");
    char key[6]; snprintf(key, sizeof key, "%.2s000", p + 1);
    while (fp && fgets(line, sizeof line, fp)) {
        if (strncmp(line, key, 5) && strncmp(line, p + 1, 5)) continue;
        char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : NULL;
        if (!a || !b) continue;
        *b = 0; char *st = b + 1; st[strcspn(st, "\r\n")] = 0;
        if (!strncmp(line, key, 5)) snprintf(state, sizeof state, "%s", st);
        if (!strncmp(line, p + 1, 5) && a[1]) { fclose(fp); snprintf(out, n, "%s, %s", a + 1, st); return; }
    }
    if (fp) fclose(fp);
    if (!strncmp(p + 3, "000", 3)) snprintf(out, n, "all of %s", state[0] ? state : "state code");
    else snprintf(out, n, "county code %.5s%s%s", p + 1, state[0] ? ", " : "", state);
}

/* ZCZC-ORG-EEE-PSSCCC-PSSCCC+TTTT-JJJHHMM-LLLLLLLL- into words */
static void same_words(const char *z, char *out, size_t n)
{
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s", z);
    char *field[40]; int nf = 0;
    for (char *t = strtok(tmp, "-"); t && nf < 40; t = strtok(NULL, "-")) field[nf++] = t;
    if (nf < 4) { snprintf(out, n, "Alert: %s", z); return; }
    const char *org = !strcmp(field[1], "WXR") ? "National Weather Service" : !strcmp(field[1], "EAS") ? "a broadcast station"
                    : !strcmp(field[1], "CIV") ? "civil authorities" : !strcmp(field[1], "PEP") ? "the national Primary Entry Point" : field[1];
    const char *ev = same_event(field[2]);
    size_t used = snprintf(out, n, "Alert from %s: %s", org, ev ? ev : field[2]);
    for (int i = 3; i < nf; i++) {
        char *plus = strchr(field[i], '+');
        char place[96];
        if (strlen(field[i]) >= 6 && isdigit((unsigned char)field[i][0])) {
            same_place(field[i], place, sizeof place);
            used += snprintf(out + used, used < n ? n - used : 0, ", %s", place);
        }
        if (plus && strlen(plus) >= 5) {
            int hh = (plus[1] - '0') * 10 + (plus[2] - '0'), mm = (plus[3] - '0') * 10 + (plus[4] - '0');
            if (hh) used += snprintf(out + used, used < n ? n - used : 0, ", for %d hour%s%s", hh, hh > 1 ? "s" : "", mm ? " and" : "");
            if (mm || !hh) used += snprintf(out + used, used < n ? n - used : 0, "%s%d minutes", hh ? " " : ", for ", mm);
            break;
        }
    }
}

static char dtmf[64]; static volatile int ndtmf; static volatile DWORD dtmf_t;

/* touch-tone digits arrive one per line: speak them as one number after a 1.5 s pause */
static void dtmf_flush(void)
{
    if (!ndtmf || GetTickCount() - dtmf_t < 1500) return;
    char out[100];
    snprintf(out, sizeof out, "Touch tones: %s", dtmf);
    ndtmf = 0; dtmf[0] = 0;
    dec_add(out);
}

static void mm_line(char *line)
{
    char out[400];
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\r' || line[L - 1] == '\n')) line[--L] = 0;
    if (!L) return;
    if (!strncmp(line, "EAS: ", 5)) {
        const char *z = line + 5;
        if (!strncmp(z, "NNNN", 4)) { dec_add("End of alert"); return; }
        static char last[300]; static DWORD last_t;      /* headers repeat 3 times */
        if (!strcmp(last, z) && GetTickCount() - last_t < 15000) return;
        snprintf(last, sizeof last, "%s", z); last_t = GetTickCount();
        same_words(z, out, sizeof out); dec_add(out); return;
    }
    if (!strncmp(line, "POCSAG", 6)) {
        char *addr = strstr(line, "Address:"), *msg = strstr(line, "Alpha:");
        if (!msg) msg = strstr(line, "Numeric:");
        if (!msg) return;                                 /* tone-only page */
        msg = strchr(msg, ':') + 1; while (*msg == ' ') msg++;
        if (!*msg) return;
        snprintf(out, sizeof out, "Pager %ld: %s", addr ? atol(addr + 8) : 0L, msg); dec_add(out); return;
    }
    if (!strncmp(line, "FLEX", 4)) {
        char *last = strrchr(line, '|');
        if (!last || !last[1]) return;
        snprintf(out, sizeof out, "Pager: %s", last + 1); dec_add(out); return;
    }
    if (!strncmp(line, "DTMF: ", 6)) {
        if (ndtmf < 60) { dtmf[ndtmf] = line[6]; dtmf[ndtmf + 1] = 0; ndtmf++; }
        dtmf_t = GetTickCount();
        return;                                           /* spoken after a pause: dtmf_flush */
    }
    if (!strncmp(line, "AFSK1200: ", 10)) { snprintf(out, sizeof out, "Packet: %s", line + 10); dec_add(out); return; }
    if (!strncmp(line, "MORSE_CW: ", 10)) { snprintf(out, sizeof out, "Morse: %s", line + 10); dec_add(out); return; }
    if (strchr(line, ':')) dec_add(line);                 /* other decoders: as they are */
}

static DWORD WINAPI mm_reader(LPVOID p)
{
    HANDLE h = (HANDLE)p;
    char buf[1024], line[1024];
    int n = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof buf, &got, NULL) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || n == (int)sizeof line - 1) { line[n] = 0; mm_line(line); n = 0; }
            else line[n++] = buf[i];
        }
    }
    CloseHandle(h);
    return 0;
}

static void mm_stop(void)
{
    if (!mm_proc) return;
    CloseHandle(mm_in); mm_in = NULL;                    /* EOF: multimon exits, the reader ends */
    if (WaitForSingleObject(mm_proc, 500) != WAIT_OBJECT_0) TerminateProcess(mm_proc, 0);
    CloseHandle(mm_proc); mm_proc = NULL;
}

static void mm_start(int cw)
{
    char exe[MAX_PATH], cmd[600];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(strrchr(exe, '\\') + 1, "multimon-ng.exe");
    snprintf(cmd, sizeof cmd, cw ? "\"%s\" -q -t raw -c -a MORSE_CW -" :
             "\"%s\" -q -t raw -e -u -c -a POCSAG512 -a POCSAG1200 -a POCSAG2400 -a FLEX -a EAS -a AFSK1200 -a DTMF -", exe);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 1 << 20)) return;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) { CloseHandle(in_r); CloseHandle(in_w); return; }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = out_w;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r); CloseHandle(out_w); return;
    }
    CloseHandle(pi.hThread); CloseHandle(in_r); CloseHandle(out_w);
    mm_proc = pi.hProcess; mm_in = in_w; mm_cw = cw; mm_n = 0;
    CloseHandle(CreateThread(NULL, 0, mm_reader, out_r, 0, NULL));
}

/* called from the processing thread with each 48012 Hz narrow-mode audio sample */
static void mm_feed(float x, int mode)
{
    int want = g_decode_on && mode != M_WFM;
    int cw = mode == M_CW;
    if (!want) { if (mm_proc) mm_stop(); return; }
    if (mm_proc && mm_cw != cw) mm_stop();
    if (!mm_proc) mm_start(cw);
    if (!mm_proc) return;
    mm_pos += 22050.0 / FS_N;                             /* nearest-sample resample to 22050 */
    if (mm_pos < 1.0) return;
    mm_pos -= 1.0;
    float v = x * 20000.f;
    mm_buf[mm_n++] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    if (mm_n == (int)(sizeof mm_buf / sizeof mm_buf[0])) {
        DWORD w;
        if (!WriteFile(mm_in, mm_buf, sizeof mm_buf, &w, NULL)) mm_stop();
        mm_n = 0;
    }
}

static int g_listen_secs;                 /* --listen <MHz> <mode> <seconds> */

/* ---------- dsd-neo: digital voice (P25, DMR, NXDN, D-STAR, YSF...) ----------
   Fed the same 48 kHz narrow-FM discriminator audio; its stdout is decoded speech
   (8 kHz mono, only while someone talks) and its stderr is text about the call. */
static HANDLE ds_proc, ds_in;
static short ds_buf[4096]; static int ds_n;
static out_t voi;

/* decoded-voice player: a ring the decoder fills, and a paced thread that plays it in 20 ms
   blocks for as long as a call is active. Missing frames become short silence instead of the
   device running dry, pausing and re-buffering (which is what made speech stutter). */
#define VRING 32000                                   /* 4 s at 8 kHz */
static short vring[VRING];
static volatile LONG vw, vr;
static volatile LONGLONG v_last;                       /* when decoded voice last arrived */

static void voice_push(const short *s, DWORD n)
{
    for (DWORD i = 0; i < n; i++) { vring[vw] = s[i]; vw = (vw + 1) % VRING; }
    LONG fill = (vw - vr + VRING) % VRING;
    if (fill > 8000) vr = (vw - 2400 + VRING) % VRING;  /* more than 1 s behind: catch up */
    v_last = now();
}

static DWORD WINAPI voice_thread(LPVOID p)
{
    (void)p;
    int playing = 0;
    LONGLONG next = now(), step = (LONGLONG)(0.020 * g_qpf);
    while (!g_stop) {
        LONG fill = (vw - vr + VRING) % VRING;
        int active = v_last && (now() - v_last) < (LONGLONG)(1.2 * g_qpf);
        if (!playing) {
            /* start a call once 300 ms are buffered (or the burst is short and complete) */
            if (fill >= 2400 || (fill > 0 && !active)) { playing = 1; next = now(); }
            else { Sleep(5); continue; }
        }
        if (!active && fill == 0) { playing = 0; continue; }
        float v = 1.6f * g_volume;
        for (int i = 0; i < 160; i++) {
            short s = 0;
            if ((vw - vr + VRING) % VRING) { s = vring[vr]; vr = (vr + 1) % VRING; }
            s = clip16(s * v);
            out_frame(&voi, s, s);
        }
        next += step;
        LONGLONG wait = next - now();
        if (wait > 0) Sleep((DWORD)(wait * 1000 / g_qpf));
        else if (wait < -step * 5) next = now();        /* fell behind (system busy): resync */
    }
    return 0;
}

static char ds_call[160], ds_said[160];       /* call info from stderr; last one announced */
static DWORD ds_said_t;

static DWORD WINAPI ds_voice_reader(LPVOID p)
{
    HANDLE h = (HANDLE)p;
    short buf[1024];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof buf, &got, NULL) || !got) break;
        /* voice is starting: announce who it is, once per call (control channels mention
           talkgroups constantly, so only real voice triggers an announcement) */
        if (now() > g_voice_until && ds_call[0] && (strcmp(ds_call, ds_said) || GetTickCount() - ds_said_t > 20000)) {
            snprintf(ds_said, sizeof ds_said, "%s", ds_call);
            ds_said_t = GetTickCount();
            dec_add(ds_said);
        }
        voice_push(buf, got / 2);
        if (g_listen_secs) {                                /* test runs keep the decoded voice, and its timing */
            char path[MAX_PATH]; snprintf(path, sizeof path, "%s", dec_log_path);
            strcpy(strrchr(path, '\\') + 1, "voice_test.raw");
            FILE *fp = fopen(path, "ab"); if (fp) { fwrite(buf, 1, got, fp); fclose(fp); }
            strcpy(strrchr(path, '\\') + 1, "voice_timing.log");
            fp = fopen(path, "a"); if (fp) { fprintf(fp, "%.3f %lu\n", now() / g_qpf, got); fclose(fp); }
        }
        g_voice_until = now() + (LONGLONG)(0.5 * g_qpf);
    }
    CloseHandle(h);
    return 0;
}

/* talkgroups.csv beside presets.txt: WACN,talkgroup,spoken name,encrypted. Re-read when it changes. */
#define MAX_TG 3000
static struct { char wacn[8]; long tg; char name[96]; } tgs[MAX_TG];
static int ntg;
static FILETIME tg_mtime;

static const char *tg_name(const char *wacn, long tg)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s", saved_path);
    strcpy(strrchr(path, '\\') + 1, "talkgroups.csv");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &tg_mtime)) {
        tg_mtime = fa.ftLastWriteTime; ntg = 0;
        FILE *fp = fopen(path, "r");
        char line[300];
        while (fp && fgets(line, sizeof line, fp) && ntg < MAX_TG) {
            if (line[0] == '#') continue;
            char *f[4] = {0}; int k = 0;
            for (char *t = strtok(line, ","); t && k < 4; t = strtok(NULL, ",")) f[k++] = trim(t);
            if (k >= 3 && atol(f[1]) > 0) {
                snprintf(tgs[ntg].wacn, sizeof tgs[ntg].wacn, "%s", f[0]);
                tgs[ntg].tg = atol(f[1]);
                snprintf(tgs[ntg].name, sizeof tgs[ntg].name, "%s", f[2]);
                ntg++;
            }
        }
        if (fp) fclose(fp);
    }
    for (int i = 0; i < ntg; i++)
        if (tgs[i].tg == tg && (!wacn[0] || !_stricmp(tgs[i].wacn, wacn))) return tgs[i].name;
    return NULL;
}

/* ---- signal identification for Explore: what digital protocol is this, with its IDs ---- */
#define NPROTO 12
static const char *proto_keys[NPROTO]  = { "P25p1", "P25p2", "DMR", "NXDN48", "NXDN96", "YSF", "DSTAR", "dPMR", "M17", "EDACS", "PV", "IDAS" };
static const char *proto_names[NPROTO] = { "P25", "P25 phase 2", "DMR", "NXDN", "NXDN", "Yaesu Fusion", "D-Star", "dPMR", "M17", "EDACS", "ProVoice", "NXDN" };
static volatile LONG id_counts[NPROTO], id_total;
static volatile int id_p25_control, id_p25_voice, id_capplus, id_conplus, id_tier3, id_dmr_cc = -1;
static char id_wacn[8], id_sys[8], id_nac[8];

static void id_reset(void)
{
    for (int i = 0; i < NPROTO; i++) id_counts[i] = 0;
    id_total = 0; id_p25_control = id_p25_voice = id_capplus = id_conplus = id_tier3 = 0; id_dmr_cc = -1;
    id_wacn[0] = id_sys[0] = id_nac[0] = 0;
}

static volatile LONGLONG g_digital_until;          /* raw audio is muted while a digital signal is confirmed */

static void id_line(const char *line)
{
    const char *p = strstr(line, "Sync: ");
    if (p) {
        p += 6; while (*p == '+' || *p == '-' || *p == ' ') p++;
        for (int i = 0; i < NPROTO; i++)
            if (!strncmp(p, proto_keys[i], strlen(proto_keys[i])) && !strstr(line, "ERR")) {
                id_counts[i]++; id_total++;
                /* four clean syncs within a second: this is a digital signal, not analog fooling the detector */
                static LONGLONG t4[4]; static int n4;
                LONGLONG tn = now(); t4[n4++ & 3] = tn;
                if (t4[n4 & 3] && tn - t4[n4 & 3] < (LONGLONG)g_qpf) g_digital_until = tn + (LONGLONG)(0.8 * g_qpf);
                break;
            }
        if (strstr(line, "TSBK")) id_p25_control++;
        if (strstr(line, "LDU")) id_p25_voice++;
    }
    const char *q;
    if ((q = strstr(line, "WACN: "))) sscanf(q + 6, "%7[0-9A-Fa-f]", id_wacn);
    if ((q = strstr(line, "SYS: "))) sscanf(q + 5, "%7[0-9A-Fa-f]", id_sys);
    if ((q = strstr(line, "NAC/CC: "))) sscanf(q + 8, "%7[0-9A-Fa-f]", id_nac);
    if ((q = strstr(line, "Color Code="))) id_dmr_cc = atoi(q + 11);
    if (strstr(line, "Capacity Plus")) id_capplus++;
    if (strstr(line, "Connect Plus")) id_conplus++;
    if (strstr(line, "Aloha") || strstr(line, "TIII") || strstr(line, "Tier III")) id_tier3++;
}

/* a short description of what was heard, or NULL if nothing digital was clear */
static const char *id_describe(char *out, size_t n)
{
    int best = -1;
    for (int i = 0; i < NPROTO; i++) if (best < 0 || id_counts[i] > id_counts[best]) best = i;
    if (best < 0 || id_counts[best] < 8 || id_counts[best] * 10 < id_total * 7) return NULL;   /* not convincing */
    size_t u = snprintf(out, n, "%s", proto_names[best]);
    if (best <= 1) {
        u += snprintf(out + u, n - u, "%s", id_p25_control > id_p25_voice ? " control channel" : " voice");
        if (id_wacn[0]) u += snprintf(out + u, n - u, ", system %s", id_wacn);
        else if (id_nac[0]) u += snprintf(out + u, n - u, ", NAC %s", id_nac);
    } else if (best == 2) {
        if (id_capplus) u += snprintf(out + u, n - u, " Capacity Plus");
        else if (id_conplus) u += snprintf(out + u, n - u, " Connect Plus");
        else if (id_tier3) u += snprintf(out + u, n - u, " trunked, tier 3");
        if (id_dmr_cc >= 0) u += snprintf(out + u, n - u, ", color code %d", id_dmr_cc);
    }
    return out;
}

/* ---- DMR Capacity Plus: learn which frequency carries which channel number (LSN) ----
   Each repeater carries two LSNs (1-2, 3-4, ...). The rest channel's LSN is announced, so its
   pair is known at once. When an unknown LSN goes busy, whichever 6.25 kHz channel in view just
   rose well above its idle level is that repeater. The map is saved per system and handed to
   dsd-neo (-C) so it can follow calls. */
#define CP_MAX_LSN 16
static volatile double g_trunk_cc;                 /* (defined with trunk following below) */
static volatile int g_trunk_dmr;                   /* trunking a DMR Capacity Plus system */
static double cp_freq[CP_MAX_LSN + 1];             /* learned LSN -> Hz (0 = unknown) */
static volatile int cp_rest = 0, cp_busy_unknown = 0, cp_cc = -1;
static volatile LONG cp_map_changed;
static char cp_map_path[MAX_PATH];
static volatile double g_tcp_center, g_tcp_rate = 1536000;   /* what dsd-neo tuned the dongle to */

static void cp_load_or_init(int cc, double rest_hz)
{
    memset(cp_freq, 0, sizeof cp_freq);
    snprintf(cp_map_path, sizeof cp_map_path, "%s", saved_path);
    char name[80]; snprintf(name, sizeof name, "capplus_cc%d_%.0f.csv", cc, rest_hz / 1000);
    strcpy(strrchr(cp_map_path, '\\') + 1, name);
    FILE *fp = fopen(cp_map_path, "r");
    char line[128];
    while (fp && fgets(line, sizeof line, fp)) {
        int lsn; double hz;
        if (sscanf(line, "%d,%lf", &lsn, &hz) == 2 && lsn >= 1 && lsn <= CP_MAX_LSN) cp_freq[lsn] = hz;
    }
    if (fp) fclose(fp);
    cp_cc = cc;
}

static void cp_save(void)
{
    FILE *fp = fopen(cp_map_path, "w");
    if (!fp) return;
    fprintf(fp, "LSN(dec),frequency(Hz) (do not delete this line or won't import properly)\n");
    for (int i = 1; i <= CP_MAX_LSN; i++) if (cp_freq[i] > 0) fprintf(fp, "%d,%.0f\n", i, cp_freq[i]);
    fclose(fp);
}

static void cp_learn_pair(int lsn, double hz)
{
    int a = (lsn - 1) / 2 * 2 + 1;                         /* the repeater's two LSNs */
    if (cp_freq[a] == hz && cp_freq[a + 1] == hz) return;
    cp_freq[a] = cp_freq[a + 1] = hz;
    cp_save();
    InterlockedIncrement(&cp_map_changed);
}

/* status lines: " Capacity Plus Channel Status ... Rest LSN: 4" and "LSN 01: Idle; LSN 02: Group 123 ..." */
static void cp_line(const char *line)
{
    const char *p;
    if ((p = strstr(line, "Rest LSN: "))) {
        int r = atoi(p + 10);
        if (r >= 1 && r <= CP_MAX_LSN) {
            cp_rest = r;
            /* we're parked on the rest channel when dsd-neo isn't on a call: that's its frequency */
            if (g_trunk_dmr && g_trunk_cc > 0 && !cp_freq[r]) cp_learn_pair(r, g_trunk_cc);
        }
    }
    int busy = 0;
    for (p = strstr(line, "LSN "); p; p = strstr(p + 4, "LSN ")) {
        int lsn = atoi(p + 4);
        const char *s = strchr(p, ':');
        if (lsn < 1 || lsn > CP_MAX_LSN || !s) continue;
        s++; while (*s == ' ') s++;
        int idle = !strncmp(s, "Idle", 4) || !strncmp(s, "Rest", 4) || !*s;
        if (!idle && !cp_freq[lsn]) busy = lsn;
    }
    if (strstr(line, "LSN 0")) cp_busy_unknown = busy;
}

/* spectrum watcher, fed raw IQ while dsd-neo owns the dongle: per-channel idle levels, then
   when an unknown LSN is busy, the channel that rose the most is its repeater */
#define CP_CH 256
static float cp_idle[CP_CH], cp_now[CP_CH];
static int cp_busy_blocks;

static void cp_watch(const unsigned char *buf, uint32_t len)
{
    static float acc[FFT_N]; static int nacc;
    if (!g_trunk_dmr || len < 4 * FFT_N) return;
    spectrum_add(buf, acc);
    if (++nacc < 8) return;
    nacc = 0;
    double bin = g_tcp_rate / FFT_N, ch_w = 6250;
    float lvl[CP_CH] = {0};
    int nch = (int)(g_tcp_rate * 0.9 / ch_w);              /* channels across the flat 90% of the view */
    if (nch > CP_CH) nch = CP_CH;
    for (int c = 0; c < nch; c++) {
        double rel = (c - nch / 2) * ch_w;
        int k1 = (int)((rel - ch_w / 2) / bin) + FFT_N / 2, k2 = (int)((rel + ch_w / 2) / bin) + FFT_N / 2;
        double s = 0; for (int k = k1; k <= k2 && k < FFT_N; k++) if (k >= 0) s += acc[k];
        lvl[c] = (float)(10 * log10(s + 1e-9));
    }
    memset(acc, 0, sizeof acc);
    int lsn = cp_busy_unknown;
    if (!lsn) {                                            /* idle: learn the quiet level of every channel */
        for (int c = 0; c < nch; c++) cp_idle[c] = cp_idle[c] == 0 ? lvl[c] : cp_idle[c] * 0.9f + lvl[c] * 0.1f;
        cp_busy_blocks = 0;
        return;
    }
    for (int c = 0; c < nch; c++) cp_now[c] = cp_busy_blocks ? cp_now[c] * 0.7f + lvl[c] * 0.3f : lvl[c];
    if (++cp_busy_blocks < 4) return;                      /* let the new carrier settle */
    int best = -1; float rise1 = 0, rise2 = 0;
    for (int c = 0; c < nch; c++) {
        double hz = g_tcp_center + (c - nch / 2) * ch_w;
        if (fabs(hz - g_trunk_cc) < 10000 || fabs(hz - g_tcp_center) < 10000) continue;   /* rest channel, DC */
        float r = cp_now[c] - cp_idle[c];
        if (r > rise1) { rise2 = rise1; rise1 = r; best = c; } else if (r > rise2) rise2 = r;
    }
    if (best >= 0 && rise1 > 10 && rise1 - rise2 > 6) {
        double hz = g_tcp_center + (best - nch / 2) * ch_w;
        hz = floor(hz / 6250 + 0.5) * 6250;
        cp_learn_pair(lsn, hz);
        cp_busy_unknown = 0;
    }
}

/* one announcement per call: protocol, talkgroup (by name when known), radio, encrypted */
static void ds_line(char *line)
{
    id_line(line);
    cp_line(line);
    static char proto[32], tg[32], src[32], last_call[160];
    static int encrypted;
    static DWORD last_t;
    char *p;
    if ((p = strstr(line, "Sync: "))) {
        p += 6; while (*p == '+' || *p == '-' || *p == ' ') p++;
        char np[32]; int i = 0;
        while (*p && *p != ' ' && i < 31) np[i++] = *p++;
        np[i] = 0;
        if (!strncmp(np, "P25p1", 5)) strcpy(np, "P25");
        else if (!strncmp(np, "P25p2", 5)) strcpy(np, "P25 phase 2");
        if (strcmp(np, proto)) { snprintf(proto, sizeof proto, "%s", np); tg[0] = src[0] = 0; encrypted = 0; }
    }
    /* a new grant starts a new call: forget the last one's details */
    if (strstr(line, "Channel Grant")) { tg[0] = src[0] = 0; encrypted = 0; }
    /* talkgroups and sources appear as "Group 51", "Group [51]", "TGT=51", "Dst/TG=51" */
    for (p = strstr(line, "Group "); p; p = strstr(p + 6, "Group ")) {
        const char *q = p + 6; if (*q == '[') q++;
        if (isdigit((unsigned char)*q)) { snprintf(tg, sizeof tg, "%ld", atol(q)); break; }
    }
    if ((p = strstr(line, "TGT="))) snprintf(tg, sizeof tg, "%ld", atol(p + 4));
    if ((p = strstr(line, "Dst/TG="))) snprintf(tg, sizeof tg, "%ld", atol(p + 7));
    for (p = strstr(line, "Source "); p; p = strstr(p + 7, "Source ")) {
        const char *q = p + 7; if (*q == '[') q++;
        if (isdigit((unsigned char)*q)) { snprintf(src, sizeof src, "%ld", atol(q)); break; }
    }
    if ((p = strstr(line, "SRC="))) snprintf(src, sizeof src, "%ld", atol(p + 4));
    if ((p = strstr(line, "Src="))) snprintf(src, sizeof src, "%ld", atol(p + 4));
    if ((p = strstr(line, "ALG ID: 0x"))) { long alg = strtol(p + 10, NULL, 16); encrypted = alg != 0x80 && alg != 0; }
    static char wacn[8];
    if ((p = strstr(line, "WACN: "))) { int i = 0; p += 6; while (isxdigit((unsigned char)*p) && i < 7) wacn[i++] = *p++; wacn[i] = 0; }
    if (!strcmp(src, "0")) src[0] = 0;                 /* radio 0: a false sync on analog audio */
    if (!proto[0] || (!tg[0] && !src[0])) return;
    const char *name = tg[0] ? tg_name(wacn, atol(tg)) : NULL;
    if (name) snprintf(ds_call, sizeof ds_call, "%s%s%s%s", name, src[0] ? ", radio " : "", src, encrypted ? ", encrypted" : "");
    else snprintf(ds_call, sizeof ds_call, "%s%s%s%s%s%s", proto, tg[0] ? ", talkgroup " : "", tg, src[0] ? ", radio " : "", src,
                  encrypted ? ", encrypted" : "");
    /* encrypted calls are ignored entirely: not followed, not announced */
    (void)last_call; (void)last_t;
}

static DWORD WINAPI ds_info_reader(LPVOID p)
{
    HANDLE h = (HANDLE)p;
    char buf[2048], line[2048];
    int n = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof buf, &got, NULL) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || buf[i] == '\r' || n == (int)sizeof line - 1) {
                line[n] = 0;
                if (n) {
                    ds_line(line);
                    if (g_listen_secs) {                 /* --listen test runs keep the raw output */
                        char path[MAX_PATH]; snprintf(path, sizeof path, "%s", dec_log_path);
                        strcpy(strrchr(path, '\\') + 1, "dsd_raw.log");
                        FILE *fp = fopen(path, "a"); if (fp) { fprintf(fp, "%s\n", line); fclose(fp); }
                    }
                }
                n = 0;
            }
            else line[n++] = buf[i];
        }
    }
    CloseHandle(h);
    return 0;
}

static void ds_stop(void)
{
    if (!ds_proc) return;
    CloseHandle(ds_in); ds_in = NULL;
    if (WaitForSingleObject(ds_proc, 500) != WAIT_OBJECT_0) TerminateProcess(ds_proc, 0);
    CloseHandle(ds_proc); ds_proc = NULL;
}

/* ---------- trunk following: dsd-neo steers the tuner over rigctl (TCP 4532) ---------- */
#define RIGCTL_PORT 4532
static volatile int g_trunk;               /* T: follow the trunked system */
static volatile double g_trunk_cc;         /* the control channel we started from */
static volatile int g_rig_moves;

static void rig_reply(SOCKET s, const char *text) { send(s, text, (int)strlen(text), 0); }

/* minimal Hamlib rigctl: F/set_freq, f/get_freq, M/set_mode, m/get_mode, everything else OK */
static DWORD WINAPI rigctl_thread(LPVOID p)
{
    (void)p;
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(RIGCTL_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL yes = TRUE; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 2)) return 0;
    while (!g_stop) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        char buf[512], line[256]; int n = 0;
        for (;;) {
            int got = recv(c, buf, sizeof buf, 0);
            if (got <= 0) break;
            for (int i = 0; i < got; i++) {
                if (buf[i] != '\n' && n < (int)sizeof line - 1) { if (buf[i] != '\r') line[n++] = buf[i]; continue; }
                line[n] = 0; n = 0;
                char *cmd = line; while (*cmd == '+' || *cmd == ' ') cmd++;
                char out[64];
                if ((cmd[0] == 'F' && (cmd[1] == ' ' || !cmd[1])) || !strncmp(cmd, "\\set_freq", 9)) {
                    double hz = atof(cmd[0] == 'F' ? cmd + 1 : cmd + 9);
                    if (hz > F_MIN && hz < F_MAX && g_trunk) {
                        g_station = hz; g_rig_moves++;
                        PostMessageW(g_wnd, WM_APP + 3, 0, 0);
                    }
                    rig_reply(c, "RPRT 0\n");
                } else if ((cmd[0] == 'f' && !cmd[1]) || !strncmp(cmd, "\\get_freq", 9)) {
                    snprintf(out, sizeof out, "%.0f\n", g_station); rig_reply(c, out);
                } else if ((cmd[0] == 'm' && !cmd[1]) || !strncmp(cmd, "\\get_mode", 9)) {
                    rig_reply(c, "FM\n12500\n");
                } else if (cmd[0] == 'q' || cmd[0] == 'Q') {
                    break;
                } else if (cmd[0]) {
                    rig_reply(c, "RPRT 0\n");                /* set_mode, levels, etc: accept */
                }
            }
        }
        closesocket(c);
    }
    closesocket(ls);
    return 0;
}

/* ---------- rtl_tcp server for trunk following ----------
   Simulcast P25 needs dsd-neo's own CQPSK demodulator, which takes raw IQ.
   While trunking, dsd-neo connects here as an rtl_tcp client: we stream the dongle's raw cu8
   to it and apply its commands (frequency, sample rate, gain) to the dongle. Our own audio
   chain stands aside; only dsd-neo's decoded voice plays. */
#define RTLTCP_PORT 1234
static volatile SOCKET g_tcp_client = INVALID_SOCKET;
static unsigned char *tcp_ring; static volatile LONG tcp_w, tcp_r;
#define TCP_RING (16u << 20)
static HANDLE tcp_event;
static volatile int g_restore_normal;       /* after trunking: reconfigure the dongle for us */

static void tcp_push(const unsigned char *buf, uint32_t len)   /* from the USB callback */
{
    if (g_tcp_client == INVALID_SOCKET) return;
    uint32_t w = (uint32_t)tcp_w, first = len < TCP_RING - w ? len : TCP_RING - w;
    memcpy(tcp_ring + w, buf, first);
    memcpy(tcp_ring, buf + first, len - first);
    tcp_w = (LONG)((w + len) % TCP_RING);
    SetEvent(tcp_event);
}

static DWORD WINAPI tcp_sender(LPVOID p)
{
    SOCKET c = (SOCKET)(UINT_PTR)p;
    while (g_tcp_client == c && !g_stop) {
        uint32_t avail = ((uint32_t)tcp_w - (uint32_t)tcp_r) % TCP_RING;
        if (!avail) { WaitForSingleObject(tcp_event, 50); continue; }
        uint32_t r = (uint32_t)tcp_r, n = avail < TCP_RING - r ? avail : TCP_RING - r;
        if (n > 65536) n = 65536;
        int sent = send(c, (const char *)tcp_ring + r, (int)n, 0);
        if (sent <= 0) break;
        tcp_r = (LONG)((r + sent) % TCP_RING);
    }
    return 0;
}

static DWORD WINAPI rtltcp_thread(LPVOID p)
{
    (void)p;
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(RTLTCP_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL yes = TRUE; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) return 0;
    while (!g_stop) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        if (!g_trunk || !dev) { closesocket(c); continue; }
        int bufsz = 4 << 20; setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsz, sizeof bufsz);
        unsigned char hdr[12] = { 'R', 'T', 'L', '0', 0, 0, 0, 6, 0, 0, 0, (unsigned char)ngains };   /* tuner type 6 = R828D */
        send(c, (const char *)hdr, 12, 0);
        tcp_r = tcp_w;
        /* fixed gain instead of the client's AGC: measured 2026-10-03 on a simulcast P25 system, the R828D's
           auto gain left 23% of voice frames needing repair, 40.2 dB only 4.5%. SDR_TRUNK_GAIN overrides
           (dB, or "auto" to let dsd-neo decide). */
        const char *fg = getenv("SDR_TRUNK_GAIN");
        int fixed_gain = fg && !_stricmp(fg, "auto") ? 0 : fg && atof(fg) > 0 ? (int)(atof(fg) * 10 + 0.5) : 402;
        if (fixed_gain) { rtlsdr_set_tuner_gain_mode(dev, 1); rtlsdr_set_tuner_gain(dev, fixed_gain); }
        g_tcp_client = c;
        CloseHandle(CreateThread(NULL, 0, tcp_sender, (LPVOID)(UINT_PTR)c, 0, NULL));
        unsigned char cmd[5]; int have = 0;
        for (;;) {
            int got = recv(c, (char *)cmd + have, 5 - have, 0);
            if (got <= 0) break;
            have += got;
            if (have < 5) continue;
            have = 0;
            uint32_t v = ((uint32_t)cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
            rtlsdr_dev_t *d = dev;
            if (!d) continue;
            switch (cmd[0]) {
            case 0x01:                                         /* dsd-neo tunes a quarter sample rate above the channel */
                rtlsdr_set_center_freq(d, v); g_tcp_center = v;
                g_station = floor((v - g_tcp_rate / 4) / 6250 + 0.5) * 6250;
                PostMessageW(g_wnd, WM_APP + 3, 0, 0); break;
            case 0x02: rtlsdr_set_sample_rate(d, v); g_tcp_rate = v; break;
            case 0x03: if (!fixed_gain) rtlsdr_set_tuner_gain_mode(d, (int)v); break;
            case 0x04: if (!fixed_gain) rtlsdr_set_tuner_gain(d, (int)v); break;
            case 0x05: rtlsdr_set_freq_correction(d, (int)v); break;
            case 0x08: if (!fixed_gain) rtlsdr_set_agc_mode(d, (int)v); break;
            case 0x0d: if (!fixed_gain && (int)v < ngains) rtlsdr_set_tuner_gain(d, gains[v]); break;
            default: break;                                    /* others don't apply to the V4 */
            }
        }
        g_tcp_client = INVALID_SOCKET;
        closesocket(c);
        g_restore_normal = 1;                                  /* put the dongle back for our own chain */
    }
    return 0;
}

/* dsd-neo group list from talkgroups.csv: names for every talkgroup, and the ones known to be
   encrypted are Blocked, so their grants are skipped without moving the radio at all */
static void write_dsd_groups(char *out_path, size_t n)
{
    char src[MAX_PATH], line[300];
    snprintf(src, sizeof src, "%s", saved_path); strcpy(strrchr(src, '\\') + 1, "talkgroups.csv");
    snprintf(out_path, n, "%s", saved_path); strcpy(strrchr(out_path, '\\') + 1, "dsd_groups.csv");
    FILE *in = fopen(src, "r"), *out = fopen(out_path, "w");
    if (!out) { if (in) fclose(in); out_path[0] = 0; return; }
    fprintf(out, "DEC,Mode(A- Allow; B - Block; DE - Digital Enc),Name of Group,Tag (do not delete this line or won't import properly)\n");
    static long done[MAX_TG]; int ndone = 0;
    while (in && fgets(line, sizeof line, in)) {
        if (line[0] == '#') continue;
        char *f[4] = {0}; int k = 0;
        for (char *t = strtok(line, ","); t && k < 4; t = strtok(NULL, ",")) f[k++] = trim(t);
        if (k < 3 || atol(f[1]) <= 0) continue;
        long tg = atol(f[1]); int dup = 0;
        for (int i = 0; i < ndone; i++) if (done[i] == tg) dup = 1;
        if (dup || ndone >= MAX_TG) continue;
        done[ndone++] = tg;
        int enc = k == 4 && !_stricmp(f[3], "yes");
        fprintf(out, "%ld,%s,%s,%s\n", tg, enc ? "B" : "A", f[2], f[0]);
    }
    if (in) fclose(in);
    fclose(out);
}

static void ds_start(void)
{
    char exe[MAX_PATH], cmd[900], groups[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(strrchr(exe, '\\') + 1, "dsd-neo.exe");
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return;
    const char *extra = getenv("SDR_DSD_EXTRA");           /* testing: extra dsd-neo options */
    if (g_trunk) {
        /* --enc-lockout: an encrypted call is recognised from its grant/first frame and skipped */
        write_dsd_groups(groups, sizeof groups);
        char gopt[MAX_PATH + 8] = "";
        if (groups[0]) snprintf(gopt, sizeof gopt, "-G \"%s\"", groups);
        char copt[MAX_PATH + 8] = "";
        int have_map = 0;
        for (int i = 1; i <= CP_MAX_LSN; i++) if (cp_freq[i] > 0) have_map = 1;
        if (g_trunk_dmr && have_map) snprintf(copt, sizeof copt, "-C \"%s\"", cp_map_path);
        snprintf(cmd, sizeof cmd, "\"%s\" %s -T -i rtltcp:127.0.0.1:%d:%.4fM -o - --stdout-mono --enc-lockout %s %s %s", exe,
                 g_trunk_dmr ? "-fs" : "-ft", RTLTCP_PORT, g_trunk_cc / 1e6, gopt, copt, extra ? extra : "");
    }
    else snprintf(cmd, sizeof cmd, "\"%s\" -fa -i - -s 48000 -o - --stdout-mono", exe);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r, in_w, out_r, out_w, err_r, err_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 1 << 20)) return;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) { CloseHandle(in_r); CloseHandle(in_w); return; }
    if (!CreatePipe(&err_r, &err_w, &sa, 0)) { CloseHandle(in_r); CloseHandle(in_w); CloseHandle(out_r); CloseHandle(out_w); return; }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = err_w;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(in_r); CloseHandle(out_w); CloseHandle(err_w);
    if (!ok) { CloseHandle(in_w); CloseHandle(out_r); CloseHandle(err_r); return; }
    CloseHandle(pi.hThread);
    ds_proc = pi.hProcess; ds_in = in_w; ds_n = 0;
    CloseHandle(CreateThread(NULL, 0, ds_voice_reader, out_r, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, ds_info_reader, err_r, 0, NULL));
}

/* each 48012 Hz discriminator sample in narrow FM (48012 vs 48000 is close enough) */
static void ds_feed(float d, int mode)
{
    if (g_trunk) return;                                   /* trunking: dsd-neo reads IQ over rtl_tcp */
    if (!g_decode_on || mode != M_NFM) { if (ds_proc) ds_stop(); return; }
    if (!ds_proc) ds_start();
    if (!ds_proc) return;
    float v = d * 20000.f;
    ds_buf[ds_n++] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    if (ds_n == (int)(sizeof ds_buf / sizeof ds_buf[0])) {
        DWORD w;
        if (!WriteFile(ds_in, ds_buf, sizeof ds_buf, &w, NULL)) ds_stop();
        ds_n = 0;
    }
}

/* ---------- ADS-B: aircraft on 1090 MHz ----------
   When decoders are on and the dial is near 1090 MHz, the radio thread reopens the dongle at
   2 MS/s centred on 1090 and every block comes here instead of the audio chain. Mode S frames:
   preamble pulses at 0, 1, 3.5, 4.5 us; then 112 (or 56) bits, 1 us each, Manchester coded. */
#define ADSB_FS 2000000
/* where the listener is: from Windows Location when it's allowed, else the last known spot
   (location.txt beside presets.txt, "lat lon", which can also be typed by hand). No guessing. */
static volatile double g_home_lat, g_home_lon;
static volatile int g_have_home;
#define HOME_LAT g_home_lat
#define HOME_LON g_home_lon

/* ---------- driver: Windows' own WinUSB on the dongle's interface 0 ----------
   RTL-SDR dongles (0BDA:2838 / 0BDA:2832) arrive with no usable driver. This binds Microsoft's
   in-box, signed winusb.inf to the radio interface (the IR interface MI_01 is left alone) and adds
   the DeviceInterfaceGUIDs value WinUSB needs for libusb to find it. Needs administrator rights:
   the app re-runs itself elevated with --install-driver. */
static const wchar_t *RTL_IDS[] = { L"USB\\VID_0BDA&PID_2838", L"USB\\VID_0BDA&PID_2832" };
#define RTL_IF_GUID L"{6C4E8B21-3F5A-4C2E-9B7D-52A1E0D3F801}"

/* finds the radio's interface; returns the device set (caller destroys) or INVALID_HANDLE_VALUE */
static HDEVINFO rtl_find(SP_DEVINFO_DATA *out)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return set;
    SP_DEVINFO_DATA dev = { sizeof dev };
    int best = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t id[512];
        if (!SetupDiGetDeviceInstanceIdW(set, &dev, id, 512, NULL)) continue;
        for (int k = 0; k < 2; k++) {
            size_t L = wcslen(RTL_IDS[k]);
            if (_wcsnicmp(id, RTL_IDS[k], L)) continue;
            int score = !_wcsnicmp(id + L, L"&MI_00", 6) ? 2 : (id[L] == L'\\' ? 1 : 0);   /* interface 0, or a non-composite device */
            if (score > best) { best = score; *out = dev; }
        }
    }
    if (!best) { SetupDiDestroyDeviceInfoList(set); return INVALID_HANDLE_VALUE; }
    return set;
}

/* 0: no radio plugged in, 1: driver fine, 2: radio needs the driver */
static int driver_state(void)
{
    SP_DEVINFO_DATA dev;
    HDEVINFO set = rtl_find(&dev);
    if (set == INVALID_HANDLE_VALUE) return 0;
    wchar_t svc[64] = L"";
    SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_SERVICE, NULL, (BYTE *)svc, sizeof svc, NULL);
    int ok = !_wcsicmp(svc, L"WinUSB");
    if (ok) {
        HKEY k = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        DWORD type, size = 0;
        ok = k != INVALID_HANDLE_VALUE && RegQueryValueExW(k, L"DeviceInterfaceGUIDs", NULL, &type, NULL, &size) == ERROR_SUCCESS;
        if (k != INVALID_HANDLE_VALUE) RegCloseKey(k);
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok ? 1 : 2;
}

/* elevated: bind WinUSB and restart the device. Returns 0 on success. */
static int install_driver(void)
{
    SP_DEVINFO_DATA dev;
    HDEVINFO set = rtl_find(&dev);
    if (set == INVALID_HANDLE_VALUE) return 1;
    int rc = 2;
    /* the interface GUID first, so WinUSB registers it when it starts */
    HKEY k = SetupDiCreateDevRegKeyW(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, NULL, NULL);
    if (k != INVALID_HANDLE_VALUE) {
        static const wchar_t guids[] = RTL_IF_GUID L"\0";
        RegSetValueExW(k, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE *)guids, sizeof guids);
        RegCloseKey(k);
    }
    wchar_t svc[64] = L"";
    SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_SERVICE, NULL, (BYTE *)svc, sizeof svc, NULL);
    if (_wcsicmp(svc, L"WinUSB")) {
        SP_DEVINSTALL_PARAMS_W p = { sizeof p };
        SetupDiGetDeviceInstallParamsW(set, &dev, &p);
        p.Flags |= DI_ENUMSINGLEINF; p.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        GetWindowsDirectoryW(p.DriverPath, MAX_PATH); wcscat_s(p.DriverPath, MAX_PATH, L"\\INF\\winusb.inf");
        SetupDiSetDeviceInstallParamsW(set, &dev, &p);
        if (SetupDiBuildDriverInfoList(set, &dev, SPDIT_CLASSDRIVER)) {
            SP_DRVINFO_DATA_V2_W drv = { sizeof drv };
            for (DWORD i = 0; SetupDiEnumDriverInfoW(set, &dev, SPDIT_CLASSDRIVER, i, &drv); i++) {
                BYTE buf[4096]; SP_DRVINFO_DETAIL_DATA_W *dt = (SP_DRVINFO_DETAIL_DATA_W *)buf; dt->cbSize = sizeof *dt;
                if (!SetupDiGetDriverInfoDetailW(set, &dev, &drv, dt, sizeof buf, NULL)) continue;
                if (_wcsicmp(dt->HardwareID, L"USB\\MS_COMP_WINUSB")) continue;
                BOOL reboot = FALSE;
                rc = DiInstallDevice(NULL, set, &dev, &drv, 0, &reboot) ? 0 : 3;
                break;
            }
        }
    } else rc = 0;
    /* restart it so WinUSB picks up the interface GUID */
    SP_PROPCHANGE_PARAMS pc = { { sizeof(SP_CLASSINSTALL_HEADER), DIF_PROPERTYCHANGE }, DICS_PROPCHANGE, DICS_FLAG_GLOBAL, 0 };
    if (SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof pc))
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev);
    SetupDiDestroyDeviceInfoList(set);
    return rc;
}

/* ask once, then run ourselves elevated; Windows shows its usual permission prompt */
static volatile int g_driver_asked;
static void offer_driver(HWND h)
{
    if (g_driver_asked) return;
    g_driver_asked = 1;
    if (MessageBoxW(h, L"Your radio is plugged in but doesn't have its driver yet. Install it now? "
                       L"Windows will ask for permission.", L"SDR tuner", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    SHELLEXECUTEINFOW se = { sizeof se };
    se.fMask = SEE_MASK_NOCLOSEPROCESS; se.lpVerb = L"runas"; se.lpFile = exe; se.lpParameters = L"--install-driver"; se.nShow = SW_HIDE;
    if (!ShellExecuteExW(&se)) { say("Driver not installed"); return; }
    WaitForSingleObject(se.hProcess, 60000);
    DWORD rc = 1; GetExitCodeProcess(se.hProcess, &rc); CloseHandle(se.hProcess);
    say(rc == 0 ? "Driver installed. Connecting." : "The driver install didn't work. Try unplugging the radio and plugging it back in.");
}

/* ---------- updates: newest release on GitHub, downloaded and swapped in at start-up ---------- */
static int http_get(const wchar_t *url, char **out, DWORD *out_n)
{
    URL_COMPONENTSW uc = { sizeof uc };
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    HINTERNET s = WinHttpOpen(L"AccessibleSDR/" WSTR(TUNER_VERSION), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) return 0;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    int ok = 0; char *buf = NULL; DWORD n = 0, cap = 0;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1, NULL, 0, 0, 0)
          && WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
        if (status == 200) {
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                if (n + avail + 1 > cap) { cap = (n + avail + 1) * 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
                DWORD got = 0;
                if (!WinHttpReadData(r, buf + n, avail, &got) || !got) break;
                n += got;
            }
            ok = buf != NULL;
            if (ok) buf[n] = 0;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    if (!ok) { free(buf); return 0; }
    *out = buf; *out_n = n;
    return 1;
}

static int version_newer(const char *a, const char *b)   /* is a newer than b? "1.2.3" */
{
    int x[3] = {0}, y[3] = {0};
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]); sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
    return 0;
}

static DWORD WINAPI update_thread(LPVOID p)
{
    (void)p;
    wchar_t dir[MAX_PATH]; GetModuleFileNameW(NULL, dir, MAX_PATH); *wcsrchr(dir, L'\\') = 0;
    {   /* leftovers from the last update */
        wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%s\\*.old", dir);
        WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW(pat, &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do { wchar_t f[MAX_PATH]; swprintf(f, MAX_PATH, L"%s\\%s", dir, fd.cFileName); DeleteFileW(f); } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
    }
    if (GetEnvironmentVariableA("SDR_NO_UPDATE", NULL, 0)) return 0;
    char *json; DWORD n;
    if (!http_get(L"https://api.github.com/repos/" WSTR(UPDATE_REPO) L"/releases/latest", &json, &n)) return 0;
    char tag[32] = "", url[512] = "";
    char *t = strstr(json, "\"tag_name\"");
    if (t && (t = strchr(t + 10, '"'))) sscanf(t + 1, "%31[^\"]", tag);
    for (char *u = strstr(json, "\"browser_download_url\""); u; u = strstr(u + 1, "\"browser_download_url\"")) {
        char cand[512] = ""; char *q = strchr(u + 22, '"');
        if (q) sscanf(q + 1, "%511[^\"]", cand);
        size_t L = strlen(cand), A = strlen(UPDATE_ASSET);
        if (L > A && !strcmp(cand + L - A, UPDATE_ASSET)) { strcpy(url, cand); break; }
    }
    free(json);
    const char *ver = tag[0] == 'v' ? tag + 1 : tag;
    if (!tag[0] || !url[0] || !version_newer(ver, TUNER_VERSION)) return 0;
    char msg[128]; snprintf(msg, sizeof msg, "Downloading update, version %s", ver); say(msg);
    wchar_t wurl[512]; MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512);
    char *zip;
    if (!http_get(wurl, &zip, &n)) { say("The update download failed. It will try again next time."); return 0; }
    wchar_t tmp[MAX_PATH], zpath[MAX_PATH], xdir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(zpath, MAX_PATH, L"%ssdr-update.zip", tmp); swprintf(xdir, MAX_PATH, L"%ssdr-update", tmp);
    HANDLE f = CreateFileW(zpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD w = 0; if (f != INVALID_HANDLE_VALUE) { WriteFile(f, zip, n, &w, NULL); CloseHandle(f); }
    free(zip);
    if (w != n) return 0;
    /* unpack with Windows' own tar (it reads zip) */
    wchar_t cmd[3 * MAX_PATH];
    swprintf(cmd, 3 * MAX_PATH, L"cmd.exe /c rmdir /s /q \"%s\" 2>nul & mkdir \"%s\" && tar -xf \"%s\" -C \"%s\"", xdir, xdir, zpath, xdir);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 0;
    WaitForSingleObject(pi.hProcess, 60000); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    /* swap files in: files in use (this exe, loaded DLLs) can be renamed aside, then replaced */
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%s\\*", xdir);
    WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW(pat, &fd);
    int moved = 0;
    if (fh == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t src[MAX_PATH], dst[MAX_PATH], old[MAX_PATH];
        swprintf(src, MAX_PATH, L"%s\\%s", xdir, fd.cFileName);
        swprintf(dst, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        swprintf(old, MAX_PATH, L"%s.old", dst);
        DeleteFileW(old);
        MoveFileW(dst, old);
        if (MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) moved++;
        else MoveFileW(old, dst);                           /* couldn't place it: put the old one back */
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    if (!moved) return 0;
    say("Updated. Restarting.");
    Sleep(1500);
    PostMessageW(g_wnd, WM_APP + 8, 0, 0);                  /* start the new copy and quit */
    return 0;
}

static DWORD WINAPI location_thread(LPVOID p)
{
    char path[MAX_PATH], line[128];
    snprintf(path, sizeof path, "%s", (const char *)p); strcpy(strrchr(path, '\\') + 1, "location.txt");
    FILE *fp = fopen(path, "r");
    if (fp) {
        if (fgets(line, sizeof line, fp)) {
            double la, lo;
            if (sscanf(line, "%lf %lf", &la, &lo) == 2 && fabs(la) <= 90 && fabs(lo) <= 180) { g_home_lat = la; g_home_lon = lo; g_have_home = 1; }
        }
        fclose(fp);
    }
    /* ask Windows (works only if Location is allowed for desktop apps) */
    const char *ps = "powershell.exe -NoProfile -NonInteractive -Command \"Add-Type -AssemblyName System.Device;"
        "$w=New-Object System.Device.Location.GeoCoordinateWatcher;[void]$w.TryStart($false,[TimeSpan]::FromSeconds(20));"
        "$c=$w.Position.Location;if(-not $c.IsUnknown){[string]::Format([Globalization.CultureInfo]::InvariantCulture,'{0} {1}',$c.Latitude,$c.Longitude)}\"";
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return 0;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = w; si.hStdError = w;
    PROCESS_INFORMATION pi; char cmd[1024]; snprintf(cmd, sizeof cmd, "%s", ps);
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(w);
        char out[256] = ""; DWORD got = 0, tot = 0;
        while (tot < sizeof out - 1 && ReadFile(r, out + tot, sizeof out - 1 - tot, &got, NULL) && got) tot += got;
        out[tot] = 0;
        WaitForSingleObject(pi.hProcess, 30000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        double la, lo;
        if (sscanf(out, "%lf %lf", &la, &lo) == 2 && fabs(la) <= 90 && fabs(lo) <= 180 && (la != 0 || lo != 0)) {
            g_home_lat = la; g_home_lon = lo; g_have_home = 1;
            fp = fopen(path, "w"); if (fp) { fprintf(fp, "%.5f %.5f\n", la, lo); fclose(fp); }
        }
    } else CloseHandle(w);
    CloseHandle(r);
    return 0;
}
static volatile int g_adsb;                /* radio is in aircraft mode */
static volatile int g_adsb_want, g_adsb_switch;
static uint16_t adsb_mag[256 * 256];       /* |i| + |q| lookup for cu8 pairs */
typedef struct {
    uint32_t icao; char call[9]; int alt, have_alt, announced;
    int cpr_lat[2], cpr_lon[2]; DWORD cpr_t[2]; double lat, lon; int have_pos;
    double speed, heading; int have_vel; DWORD seen;
} plane_t;
#define MAX_PLANES 200
static plane_t planes[MAX_PLANES];
static int nplanes;
static CRITICAL_SECTION adsb_lock;

static uint32_t modes_crc(const uint8_t *m, int bits)
{
    uint32_t crc = 0;
    for (int i = 0; i < bits - 24; i++) {
        if ((m[i / 8] >> (7 - i % 8)) & 1) crc ^= 0x800000;
        crc <<= 1;
        if (crc & 0x1000000) crc ^= 0x1FFF409;
    }
    crc &= 0xFFFFFF;
    uint32_t tx = ((uint32_t)m[bits / 8 - 3] << 16) | ((uint32_t)m[bits / 8 - 2] << 8) | m[bits / 8 - 1];
    return crc ^ tx;                                        /* 0 when the frame is intact */
}

static double cpr_mod(double a, double b) { double r = fmod(a, b); return r < 0 ? r + b : r; }
static int cpr_nl(double lat)
{
    if (fabs(lat) >= 87) return 1;
    double a = 1 - cos(PI / 30), b = cos(PI / 180 * fabs(lat));
    return (int)floor(2 * PI / acos(1 - a / (b * b)));
}

/* global CPR decode from one even and one odd airborne position */
static int cpr_decode(plane_t *p)
{
    double lat0 = p->cpr_lat[0] / 131072.0, lat1 = p->cpr_lat[1] / 131072.0;
    double lon0 = p->cpr_lon[0] / 131072.0, lon1 = p->cpr_lon[1] / 131072.0;
    int j = (int)floor(59 * lat0 - 60 * lat1 + 0.5);
    double rlat0 = 6.0 * (cpr_mod(j, 60) + lat0), rlat1 = 360.0 / 59 * (cpr_mod(j, 59) + lat1);
    if (rlat0 >= 270) rlat0 -= 360;
    if (rlat1 >= 270) rlat1 -= 360;
    if (cpr_nl(rlat0) != cpr_nl(rlat1)) return 0;
    int odd = p->cpr_t[1] > p->cpr_t[0];
    double lat = odd ? rlat1 : rlat0;
    int nl = cpr_nl(lat), ni = nl - odd; if (ni < 1) ni = 1;
    int m = (int)floor(lon0 * (nl - 1) - lon1 * nl + 0.5);
    double lon = (360.0 / ni) * (cpr_mod(m, ni) + (odd ? lon1 : lon0));
    if (lon >= 180) lon -= 360;
    p->lat = lat; p->lon = lon; p->have_pos = 1;
    return 1;
}

static const char *compass(double deg)
{
    static const char *n[8] = { "north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest" };
    return n[(int)floor(cpr_mod(deg + 22.5, 360) / 45)];
}

static void plane_say(plane_t *p)
{
    char out[300];
    size_t u = snprintf(out, sizeof out, "Aircraft %s", p->call[0] ? p->call : "unidentified");
    if (p->have_alt) u += snprintf(out + u, sizeof out - u, ", %d feet", p->alt);
    if (p->have_pos && g_have_home) {
        double dlat = (p->lat - HOME_LAT) * PI / 180, dlon = (p->lon - HOME_LON) * PI / 180;
        double a = sin(dlat / 2) * sin(dlat / 2) + cos(HOME_LAT * PI / 180) * cos(p->lat * PI / 180) * sin(dlon / 2) * sin(dlon / 2);
        double miles = 3958.8 * 2 * atan2(sqrt(a), sqrt(1 - a));
        double brg = atan2(sin(dlon) * cos(p->lat * PI / 180), cos(HOME_LAT * PI / 180) * sin(p->lat * PI / 180) -
                           sin(HOME_LAT * PI / 180) * cos(p->lat * PI / 180) * cos(dlon)) * 180 / PI;
        u += snprintf(out + u, sizeof out - u, ", %.0f miles %s", miles, compass(brg));
    }
    if (p->have_vel) u += snprintf(out + u, sizeof out - u, ", heading %s at %.0f knots", compass(p->heading), p->speed);
    dec_add(out);
}

static void adsb_frame(const uint8_t *m)
{
    int df = m[0] >> 3;
    if (df != 17 && df != 18) return;
    uint32_t icao = ((uint32_t)m[1] << 16) | (m[2] << 8) | m[3];
    int tc = m[4] >> 3;
    EnterCriticalSection(&adsb_lock);
    plane_t *p = NULL;
    for (int i = 0; i < nplanes; i++) if (planes[i].icao == icao) { p = &planes[i]; break; }
    if (!p) {
        if (nplanes == MAX_PLANES) {                       /* reuse the stalest */
            int old = 0; for (int i = 1; i < nplanes; i++) if (planes[i].seen < planes[old].seen) old = i;
            p = &planes[old];
        } else p = &planes[nplanes++];
        memset(p, 0, sizeof *p); p->icao = icao;
    }
    p->seen = GetTickCount();
    if (tc >= 1 && tc <= 4) {                              /* identification: 8 six-bit characters */
        static const char cs[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
        uint64_t v = 0; for (int i = 5; i < 11; i++) v = (v << 8) | m[i];
        char c[9]; int n = 0;
        for (int i = 0; i < 8; i++) { char ch = cs[(v >> (42 - 6 * i)) & 63]; if (ch != ' ' && ch != '#') c[n++] = ch; }
        c[n] = 0;
        if (n) snprintf(p->call, sizeof p->call, "%s", c);
    } else if (tc >= 9 && tc <= 18) {                      /* airborne position */
        int ac = ((m[5] << 4) | (m[6] >> 4)) & 0xFFF;
        if (ac & 0x10) { int n = ((ac & 0xFE0) >> 1) | (ac & 0xF); p->alt = n * 25 - 1000; p->have_alt = 1; }
        int odd = (m[6] >> 2) & 1;
        p->cpr_lat[odd] = ((m[6] & 3) << 15) | (m[7] << 7) | (m[8] >> 1);
        p->cpr_lon[odd] = ((m[8] & 1) << 16) | (m[9] << 8) | m[10];
        p->cpr_t[odd] = GetTickCount();
        if (p->cpr_t[0] && p->cpr_t[1] && (p->cpr_t[0] > p->cpr_t[1] ? p->cpr_t[0] - p->cpr_t[1] : p->cpr_t[1] - p->cpr_t[0]) < 10000)
            cpr_decode(p);
    } else if (tc == 19 && (m[4] & 7) == 1) {             /* ground speed and track */
        int ew = ((m[5] & 3) << 8) | m[6], ns = ((m[7] & 0x7F) << 3) | (m[8] >> 5);
        int ew_s = (m[5] >> 2) & 1, ns_s = (m[7] >> 7) & 1;
        if (ew && ns) {
            double vx = (ew - 1) * (ew_s ? -1 : 1), vy = (ns - 1) * (ns_s ? -1 : 1);
            p->speed = sqrt(vx * vx + vy * vy); p->heading = cpr_mod(atan2(vx, vy) * 180 / PI, 360); p->have_vel = 1;
        }
    }
    /* announce once, when it has a name and a position */
    if (!p->announced && p->call[0] && p->have_pos) { p->announced = 1; plane_say(p); }
    LeaveCriticalSection(&adsb_lock);
}

static void adsb_process(const unsigned char *buf, uint32_t len)
{
    static uint16_t mag[2 * 65536 + 300];
    static int carry;                                       /* samples kept from the last block */
    uint32_t n = len / 2;
    if (n > 2 * 65536) n = 2 * 65536;
    for (uint32_t i = 0; i < n; i++) mag[carry + i] = adsb_mag[(buf[2 * i] << 8) | buf[2 * i + 1]];
    int total = carry + (int)n;
    int i = 0;
    for (; i + 16 + 224 < total; i++) {
        const uint16_t *a = mag + i;
        /* preamble: high at 0, 2, 7, 9; low between */
        if (!(a[0] > a[1] && a[2] > a[1] && a[2] > a[3] && a[0] > a[3] && a[7] > a[6] && a[9] > a[8] &&
              a[4] < a[0] && a[5] < a[0] && a[6] < a[0] && a[8] < a[0])) continue;
        int high = (a[0] + a[2] + a[7] + a[9]) / 6;
        if (a[4] >= high || a[5] >= high || a[11] >= high || a[12] >= high || a[13] >= high || a[14] >= high) continue;
        uint8_t msg[14] = {0};
        for (int b = 0; b < 112; b++) {
            int x = a[16 + 2 * b], y = a[17 + 2 * b];
            if (x > y) msg[b / 8] |= 0x80 >> (b % 8);
        }
        int df = msg[0] >> 3;
        int bits = df >= 16 ? 112 : 56;
        if (modes_crc(msg, bits) == 0) { adsb_frame(msg); i += 16 + 2 * bits - 1; }
    }
    carry = total - i;                                      /* keep the unscanned tail */
    memmove(mag, mag + i, carry * sizeof(uint16_t));
}

/* ---------- generic decoder child: we write samples to its stdin, it prints lines ---------- */
typedef void (*line_fn)(char *line);
typedef struct { HANDLE proc, in; } child_t;
typedef struct { HANDLE h; line_fn fn; } reader_arg_t;

static DWORD WINAPI child_reader(LPVOID p)
{
    reader_arg_t a = *(reader_arg_t *)p; free(p);
    char buf[2048], line[4096]; int n = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(a.h, buf, sizeof buf, &got, NULL) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || buf[i] == '\r' || n == (int)sizeof line - 1) { line[n] = 0; if (n) a.fn(line); n = 0; }
            else line[n++] = buf[i];
        }
    }
    CloseHandle(a.h);
    return 0;
}

static int child_start(child_t *c, const char *exe_name, const char *args, line_fn fn)
{
    char exe[MAX_PATH], cmd[800];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(strrchr(exe, '\\') + 1, exe_name);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return 0;
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 1 << 20)) return 0;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) { CloseHandle(in_r); CloseHandle(in_w); return 0; }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = nul;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(in_r); CloseHandle(out_w); CloseHandle(nul);
    if (!ok) { CloseHandle(in_w); CloseHandle(out_r); return 0; }
    CloseHandle(pi.hThread);
    c->proc = pi.hProcess; c->in = in_w;
    reader_arg_t *a = malloc(sizeof *a); a->h = out_r; a->fn = fn;
    CloseHandle(CreateThread(NULL, 0, child_reader, a, 0, NULL));
    return 1;
}

static void child_stop(child_t *c)
{
    if (!c->proc) return;
    CloseHandle(c->in); c->in = NULL;
    if (WaitForSingleObject(c->proc, 500) != WAIT_OBJECT_0) TerminateProcess(c->proc, 0);
    CloseHandle(c->proc); c->proc = NULL;
}

static void child_write(child_t *c, const void *data, DWORD n)
{
    DWORD w;
    if (c->proc && !WriteFile(c->in, data, n, &w, NULL)) child_stop(c);
}

static void miles_dir(double lat, double lon, char *out, size_t n)
{
    if (!g_have_home) { snprintf(out, n, "position known, your location isn't set"); return; }
    double dlat = (lat - HOME_LAT) * PI / 180, dlon = (lon - HOME_LON) * PI / 180;
    double a = sin(dlat / 2) * sin(dlat / 2) + cos(HOME_LAT * PI / 180) * cos(lat * PI / 180) * sin(dlon / 2) * sin(dlon / 2);
    double brg = atan2(sin(dlon) * cos(lat * PI / 180), cos(HOME_LAT * PI / 180) * sin(lat * PI / 180) -
                       sin(HOME_LAT * PI / 180) * cos(lat * PI / 180) * cos(dlon)) * 180 / PI;
    snprintf(out, n, "%.0f miles %s", 3958.8 * 2 * atan2(sqrt(a), sqrt(1 - a)), compass(brg));
}

/* ---------- AIS (ships): IQ centred on 162.000 MHz, both channels at +-25 kHz ---------- */
static child_t ais;
static short ais_buf[8192]; static int ais_n;
static double ais_ri = 1, ais_rq = 0;

static double json_num(const char *j, const char *key, int *ok);
static void json_str(const char *j, const char *key, char *out, size_t n);

static void ais_line(char *line)
{
    if (line[0] != '{') return;
    int ok; char name[64], out[300], where[64];
    double mmsi = json_num(line, "mmsi", &ok);
    if (!ok) return;
    static struct { double mmsi; DWORD t; } seen[300]; static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i].mmsi == mmsi && GetTickCount() - seen[i].t < 300000) return;
    json_str(line, "shipname", name, sizeof name);
    double lat = json_num(line, "lat", &ok); int has_pos = ok;
    double lon = json_num(line, "lon", &ok); has_pos &= ok && fabs(lat) <= 90 && fabs(lon) <= 180;
    if (!name[0] && !has_pos) return;                       /* wait for something worth saying */
    if (nseen < 300) { seen[nseen].mmsi = mmsi; seen[nseen].t = GetTickCount(); nseen++; }
    size_t u = snprintf(out, sizeof out, "Ship %s", name[0] ? name : "");
    if (!name[0]) u += snprintf(out + u, sizeof out - u, "number %.0f", mmsi);
    if (has_pos) { miles_dir(lat, lon, where, sizeof where); u += snprintf(out + u, sizeof out - u, ", %s", where); }
    double sp = json_num(line, "speed", &ok); if (ok && sp > 0.5 && sp < 100) u += snprintf(out + u, sizeof out - u, ", %.0f knots", sp);
    dec_add(out);
}

static void ais_feed(float i, float q, double station)
{
    if (!ais.proc && !child_start(&ais, "AIS-catcher.exe", "-r CS16 . -s 248062 -o 5", ais_line)) return;
    /* shift so 162.000 MHz sits at 0 Hz, whichever AIS channel the dial is on */
    double w = -2 * PI * (162.0e6 - station) / FS_MPX;
    double a = i * ais_ri - q * ais_rq, b = i * ais_rq + q * ais_ri;
    double t = ais_ri * cos(w) - ais_rq * sin(w); ais_rq = ais_ri * sin(w) + ais_rq * cos(w); ais_ri = t;
    if (++ais_n % 4096 == 0) { double m = sqrt(ais_ri * ais_ri + ais_rq * ais_rq); ais_ri /= m; ais_rq /= m; }
    static int k;
    ais_buf[k++] = (short)(a * 16000); ais_buf[k++] = (short)(b * 16000);
    if (k == 8192) { child_write(&ais, ais_buf, sizeof ais_buf); k = 0; }
}

/* ---------- ACARS: AM audio at 48 kHz from the aircraft data frequencies ---------- */
static child_t acars;
static short acars_buf[4096]; static int acars_n;

static int is_acars(double f)
{
    static const double fr[] = { 129.125, 130.025, 130.425, 130.45, 131.125, 131.45, 131.475, 131.525, 131.55, 131.725,
                                 136.7, 136.75, 136.8, 136.85, 136.9, 136.925, 136.975 };
    for (size_t i = 0; i < sizeof fr / sizeof fr[0]; i++) if (fabs(f - fr[i] * 1e6) < 3000) return 1;
    return 0;
}

static void acars_line(char *line)
{
    if (line[0] != '{') return;
    char flight[32], tail[32], text[220], out[400];
    json_str(line, "flight", flight, sizeof flight);
    json_str(line, "tail", tail, sizeof tail);
    json_str(line, "text", text, sizeof text);
    if (!text[0]) return;                                   /* link tests and acknowledgements: nothing to say */
    size_t u = snprintf(out, sizeof out, "Aircraft message");
    if (flight[0]) u += snprintf(out + u, sizeof out - u, ", flight %s", flight);
    if (tail[0]) u += snprintf(out + u, sizeof out - u, ", tail %s", tail);
    if (text[0]) u += snprintf(out + u, sizeof out - u, ": %s", text);
    dec_add(out);
}

static void acars_stop_if_running(void) { if (acars.proc) child_stop(&acars); }

static void acars_feed(float x)
{
    if (!acars.proc && !child_start(&acars, "acarsdec.exe", "--stdin --output json:file", acars_line)) return;
    float v = x * 16000.f;
    acars_buf[acars_n++] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    if (acars_n == 4096) { child_write(&acars, acars_buf, sizeof acars_buf); acars_n = 0; }
}

/* ---------- FT8: 15-second UTC slots of USB audio at 12 kHz, decoded by decode_ft8 ---------- */
static int is_ft8(double f)
{
    static const double fr[] = { 1.84, 3.573, 5.357, 7.074, 10.136, 14.074, 18.1, 21.074, 24.915, 28.074, 50.313, 144.174 };
    for (size_t i = 0; i < sizeof fr / sizeof fr[0]; i++) if (fabs(f - fr[i] * 1e6) < 3000) return 1;
    return 0;
}

static short ft8_buf[12000 * 16]; static int ft8_n, ft8_slot = -1, ft8_dec; static float ft8_acc;
static volatile LONG ft8_heard;

static void ft8_line(char *line)
{
    /* "000000 +13.0 +1.60 1291 ~  CQ R7IW LN35": keep the message after the ~ */
    char *m = strchr(line, '~');
    if (!m) return;
    m++; while (*m == ' ') m++;
    if (!*m) return;
    char out[200]; snprintf(out, sizeof out, "FT8: %s", m);
    int speak = g_decode_speak; g_decode_speak = 0;         /* logged and readable with R, not spoken */
    dec_add(out);
    g_decode_speak = speak;
    InterlockedIncrement(&ft8_heard);
}

static DWORD WINAPI ft8_decode_thread(LPVOID p)
{
    char *wav = (char *)p, args[MAX_PATH + 8];
    snprintf(args, sizeof args, "\"%s\"", wav);
    child_t c = {0};
    ft8_heard = 0;
    if (child_start(&c, "decode_ft8.exe", args, ft8_line)) {
        CloseHandle(c.in); c.in = NULL;
        WaitForSingleObject(c.proc, 20000);
        Sleep(200);                                         /* let the reader finish */
        CloseHandle(c.proc);
        if (ft8_heard) { char t[64]; snprintf(t, sizeof t, "FT8, %ld stations heard", ft8_heard); beep(6); say(t); }
    }
    free(wav);
    return 0;
}

static void ft8_write_wav(const char *path, const short *s, int n)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    unsigned char h[44] = { 'R','I','F','F' }; uint32_t v;
    v = 36 + n * 2; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); h[20] = 1; h[22] = 1;
    v = 12000; memcpy(h + 24, &v, 4); v = 24000; memcpy(h + 28, &v, 4); h[32] = 2; h[34] = 16;
    memcpy(h + 36, "data", 4); v = n * 2; memcpy(h + 40, &v, 4);
    fwrite(h, 1, 44, fp); fwrite(s, 2, n, fp); fclose(fp);
}

static void ft8_feed(float x)
{
    /* 48012 -> 12003 Hz: average 4 samples (FT8 lives below 3 kHz) */
    ft8_acc += x;
    if (++ft8_dec < 4) return;
    ft8_dec = 0;
    float v = ft8_acc / 4 * 12000.f; ft8_acc = 0;
    SYSTEMTIME t; GetSystemTime(&t);
    int slot = (t.wMinute * 60 + t.wSecond) / 15;
    if (slot != ft8_slot) {                                  /* a slot boundary: decode the last one */
        if (ft8_slot >= 0 && ft8_n > 12000 * 12) {
            char *path = malloc(MAX_PATH);
            snprintf(path, MAX_PATH, "%s", dec_log_path);
            strcpy(strrchr(path, '\\') + 1, "ft8_slot.wav");
            ft8_write_wav(path, ft8_buf, ft8_n);
            CloseHandle(CreateThread(NULL, 0, ft8_decode_thread, path, 0, NULL));
        }
        ft8_slot = slot; ft8_n = 0;
    }
    if (ft8_n < (int)(sizeof ft8_buf / sizeof ft8_buf[0])) ft8_buf[ft8_n++] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* ---------- rtl_433: wireless sensors in the ISM bands, fed 248 kHz IQ ---------- */
static HANDLE rt_proc, rt_in;
static short rt_buf[8192]; static int rt_n;

static int in_ism(double f)
{
    return (f >= 300e6 && f <= 320e6) || (f >= 344e6 && f <= 346e6) || (f >= 433.0e6 && f <= 434.9e6) ||
           (f >= 862e6 && f <= 870e6) || (f >= 902e6 && f <= 928e6);
}

static double json_num(const char *j, const char *key, int *ok)
{
    char k[64]; snprintf(k, sizeof k, "\"%s\" : ", key);
    const char *p = strstr(j, k);
    if (!p) { snprintf(k, sizeof k, "\"%s\":", key); p = strstr(j, k); }
    if (!p) { *ok = 0; return 0; }
    p += strlen(k);
    while (*p == ' ' || *p == '"') p++;
    *ok = (*p == '-' || isdigit((unsigned char)*p));
    return atof(p);
}

static void json_str(const char *j, const char *key, char *out, size_t n)
{
    char k[64]; snprintf(k, sizeof k, "\"%s\" : \"", key);
    const char *p = strstr(j, k);
    if (!p) { snprintf(k, sizeof k, "\"%s\":\"", key); p = strstr(j, k); }
    out[0] = 0;
    if (!p) return;
    p += strlen(k);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) { out[i++] = *p == '-' || *p == '_' ? ' ' : *p; p++; }
    out[i] = 0;
}

/* {"model":"Acurite-Tower","id":1234,"temperature_C":21.3,...} into a short sentence */
static void rt_line(char *line)
{
    if (line[0] != '{') return;
    char model[64], out[400];
    json_str(line, "model", model, sizeof model);
    if (!model[0]) return;
    int ok; double v;
    size_t used = snprintf(out, sizeof out, "%s", model);
    v = json_num(line, "id", &ok); if (ok) used += snprintf(out + used, sizeof out - used, " %.0f", v);
    v = json_num(line, "temperature_C", &ok);
    if (ok) used += snprintf(out + used, sizeof out - used, ", %.0f degrees", v * 9 / 5 + 32);
    else { v = json_num(line, "temperature_F", &ok); if (ok) used += snprintf(out + used, sizeof out - used, ", %.0f degrees", v); }
    v = json_num(line, "humidity", &ok); if (ok) used += snprintf(out + used, sizeof out - used, ", humidity %.0f percent", v);
    v = json_num(line, "pressure_kPa", &ok); if (ok) used += snprintf(out + used, sizeof out - used, ", tire %.0f p s i", v * 0.145038);
    v = json_num(line, "wind_avg_km_h", &ok); if (ok) used += snprintf(out + used, sizeof out - used, ", wind %.0f miles per hour", v * 0.621371);
    v = json_num(line, "rain_mm", &ok); if (ok) used += snprintf(out + used, sizeof out - used, ", rain total %.1f inches", v / 25.4);
    v = json_num(line, "battery_ok", &ok); if (ok && v == 0) used += snprintf(out + used, sizeof out - used, ", low battery");
    {
        char ev[64]; json_str(line, "event", ev, sizeof ev);
        if (!ev[0]) json_str(line, "button", ev, sizeof ev);
        if (ev[0]) used += snprintf(out + used, sizeof out - used, ", %s", ev);
    }
    static char last[400]; static DWORD last_t;   /* sensors repeat each packet 2-3 times */
    if (!strcmp(last, out) && GetTickCount() - last_t < 5000) return;
    snprintf(last, sizeof last, "%s", out); last_t = GetTickCount();
    dec_add(out);
}

static DWORD WINAPI rt_reader(LPVOID p)
{
    HANDLE h = (HANDLE)p;
    char buf[2048], line[4096];
    int n = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof buf, &got, NULL) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || n == (int)sizeof line - 1) { line[n] = 0; rt_line(line); n = 0; }
            else line[n++] = buf[i];
        }
    }
    CloseHandle(h);
    return 0;
}

static void rt_stop(void)
{
    if (!rt_proc) return;
    CloseHandle(rt_in); rt_in = NULL;
    if (WaitForSingleObject(rt_proc, 500) != WAIT_OBJECT_0) TerminateProcess(rt_proc, 0);
    CloseHandle(rt_proc); rt_proc = NULL;
}

static void rt_start(void)
{
    char exe[MAX_PATH], cmd[600];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(strrchr(exe, '\\') + 1, "rtl_433.exe");
    snprintf(cmd, sizeof cmd, "\"%s\" -r cs16:- -s 248062 -F json -M level", exe);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 1 << 20)) return;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) { CloseHandle(in_r); CloseHandle(in_w); return; }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = nul;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(in_r); CloseHandle(out_w); CloseHandle(nul);
    if (!ok) { CloseHandle(in_w); CloseHandle(out_r); return; }
    CloseHandle(pi.hThread);
    rt_proc = pi.hProcess; rt_in = in_w; rt_n = 0;
    CloseHandle(CreateThread(NULL, 0, rt_reader, out_r, 0, NULL));
}

/* one complex sample at 248062.5 Hz, the station at 0 Hz */
static void rt_feed(float i, float q)
{
    if (!rt_proc) rt_start();
    if (!rt_proc) return;
    rt_buf[rt_n++] = (short)(i * 16000); rt_buf[rt_n++] = (short)(q * 16000);
    if (rt_n == (int)(sizeof rt_buf / sizeof rt_buf[0])) {
        DWORD w;
        if (!WriteFile(rt_in, rt_buf, sizeof rt_buf, &w, NULL)) rt_stop();
        rt_n = 0;
    }
}

/* glide: a tap moves at once; holding glides at one constant speed for the mode, like turning
   a dial (Shift: 10x slower, Ctrl: 10x faster). It stops at the category's edge. */
static volatile int g_fast;
static void advance_dial(double dt)
{
    int dir = g_dir;
    if (!dir) return;
    double held = (now() - g_hold_start) / g_qpf;
    if (held < 0.25) return;
    double tap, fine, glide, gmax;
    mode_params(g_mode, g_station, &tap, &fine, &glide, &gmax);
    double v = glide * (g_fine ? 0.1 : g_fast ? 10 : 1);
    double f = g_station + dir * v * dt;
    g_at_edge = f <= CAT_LO || f >= CAT_HI;
    if (f < CAT_LO) f = CAT_LO; if (f > CAT_HI) f = CAT_HI;
    g_station = f;
}

/* USB callback only copies into this ring; process_thread does the work, so a slow moment in
   the DSP can never stall USB and drop samples (HD Radio cannot survive gaps) */
#define RAW_RING (8u << 20)                /* bytes: ~2.8 s */
static unsigned char *raw_ring;
static volatile LONG raw_w, raw_r;
static HANDLE dsp_event;

static void CALLBACK_cb(unsigned char *buf, uint32_t len, void *ctx)
{
    (void)ctx;
    if (g_stop) { rtlsdr_cancel_async(dev); return; }
    tcp_push(buf, len);                                    /* trunk following: raw IQ to dsd-neo */
    uint32_t w = (uint32_t)raw_w, first = len < RAW_RING - w ? len : RAW_RING - w;
    memcpy(raw_ring + w, buf, first);
    memcpy(raw_ring, buf + first, len - first);
    raw_w = (LONG)((w + len) % RAW_RING);
    SetEvent(dsp_event);
}

static void process_block(unsigned char *buf, uint32_t len)
{
    static LONGLONG last;
    LONGLONG t = now();
    if (last) advance_dial((t - last) / g_qpf);
    last = t;

    /* trunk following owns the dongle through rtl_tcp; afterwards, set it back up for us */
    if (g_tcp_client != INVALID_SOCKET) { cp_watch(buf, len); return; }
    int want_adsb = g_decode_on && fabs(g_station - 1090e6) < 1.5e6;
    if (g_restore_normal && dev && !g_adsb_switch) {
        g_restore_normal = 0;
        g_adsb_want = want_adsb; g_adsb_switch = 1;
        rtlsdr_cancel_async(dev);
        return;
    }
    /* aircraft mode near 1090 MHz with decoders on: the radio thread reopens at 2 MS/s */
    if (want_adsb != g_adsb && !g_adsb_switch && dev) {
        g_adsb_want = want_adsb; g_adsb_switch = 1;
        PostMessageW(g_wnd, WM_APP + 5, want_adsb, 0);
        rtlsdr_cancel_async(dev);
        return;
    }
    if (g_adsb) { adsb_process(buf, len); return; }

    /* the hardware moved: once the samples tuned the old way have drained, use the new LO */
    if (g_retune_pending && g_switch_at && g_samples >= g_switch_at) {
        lo_used = g_lo_next;
        cur_off = g_station - lo_used;
        g_switch_at = 0;
        InterlockedExchange(&g_retune_pending, 0);
        fade_target = scan_active ? 0.f : 1.f;
    }
    if (scan_step(buf, len)) { g_samples += len / 2; return; }   /* scanning: no listening */
    double target = g_station - lo_used;
    /* while gliding, use the whole captured slice (both sides of the LO) so the hardware has to
       move half as often; parked, only move off the DC spike or the slice's soft edges */
    int out_of_window = g_dir ? (target < -630000 || target > 630000)
                              : (fabs(target) < 25000 || fabs(target) > 600000);
    if (!g_retune_pending && out_of_window) {
        /* leave the most room in the direction we're travelling */
        double place = g_dir > 0 ? -600000 : g_dir < 0 ? 600000 : OFF_MID;
        g_lo_next = g_station - place;
        fade_target = 0;
        InterlockedExchange(&g_retune_pending, 1);
        SetEvent(retune_event);
    }

    int mode = g_mode;
    int hd_on = g_hd_state != HD_OFF && mode == M_WFM;
    {   /* a new station: start RDS over (and announce its name once decoded) */
        static double rds_station;
        if (fabs(g_station - rds_station) > 20000 && !g_dir) { rds_station = g_station; rds_reset(); }
    }
    int ism = g_decode_on && in_ism(g_station) && !g_retune_pending;   /* wireless sensors */
    if (!ism && rt_proc) rt_stop();
    int ais_on = g_decode_on && fabs(g_station - 162.0e6) < 40000 && !g_retune_pending;   /* ships */
    if (!ais_on && ais.proc) child_stop(&ais);

    /* automatic gain: keep the 8-bit input in its useful middle. Too low and weak signals sink
       into the digitizer's coarseness; too high and strong ones clip. Every ~0.5 s, one step. */
    {
        static double agc_sq; static long agc_n, agc_clip; static int agc_blocks;
        for (uint32_t k = 0; k < len; k += 8) {
            int v = buf[k] - 128;
            agc_sq += v * v; agc_n++;
            if (buf[k] <= 1 || buf[k] >= 254) agc_clip++;
        }
        if (++agc_blocks >= 45 && ngains && !g_retune_pending && !g_dir) {   /* frozen while gliding */
            double rms = sqrt(agc_sq / agc_n), clip = (double)agc_clip / agc_n;
            int idx = g_gain_idx;
            if ((clip > 0.0005 || rms > 45) && idx > 0) idx--;
            else if (rms < 16 && clip == 0 && idx < ngains - 1) idx++;
            if (idx != g_gain_idx) { g_gain_idx = idx; SetEvent(retune_event); }
            agc_sq = 0; agc_n = 0; agc_clip = 0; agc_blocks = 0;
        }
    }
    static int sq_count;
    if (++sq_count >= 8) { sq_count = 0; if (!g_retune_pending) squelch_check(buf, cur_off, mode); }   /* ~90 ms */
    uint32_t nsamp = len / 2;
    for (uint32_t base = 0; base < nsamp; base += 64) {
        /* move the NCO smoothly towards the dial, 64 samples at a time */
        uint32_t end = base + 64 < nsamp ? base + 64 : nsamp;
        if (!g_retune_pending) cur_off += (target - cur_off) * (double)(end - base) / (nsamp - base);
        double w = -2 * PI * cur_off / FS_IN, si = cos(w), sq = sin(w);
        for (uint32_t s = base; s < end; s++) {
            float x = (buf[2 * s] - 127.5f) / 127.5f, y = (buf[2 * s + 1] - 127.5f) / 127.5f;
            float i = (float)(x * rot_i - y * rot_q), q = (float)(x * rot_q + y * rot_i);
            double ni = rot_i * si - rot_q * sq; rot_q = rot_i * sq + rot_q * si; rot_i = ni;

            if (hd_on) {                                     /* half-band /2 -> 744 kHz for nrsc5 */
                hb_i[hb_pos] = hb_i[hb_pos + HB_TAPS] = i;
                hb_q[hb_pos] = hb_q[hb_pos + HB_TAPS] = q;
                hb_pos = (hb_pos + 1) % HB_TAPS;
                if (++hb_phase == 2) {
                    hb_phase = 0;
                    float a = 0, b = 0;
                    const float *pi = hb_i + hb_pos, *pq = hb_q + hb_pos;
                    for (int k = 0; k < HB_TAPS; k++) { a += hb_h[k] * pi[k]; b += hb_h[k] * pq[k]; }
                    hd_ring[hd_w] = a; hd_ring[(hd_w + 1) & (HD_RING - 1)] = b;
                    hd_w = (hd_w + 2) & (HD_RING - 1);
                }
            }

            if (mode != M_WFM) { narrow_sample(i, q, mode); if (!ism && !ais_on) continue; }
            ci[cpos] = ci[cpos + CH_TAPS] = i;
            cq[cpos] = cq[cpos + CH_TAPS] = q;
            cpos = (cpos + 1) % CH_TAPS;
            if (++cphase < D1) continue;
            cphase = 0;
            float yi = 0, yq = 0;
            const float *pi = ci + cpos, *pq = cq + cpos;
            for (int k = 0; k < CH_TAPS; k++) { yi += ch_h[k] * pi[k]; yq += ch_h[k] * pq[k]; }
            if (ism) rt_feed(yi, yq);
            if (ais_on) ais_feed(yi, yq, g_station);
            if (mode != M_WFM) continue;
            float re = yi * prev_i + yq * prev_q, im = yq * prev_i - yi * prev_q;
            prev_i = yi; prev_q = yq;
            mpx_sample(atan2f(im, re) * (float)(FS_MPX / (2 * PI * 75000.0)));
        }
    }
    double mag = sqrt(rot_i * rot_i + rot_q * rot_q);
    rot_i /= mag; rot_q /= mag;
    g_samples += nsamp;
    if (hd_on) SetEvent(hd_event);
}

static DWORD WINAPI process_thread(LPVOID p)
{
    (void)p;
    static unsigned char chunk[32768];
    while (!g_stop) {
        uint32_t avail = ((uint32_t)raw_w - (uint32_t)raw_r) % RAW_RING;
        if (avail < sizeof chunk) { WaitForSingleObject(dsp_event, 50); continue; }
        uint32_t r = (uint32_t)raw_r, first = sizeof chunk < RAW_RING - r ? sizeof chunk : RAW_RING - r;
        memcpy(chunk, raw_ring + r, first);
        memcpy(chunk + first, raw_ring, sizeof chunk - first);
        raw_r = (LONG)((r + sizeof chunk) % RAW_RING);
        process_block(chunk, sizeof chunk);
    }
    return 0;
}

static volatile int g_gain_applied = -1;

static DWORD WINAPI retune_thread(LPVOID p)
{
    (void)p;
    while (!g_stop) {
        if (WaitForSingleObject(retune_event, 200) != WAIT_OBJECT_0) continue;
        rtlsdr_dev_t *d = dev;
        if (!d) continue;
        int g = (g_gain_idx >= 0 && ngains) ? gains[g_gain_idx] : gain_for(g_mode, g_station);
        if (g != g_gain_applied) { rtlsdr_set_tuner_gain(d, g); g_gain_applied = g; }
        if (!g_retune_pending || g_switch_at) continue;      /* gain-only wakeup */
        rtlsdr_set_center_freq(d, (uint32_t)(g_lo_next + 0.5));
        g_retunes++;
        /* samples already in the ring, plus a few USB transfers in flight, were captured at
           the old frequency */
        uint32_t backlog = ((uint32_t)raw_w - (uint32_t)raw_r) % RAW_RING;
        g_switch_at = g_samples + backlog / 2 + 3 * (USB_LEN / 2) + 8192;
    }
    return 0;
}

/* opens the dongle, streams until it disappears, and tries again every 2 seconds */
static DWORD WINAPI radio_thread(LPVOID p)
{
    (void)p;
    int announced_missing = 0, ever_connected = 0;
    while (!g_stop) {
        rtlsdr_dev_t *d = NULL;
        if (rtlsdr_get_device_count() == 0 || rtlsdr_open(&d, 0) < 0) {
            if (driver_state() == 2) { PostMessageW(g_wnd, WM_APP + 7, 0, 0); Sleep(3000); continue; }   /* needs its driver */
            if (!announced_missing) { say("Radio not found. Trying again every 2 seconds."); beep(4); announced_missing = 1; }
            for (int k = 0; k < 20 && !g_stop; k++) Sleep(100);
            continue;
        }
        rtlsdr_set_tuner_gain_mode(d, 1);
        ngains = rtlsdr_get_tuner_gains(d, gains);
        if (ngains < 0 || ngains > 64) ngains = 0;
        if (ngains && g_gain_idx < 0) g_gain_idx = nearest_gain(gain_for(g_mode, g_station));
        dev = d;
        if (announced_missing || ever_connected) { beep(3); say("Radio connected"); }
        announced_missing = 0; ever_connected = 1;
        for (;;) {
            if (g_adsb_want) {                                 /* aircraft: 2 MS/s on 1090, full gain */
                rtlsdr_set_sample_rate(d, ADSB_FS);
                rtlsdr_set_center_freq(d, 1090000000);
                rtlsdr_set_tuner_gain(d, ngains ? gains[ngains - 1] : 496);
                g_gain_applied = -1;
            } else {
                lo_used = g_lo_next = g_station - OFF_MID;
                cur_off = OFF_MID;
                rtlsdr_set_sample_rate(d, FS_IN);
                rtlsdr_set_center_freq(d, (uint32_t)(lo_used + 0.5));
                g_gain_applied = ngains ? gains[g_gain_idx] : gain_for(g_mode, g_station);
                rtlsdr_set_tuner_gain(d, g_gain_applied);
            }
            rtlsdr_reset_buffer(d);
            g_retune_pending = 0; g_switch_at = 0; fade = 0; fade_target = 1;
            raw_r = raw_w;
            g_adsb = g_adsb_want;
            rtlsdr_read_async(d, CALLBACK_cb, NULL, USB_BUFS, USB_LEN);
            if (g_stop || !g_adsb_switch) break;               /* stopped, or the dongle went away */
            g_adsb_switch = 0;                                 /* a mode switch: reconfigure and go on */
        }
        dev = NULL;
        rtlsdr_close(d);
        if (!g_stop) { beep(4); say("Radio disconnected. Trying again every 2 seconds."); announced_missing = 1; Sleep(2000); }
    }
    return 0;
}

/* ---------- window ---------- */
static int g_test_stops;                  /* --scantest <MHz> <stops> <squelch 0-3> */
static int g_glide_test;                  /* --glide: direction to "hold" */
static int g_explore, g_explore_new;      /* X: Explore running; signals added this run */
static int g_explore_test;                /* --explore: start Explore once the radio is up */
static int g_listen_secs;                 /* --listen <MHz> <mode> <seconds> */
static char g_test_log[MAX_PATH];

/* ---------- a small standard dialog asking for one line of text (NVDA reads it natively) ---------- */
static char *ib_out; static int ib_n;

static INT_PTR CALLBACK ib_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (msg == WM_INITDIALOG) { SetFocus(GetDlgItem(d, 101)); return FALSE; }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) { GetDlgItemTextA(d, 101, ib_out, ib_n); EndDialog(d, 1); return TRUE; }
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) { EndDialog(d, 0); return TRUE; }
    return FALSE;
}

static WORD *ib_align(WORD *p) { return (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }
static WORD *ib_str(WORD *p, const wchar_t *s) { while ((*p++ = *s++)) {} return p; }

static WORD *ib_item(WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t *text)
{
    p = ib_align(p);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE; it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;                              /* predefined class: 0x80 button, 0x81 edit, 0x82 static */
    p = ib_str(p, text);
    *p++ = 0;                                               /* no creation data */
    return p;
}

/* returns 1 and fills out[] if the user pressed Enter/OK */
static int input_box(HWND parent, const wchar_t *title, const wchar_t *label, char *out, int n)
{
    static WORD tpl[512];
    memset(tpl, 0, sizeof tpl);
    DLGTEMPLATE *dt = (DLGTEMPLATE *)tpl;
    dt->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt->cdit = 4; dt->cx = 200; dt->cy = 60;
    WORD *p = (WORD *)(dt + 1);
    *p++ = 0; *p++ = 0;                                     /* no menu, default class */
    p = ib_str(p, title);
    *p++ = 9; p = ib_str(p, L"Segoe UI");
    p = ib_item(p, SS_LEFT, 7, 7, 186, 10, 100, 0x82, label);
    p = ib_item(p, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 7, 19, 186, 14, 101, 0x81, L"");
    p = ib_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 90, 40, 50, 14, IDOK, 0x80, L"OK");
    p = ib_item(p, BS_PUSHBUTTON | WS_TABSTOP, 143, 40, 50, 14, IDCANCEL, 0x80, L"Cancel");
    ib_out = out; ib_n = n; out[0] = 0;
    return DialogBoxIndirectW(GetModuleHandleW(NULL), dt, parent, ib_proc) == 1;
}

/* "67202" (a US ZIP code) or "37.69 -97.33" / "37.69, -97.33" (latitude longitude) */
static int set_location(const char *text, char *said, size_t n)
{
    double la = 0, lo = 0; int ok = 0;
    char t[64]; snprintf(t, sizeof t, "%s", text);
    for (char *c = t; *c; c++) if (*c == ',' || *c == '\t' || *c == '\r' || *c == '\n') *c = ' ';
    char *s = t; while (*s == ' ') s++;
    for (size_t L = strlen(s); L && s[L - 1] == ' '; ) s[--L] = 0;          /* trim both ends */
    int zip = strlen(s) >= 5 && !strchr(s, '.');                            /* 67202 or 67202-1234 */
    for (int i = 0; zip && i < 5; i++) if (!isdigit((unsigned char)s[i])) zip = 0;
    if (zip && s[5] && s[5] != '-') zip = 0;
    if (zip) {
        char path[MAX_PATH], line[64]; snprintf(path, sizeof path, "%s", saved_path);
        strcpy(strrchr(path, '\\') + 1, "zipcodes.txt");
        FILE *fp = fopen(path, "r");
        while (fp && fgets(line, sizeof line, fp))
            if (!strncmp(line, s, 5) && sscanf(line + 5, "%lf %lf", &la, &lo) == 2) { ok = 1; break; }
        if (fp) fclose(fp);
        if (ok) snprintf(said, n, "Location set to ZIP code %.5s", s);
        else snprintf(said, n, "ZIP code %.5s not found", s);
    } else if (sscanf(s, "%lf %lf", &la, &lo) == 2 && fabs(la) <= 90 && fabs(lo) <= 180) {
        ok = 1; snprintf(said, n, "Location set");
    } else snprintf(said, n, "Type a 5 digit ZIP code, or latitude and longitude");
    if (!ok) return 0;
    g_home_lat = la; g_home_lon = lo; g_have_home = 1;
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s", saved_path);
    strcpy(strrchr(path, '\\') + 1, "location.txt");
    FILE *fp = fopen(path, "w"); if (fp) { fprintf(fp, "%.5f %.5f\n", la, lo); fclose(fp); }
    return 1;
}

static void update_title(void)
{
    char f[32], t[160];
    freq_text(f, sizeof f, g_station);
    snprintf(t, sizeof t, "SDR tuner, %s, %s, %s", cats[g_cat].name, f, mode_names[g_mode]);
    SetWindowTextA(g_wnd, t);
}

static void stop_hd(int announce)
{
    if (g_hd_state == HD_OFF) return;
    g_hd_state = HD_OFF; g_hd_audio = 0;
    SetEvent(hd_event);
    if (announce) say("Analog");
}

static void set_mode(int m)
{
    if (m != M_WFM) stop_hd(0);
    g_mode = m;
    am_avg = 1e-3f; agc_pk = 1e-3f;
    SetEvent(retune_event);                  /* the gain may differ per mode */
}

static void switch_cat(int dir)
{
    stop_hd(0);
    cats[g_cat].last = g_station; cats[g_cat].last_mode = g_mode;
    g_cat = (g_cat + dir + ncats) % ncats;
    g_station = cats[g_cat].last;
    set_mode(cats[g_cat].last_mode);
    if (ngains) g_gain_idx = nearest_gain(gain_for(g_mode, g_station));   /* a starting point; AGC refines */
    if (g_cat == saved_cat && nsaved == 0) say("Saved, empty. Shift Enter saves the current frequency.");
    else say(cats[g_cat].name);
    update_title();
}

/* the preset at this frequency, if the dial is within half a tap of one */
/* a category's stops: its own presets plus whatever Explore found inside its range */
static int stop_count(void)
{
    int n = cats[g_cat].count;
    if (g_cat != found_cat && g_cat != saved_cat)
        for (int i = 0; i < nfound; i++) if (found[i].hz >= CAT_LO && found[i].hz <= CAT_HI) n++;
    return n;
}

static const preset_t *preset_here(void)
{
    double tap, fine, glide, gmax;
    mode_params(g_mode, g_station, &tap, &fine, &glide, &gmax);
    const cat_t *c = &cats[g_cat];
    for (int i = 0; i < c->count; i++) {
        const preset_t *p = item(g_cat, i);
        if (fabs(p->hz - g_station) < tap / 2 + 1) return p;
    }
    if (g_cat != found_cat && g_cat != saved_cat)
        for (int i = 0; i < nfound; i++)
            if (fabs(found[i].hz - g_station) < tap / 2 + 1 && found[i].hz >= CAT_LO && found[i].hz <= CAT_HI) return &found[i];
    return NULL;
}

/* give an Explore result its name once we know it: "97.9" becomes "97.9, Heart" */
static void name_found_here(const char *what, double within)
{
    if (!what || !what[0]) return;
    for (int i = 0; i < nfound; i++) {
        if (fabs(found[i].hz - g_station) >= within || strstr(found[i].name, what)) continue;
        char f[32]; freq_text(f, sizeof f, found[i].hz);
        snprintf(found[i].name, sizeof found[i].name, "%s, %s", f, what);
        write_found();
        return;
    }
}

static void goto_preset(int dir)
{
    const cat_t *c = &cats[g_cat];
    const preset_t *best = NULL;
    /* nearest preset beyond the dial in this direction */
    for (int i = 0; i < c->count; i++) {
        const preset_t *p = item(g_cat, i);
        if ((dir > 0 ? p->hz > g_station + 1 : p->hz < g_station - 1) &&
            (!best || (dir > 0 ? p->hz < best->hz : p->hz > best->hz))) best = p;
    }
    if (!best) {
        if (!stop_count()) say("No stations here yet. Press X to find them.");
        else beep(0);
        return;
    }
    stop_hd(0);
    g_station = best->hz;
    if (best->mode != g_mode) set_mode(best->mode);
    say(best->name);
    update_title();
}

static void step(double hz)
{
    double tap, fine, glide, gmax;
    mode_params(g_mode, g_station, &tap, &fine, &glide, &gmax);
    double f = floor((g_station + hz) / fine + 0.5) * fine;
    if (f < CAT_LO || f > CAT_HI) { beep(0); f = f < CAT_LO ? CAT_LO : CAT_HI; }
    g_station = f;
}


static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    char f[32], t[300];
    double tap, fine, glide, gmax;
    switch (msg) {
    case WM_KEYDOWN: {
        int repeat = (lp >> 30) & 1, shift = GetKeyState(VK_SHIFT) < 0;
        mode_params(g_mode, g_station, &tap, &fine, &glide, &gmax);
        if (wp == 'S') {
            if (shift) { g_squelch = (g_squelch + 1) % 4; say(squelch_names[g_squelch]); return 0; }
            if (g_scan_req || scan_active) { g_scan_req = 0; say("Scan stopped"); }
            else { stop_hd(0); g_scan_req = 1; beep(5); say("Scanning"); }
            return 0;
        }
        if (wp == 'T') {
            if (!g_trunk) {
                if (g_mode != M_NFM) set_mode(M_NFM);
                g_decode_on = 1;
                g_trunk_cc = g_station; g_trunk = 1;
                /* a DMR Capacity Plus system (named so by Explore): learn and follow its channels */
                const preset_t *pp = NULL;                 /* what Explore found here, whatever the category */
                for (int i = 0; i < nfound; i++) if (fabs(found[i].hz - g_station) < 3200) pp = &found[i];
                if (!pp) pp = preset_here();
                const char *cc = pp ? strstr(pp->name, "color code ") : NULL;
                g_trunk_dmr = pp && strstr(pp->name, "Capacity Plus") && cc;
                if (g_trunk_dmr) cp_load_or_init(atoi(cc + 11), g_station);
                ds_stop(); ds_start();                   /* dsd-neo connects back over rtl_tcp */
                beep(5);
                say("Trunk following on. Calls will play as they happen.");
            } else {
                g_trunk = 0; ds_stop();
                g_station = g_trunk_cc;
                say("Trunk following off");
                update_title();
            }
            return 0;
        }
        /* tuning by hand ends trunk following */
        if (g_trunk && (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT || wp == VK_TAB || wp == 'M')) {
            g_trunk = 0; ds_stop(); say("Trunk following off");
        }
        /* any tuning key ends a scan */
        if ((g_scan_req || scan_active) && (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT ||
                                            wp == VK_TAB || wp == 'M' || wp == VK_RETURN || wp == VK_ESCAPE)) {
            g_scan_req = 0;
            if (g_explore) { g_explore = 0; g_explore_lap = 0; KillTimer(h, 5); }
            if (wp == VK_ESCAPE) { say("Scan stopped"); return 0; }
        }
        if (wp == VK_RETURN && shift) {                  /* save this frequency as a preset */
            int m = g_mode;
            for (int i = 0; i < nsaved; i++)
                if (fabs(saved[i].hz - g_station) < 1 && saved[i].mode == m) { say("Already saved"); return 0; }
            if (nsaved >= MAX_SAVED || saved_cat < 0) { say("Saved list is full"); return 0; }
            freq_text(f, sizeof f, g_station);
            preset_t *p = &saved[nsaved++];
            snprintf(p->name, sizeof p->name, "%s, %s", f, mode_names[m]);
            p->hz = g_station; p->mode = m;
            qsort(saved, nsaved, sizeof(preset_t), cmp_preset);
            cats[saved_cat].count = nsaved;
            write_saved();
            beep(3);
            snprintf(t, sizeof t, "Saved, %s", f); say(t);
            return 0;
        }
        if (wp == 'Z') {                                 /* your location: ZIP code or coordinates */
            char in[64];
            if (input_box(h, L"Your location", L"ZIP code, or latitude and longitude:", in, sizeof in)) {
                set_location(in, t, sizeof t); say(t);
            } else say("Location unchanged");
            return 0;
        }
        if (g_cat == found_cat && (wp == VK_UP || wp == VK_DOWN) && g_hd_state == HD_OFF) {
            goto_preset(wp == VK_UP ? 1 : -1);           /* Found here: arrows step through what was found */
            return 0;
        }
        if (g_cat == found_cat && wp == VK_ESCAPE && g_hd_state == HD_OFF && !g_scan_req && !scan_active) {
            for (int c = 0; c < ncats; c++)              /* Escape: back to the FM band */
                if (cats[c].mode == M_WFM && c != found_cat && c != saved_cat) {
                    cats[g_cat].last = g_station; cats[g_cat].last_mode = g_mode;
                    g_cat = c; g_station = cats[c].last; set_mode(M_WFM);
                    freq_text(f, sizeof f, g_station);
                    snprintf(t, sizeof t, "%s, %s", cats[c].name, f); say(t); update_title();
                    break;
                }
            return 0;
        }
        if (wp == 'X') {                                 /* Explore: find and identify everything in this category */
            if (g_explore) { g_explore = 0; g_explore_lap = 0; g_scan_req = 0; KillTimer(h, 5); say("Explore stopped"); return 0; }
            stop_hd(0); g_explore = 1; g_explore_new = 0; g_decode_on = 1;
            g_explore_lap = 1; g_explore_wrapped = 0; g_explore_origin = g_station;
            snprintf(t, sizeof t, "Exploring %s", cats[g_cat].name); beep(5); say(t);
            /* broadcast bands get a channel-by-channel survey; everything else the signal scanner */
            if (survey_build()) g_survey_req = 1;       /* bands with a real channel map */
            else g_scan_req = 1;                        /* anything else: the signal scanner */
            return 0;
        }
        if (wp == 'D') {
            if (shift) { g_decode_speak = !g_decode_speak; say(g_decode_speak ? "Speak decodes on" : "Speak decodes off"); }
            else { g_decode_on = !g_decode_on; say(g_decode_on ? "Decoders on" : "Decoders off"); }
            return 0;
        }
        if (wp == 'R') {                                 /* newest decode, then older ones */
            EnterCriticalSection(&dec_lock);
            LONG n = dec_count;
            if (!n) { LeaveCriticalSection(&dec_lock); say("Nothing decoded yet"); return 0; }
            int back = dec_read_pos;
            if (back >= n || back >= DEC_HIST) { back = 0; }
            snprintf(t, sizeof t, "%s", dec_hist[(n - 1 - back) % DEC_HIST]);
            dec_read_pos = back + 1;
            LeaveCriticalSection(&dec_lock);
            say(t);
            return 0;
        }
        if (wp == 'L') {
            if (shift) {
                if (g_cat != saved_cat) { say("Shift L clears all lockouts. Go to the Saved category first."); return 0; }
                nlock = 0; write_lockouts(); say("All lockouts cleared"); return 0;
            }
            if (is_locked_out(g_station)) { say("Already locked out"); return 0; }
            if (nlock >= MAX_LOCK) { say("Lockout list is full"); return 0; }
            lockouts[nlock] = g_station; nlock++;
            write_lockouts();
            beep(2);
            say("Locked out. The scanner will skip this.");
            return 0;
        }
        if (wp == VK_DELETE) {
            if (g_cat != saved_cat) { say("Only saved presets can be deleted. Saved is the last category."); return 0; }
            const preset_t *p = preset_here();
            if (!p) { say("Not on a saved preset"); return 0; }
            int i = (int)(p - saved);
            memmove(&saved[i], &saved[i + 1], (nsaved - i - 1) * sizeof(preset_t));
            nsaved--;
            cats[saved_cat].count = nsaved;
            write_saved();
            beep(2);
            say("Deleted");
            return 0;
        }
        if (wp == VK_ESCAPE) { if (g_hd_state != HD_OFF) stop_hd(1); return 0; }
        if (wp == VK_TAB) { switch_cat(shift ? -1 : 1); return 0; }
        if (wp == 'M') {
            set_mode((g_mode + (shift ? M_COUNT - 1 : 1)) % M_COUNT);
            say(mode_names[g_mode]); update_title();
            return 0;
        }
        if ((wp == VK_UP || wp == VK_DOWN) && g_hd_state != HD_OFF) {
            /* on HD, Up/Down move between HD1..HD4, only the ones this station carries */
            if (g_hd_state != HD_PLAYING) { say("Still looking for HD"); return 0; }
            int dir = wp == VK_UP ? 1 : -1, p = g_hd_program + dir;
            while (p >= 0 && p < 4 && !(g_hd_seen & (1 << p))) p += dir;
            if (p < 0 || p > 3) { beep(0); return 0; }
            g_hd_program = p;
            snprintf(t, sizeof t, "HD%d", p + 1); say(t);
            return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN) {
            if (!repeat) {
                int dir = wp == VK_UP ? 1 : -1;
                step(dir * (shift ? fine : tap));
                g_fine = shift; g_fast = GetKeyState(VK_CONTROL) < 0; g_hold_start = now(); g_dir = dir;
            }
            return 0;
        }
        if (wp == VK_PRIOR || wp == VK_NEXT) {
            int dir = wp == VK_PRIOR ? 1 : -1;
            if (cats[g_cat].count == 0) {                /* FM / AM: one broadcast channel per press */
                stop_hd(0);
                step(dir * (g_mode == M_WFM ? 200000.0 : 10000.0));
                const preset_t *p = preset_here();
                freq_text(f, sizeof f, g_station);
                snprintf(t, sizeof t, "%s%s%s", f, p ? ", " : "", p ? p->name : "");
                say(t); update_title();
            } else goto_preset(dir);
            return 0;
        }
        if (wp == VK_LEFT || wp == VK_RIGHT) {
            float v = g_volume + (wp == VK_RIGHT ? 0.05f : -0.05f);
            g_volume = v < 0 ? 0 : v > 1 ? 1 : v;
            snprintf(t, sizeof t, "Volume %d", (int)(g_volume * 100 + 0.5)); say(t);
            return 0;
        }
        if (wp == VK_SPACE) {
            freq_text(f, sizeof f, g_station);
            if (g_hd_audio) snprintf(t, sizeof t, "%s HD%d", f, g_hd_program + 1);
            else snprintf(t, sizeof t, "%s", f);
            say(t);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (g_mode != M_WFM) { say("HD needs wide FM"); return 0; }
            if (g_hd_state == HD_OFF) {
                g_hd_name[0] = 0;
                for (int k = 0; k < 4; k++) g_hd_title[k][0] = g_hd_artist[k][0] = 0;
                g_hd_program = 0; g_hd_seen = 0;
                g_hd_started = now();
                g_hd_state = HD_SEARCHING;
                SetEvent(hd_event);
                beep(5);
                say("Looking for HD");
            } else if (g_hd_state == HD_PLAYING) {
                snprintf(t, sizeof t, "HD%d", g_hd_program + 1); say(t);
            }
            return 0;
        }
        if (wp == 'N' && g_hd_state == HD_OFF && g_mode == M_WFM) {     /* analog FM: RDS */
            if (g_rds_rt[0]) snprintf(t, sizeof t, "%s%s%s", (const char *)g_rds_ps, g_rds_ps[0] ? ": " : "", (const char *)g_rds_rt);
            else if (g_rds_ps[0]) snprintf(t, sizeof t, "%s, no song text yet", (const char *)g_rds_ps);
            else snprintf(t, sizeof t, "No station information yet");
            say(t);
            return 0;
        }
        if (wp == 'N') {
            int p = g_hd_program;
            if (g_hd_title[p][0]) {
                snprintf(t, sizeof t, "%s%s%s", g_hd_title[p], g_hd_artist[p][0] ? ", by " : "", g_hd_artist[p]);
                say(t);
            } else say(g_hd_state == HD_OFF ? "Song information comes with HD. Press Enter to look for HD." : "No song information yet");
            return 0;
        }
        break;
    }
    case WM_KEYUP:
        if ((wp == VK_UP || wp == VK_DOWN) && g_dir) {
            g_dir = 0;
            mode_params(g_mode, g_station, &tap, &fine, &glide, &gmax);
            double grid = g_fine ? fine : tap;
            g_station = floor(g_station / grid + 0.5) * grid;
            const char *band = NULL;
            const preset_t *p = preset_here();
            freq_text(f, sizeof f, p ? p->hz : g_station);
            if (p) g_station = p->hz;
            snprintf(t, sizeof t, "%s%s%s%s%s", band ? band : "", band ? ", " : "", f, p ? ", " : "", p ? p->name : "");
            say(t);
            SetEvent(retune_event);              /* gain can change across 30 MHz */
            update_title();
            return 0;
        }
        break;
    case WM_APP: {                                   /* the scanner stopped on a signal */
        set_mode((int)wp);
        if (g_explore) {                             /* Explore: record it and keep sweeping, no waiting */
            double close_hz = g_mode == M_WFM ? 50000 : 5000;
            int known = 0;
            for (int i = 0; i < nfound; i++) if (fabs(found[i].hz - g_station) < close_hz) known = 1;
            if (!known && nfound < MAX_SAVED && found_cat >= 0) {
                /* named by frequency for now; the RDS name or digital system type is added the first
                   time you land on it (see name_found_here) */
                freq_text(f, sizeof f, g_station);
                preset_t *np = &found[nfound++];
                snprintf(np->name, sizeof np->name, "%s%s", f, lp == 2 ? ", wide digital signal" : lp ? ", digital" : "");
                np->hz = g_station; np->mode = g_mode;
                qsort(found, nfound, sizeof(preset_t), cmp_preset);
                cats[found_cat].count = nfound;
                g_explore_new++;
            }
            g_scan_req = 1;
            return 0;
        }
        const preset_t *p = preset_here();
        freq_text(f, sizeof f, g_station);
        snprintf(t, sizeof t, "%s, %s%s%s%s", f, mode_names[g_mode],
                 lp == 2 ? ", wide signal, sounds digital" : lp ? ", sounds digital" : "",
                 p ? ", " : "", p ? p->name : "");
        beep(3);
        say(t);
        update_title();
        if (g_test_stops) {                          /* --scantest: log, listen 1.5 s, continue */
            FILE *fp = fopen(g_test_log, "a");
            if (fp) { fprintf(fp, "%s   [avg %.1f dB, width %.0f Hz, flatness %.2f dB, gain %.1f]\n", t,
                              g_dbg_avg, g_dbg_w, g_dbg_flat, g_gain_applied / 10.0); fclose(fp); }
            if (--g_test_stops > 0) SetTimer(h, 2, 1500, NULL); else PostQuitMessage(0);
        }
        return 0;
    }
    case WM_APP + 3: update_title(); return 0;       /* trunk following moved the dial */
    case WM_APP + 7: offer_driver(h); return 0;      /* radio plugged in without its driver */
    case WM_APP + 8: {                               /* an update was installed: run the new copy */
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi) ||
            CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        DestroyWindow(h);
        return 0;
    }
    case WM_APP + 6: {                               /* broadcast survey finished: add the stations */
        int m = cats[g_cat].mode, added = 0;
        for (int i = 0; i < sv_found_n && nfound < MAX_SAVED; i++) {
            int known = 0;
            for (int j = 0; j < nfound; j++) if (fabs(found[j].hz - sv_found[i]) < (m == M_WFM ? 50000 : 3000)) known = 1;
            if (known) continue;
            preset_t *np = &found[nfound++];
            char fq[32]; freq_text(fq, sizeof fq, sv_found[i]);
            if (sv_found_name[i]) snprintf(np->name, sizeof np->name, "%s, %s", fq, sv_found_name[i]);
            else snprintf(np->name, sizeof np->name, "%s", fq);
            np->hz = sv_found[i]; np->mode = m;
            for (int k = 0; k < cats[g_cat].count; k++)        /* channels keep their own mode (e.g. ham FT8 is USB) */
                if (fabs(item(g_cat, k)->hz - sv_found[i]) < 100) np->mode = item(g_cat, k)->mode;
            added++;
        }
        qsort(found, nfound, sizeof(preset_t), cmp_preset);
        if (found_cat >= 0) cats[found_cat].count = nfound;
        write_found();
        g_explore = 0; g_explore_lap = 0;
        beep(3);
        snprintf(t, sizeof t, "%d active found%s. They're in Found here.", sv_found_n,
                 added < sv_found_n ? ", some already known" : "");
        say(t);
        return 0;
    }
    case WM_APP + 5:
        if (wp) say("Aircraft mode. Planes are announced as they are heard.");
        return 0;
    case WM_APP + 4:                                 /* RDS station name arrived */
        if (g_mode == M_WFM && !g_hd_audio) say((const char *)g_rds_ps);
        name_found_here((const char *)g_rds_ps, 50000);
        return 0;
    case WM_APP + 1:
        if (g_explore) {
            g_explore = 0; g_explore_lap = 0; beep(3);
            write_found();
            snprintf(t, sizeof t, "Explore finished. %d new signal%s added to Found here.", g_explore_new, g_explore_new == 1 ? "" : "s");
            say(t); return 0;
        }
        beep(2); say("Nothing found");
        if (g_test_stops) { FILE *fp = fopen(g_test_log, "a"); if (fp) { fprintf(fp, "Nothing found\n"); fclose(fp); } PostQuitMessage(0); }
        return 0;
    case WM_APP + 2: beep(0); return 0;              /* wrapped back to the start of the range */
    case WM_TIMER:                                   /* keep beeping while held against an edge */
        if (wp == 2) { KillTimer(h, 2); g_scan_req = 1; return 0; }   /* --scantest: next stop */
        if (wp == 4) {
            static LONG done; if (g_edge_ticks != done) { done = g_edge_ticks; beep(7); }
            if (g_glide_test && dev && !g_dir) { g_hold_start = now(); g_dir = g_glide_test; g_glide_test = 0; }  /* press */
            if (g_explore_test && dev) { g_explore_test = 0; PostMessageW(h, WM_KEYDOWN, 'X', 0); }             /* press X */
            return 0;
        }
        if (wp == 1 && !g_trunk && !g_dir) {                           /* name a found digital signal once identified */
            static double last_hz;
            if (fabs(g_station - last_hz) > 1000) { last_hz = g_station; id_reset(); }
            else if (g_mode == M_NFM && g_decode_on) {
                char desc[160];
                if (id_describe(desc, sizeof desc)) name_found_here(desc, 3200);
            }
        }
        if (wp == 1 && g_trunk && g_trunk_dmr) {                       /* a channel was learned: restart with the map */
            static LONG seen;
            if (cp_map_changed != seen) {
                seen = cp_map_changed;
                if (ds_proc) { ds_stop(); say("Learned a channel for this system"); }
            }
        }
        if (wp == 1 && g_trunk && !ds_proc && dev) ds_start();         /* (re)start trunk decoding */
        if (wp == 5) {                                                 /* Explore: identify, record, go on */
            KillTimer(h, 5);
            if (!g_explore) return 0;
            char desc[160]; const char *d = NULL;
            if (g_mode == M_WFM) { if (g_rds_ps[0]) { snprintf(desc, sizeof desc, "%s", (const char *)g_rds_ps); d = desc; } else d = "FM station"; }
            else if (g_mode == M_NFM) d = id_describe(desc, sizeof desc) ? desc : "analog or unidentified";
            else d = mode_names[g_mode];
            freq_text(f, sizeof f, g_station);
            if (nfound < MAX_SAVED && found_cat >= 0) {
                preset_t *p = &found[nfound++];
                snprintf(p->name, sizeof p->name, "%s, %s", f, d);
                p->hz = g_station; p->mode = g_mode;
                qsort(found, nfound, sizeof(preset_t), cmp_preset);
                cats[found_cat].count = nfound;
                write_found();
                g_explore_new++;
            }
            say(d);
            g_scan_req = 1;                                            /* on to the next one */
            return 0;
        }
        if (wp == 3) {                                                 /* --listen: time's up */
            char path[MAX_PATH]; snprintf(path, sizeof path, "%s", dec_log_path);
            strcpy(strrchr(path, '\\') + 1, "rds_test.log");
            FILE *fp = fopen(path, "a");
            if (fp) { fprintf(fp, "%.3f MHz: name [%s] text [%s]\n", g_station / 1e6, (const char *)g_rds_ps, (const char *)g_rds_rt); fclose(fp); }
            strcpy(strrchr(path, '\\') + 1, "glide_test.log");
            fp = fopen(path, "a");
            if (fp) { fprintf(fp, "ended %.3f MHz, retunes %ld, wide FM underruns %d drops %d, narrow underruns %d drops %d\n",
                              g_station / 1e6, g_retunes, ana.underruns, ana.drops, nar.underruns, nar.drops); fclose(fp); }
            PostQuitMessage(0); return 0;
        }
        dtmf_flush();
        if (g_dir && g_at_edge) beep(0);
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)show;
    LARGE_INTEGER q; QueryPerformanceFrequency(&q); g_qpf = (double)q.QuadPart;
    {   /* opt out of EcoQoS power throttling: real-time radio must not be slowed to efficiency cores */
        PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    }

    {   /* every decoder we start joins this job; when the tuner exits, however it exits, they all close */
        HANDLE job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li))
            AssignProcessToJobObject(job, GetCurrentProcess());
    }
    timeBeginPeriod(1);                             /* 1 ms sleeps: steady voice pacing */
    load_presets();
    load_lockouts();
    CreateThread(NULL, 0, location_thread, (LPVOID)saved_path, 0, NULL);
    int test_run = cmd && !strncmp(cmd, "--", 2);
    if (!test_run) CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    InitializeCriticalSection(&dec_lock);
    InitializeCriticalSection(&adsb_lock);
    for (int i = 0; i < 256; i++)
        for (int q = 0; q < 256; q++) adsb_mag[(i << 8) | q] = (uint16_t)(abs(i - 127) + abs(q - 127));
    snprintf(dec_log_path, sizeof dec_log_path, "%s", saved_path);
    strcpy(strrchr(dec_log_path, '\\') + 1, "decodes.txt");
    g_cat = 0;
    g_station = cats[0].last; g_mode = cats[0].last_mode;
    if (cmd && !strncmp(cmd, "--scantest", 10)) {   /* scan Tune anywhere from <MHz>, log each stop */
        double mhz = 0; int stops = 8, sq = 2;
        sscanf(cmd + 10, "%lf %d %d", &mhz, &stops, &sq);
        for (int c = 0; c < ncats; c++) if (!strcmp(cats[c].name, "Tune anywhere")) g_cat = c;
        g_station = mhz * 1e6; g_mode = M_NFM; g_squelch = sq & 3; g_test_stops = stops;
        GetModuleFileNameA(NULL, g_test_log, MAX_PATH);
        strcpy(strrchr(g_test_log, '\\') + 1, "scantest.log");
        DeleteFileA(g_test_log);
        g_scan_req = 1;
        cmd = NULL;
    }
    if (cmd && !strncmp(cmd, "--trunk", 7)) {       /* --trunk <MHz> <seconds> [DMR color code]: trunk following */
        double mhz = 0; int secs = 60, cc = -1;
        sscanf(cmd + 7, "%lf %d %d", &mhz, &secs, &cc);
        g_station = floor(mhz * 1e6 / 100 + 0.5) * 100; g_mode = M_NFM;
        g_trunk = 1; g_trunk_cc = g_station; g_listen_secs = secs;
        if (cc >= 0) { g_trunk_dmr = 1; cp_load_or_init(cc, g_station); }
        /* the narrowest category that holds this frequency, so arrow keys stay in range */
        double best = 1e12;
        for (int c = 0; c < ncats; c++)
            if (c != saved_cat && g_station >= cats[c].lo && g_station <= cats[c].hi && cats[c].hi - cats[c].lo < best)
                { best = cats[c].hi - cats[c].lo; g_cat = c; }
        cmd = NULL;
    }
    if (cmd && !strncmp(cmd, "--install-driver", 16)) return install_driver();   /* run elevated by offer_driver */
    if (cmd && !strncmp(cmd, "--driver-check", 14)) {
        FILE *fp = fopen("driver_check.log", "w");
        if (fp) { fprintf(fp, "driver state %d (0 none, 1 ok, 2 needs driver), version %s\n", driver_state(), TUNER_VERSION); fclose(fp); }
        return 0;
    }
    if (cmd && !strncmp(cmd, "--sounds", 8)) {      /* --sounds: each style, named, playing two phrases */
        static const char *names[3] = { "Style 1, electric piano", "Style 2, marimba", "Style 3, soft chime" };
        HMODULE nv = LoadLibraryA("nvdaControllerClient.dll");
        if (nv) { nvda_speak = (speak_fn)GetProcAddress(nv, "nvdaController_speakText"); nvda_cancel = (cancel_fn)GetProcAddress(nv, "nvdaController_cancelSpeech"); }
        g_volume = 0.30f;
        for (int s = 0; s < 3; s++) {
            g_sfx_style = s; say(names[s]); Sleep(2200);
            beep(1); Sleep(2600); beep(6); Sleep(1800);
        }
        return 0;
    }
    if (cmd && !strncmp(cmd, "--zip", 5)) {         /* --zip <text>: test the location lookup, log the result */
        char said[160];
        int ok = set_location(cmd + 6, said, sizeof said);
        FILE *fp = fopen("zip_test.log", "w");
        if (fp) { fprintf(fp, "[%s] -> %d: %s (%.4f %.4f)\n", cmd + 6, ok, said, g_home_lat, g_home_lon); fclose(fp); }
        return 0;
    }
    if (cmd && !strncmp(cmd, "--explore", 9)) {     /* --explore <MHz> <seconds>: Explore that frequency's category */
        double mhz = 0; int secs = 120;
        sscanf(cmd + 9, "%lf %d", &mhz, &secs);
        g_station = mhz * 1e6; g_listen_secs = secs; g_explore_test = 1;
        double best = 1e12;
        for (int c = 0; c < ncats; c++)
            if (c != saved_cat && c != found_cat && g_station >= cats[c].lo && g_station <= cats[c].hi && cats[c].hi - cats[c].lo < best)
                { best = cats[c].hi - cats[c].lo; g_cat = c; }
        g_mode = cats[g_cat].mode;
        cmd = NULL;
    }
    if (cmd && !strncmp(cmd, "--glide", 7)) {       /* --glide <MHz> <mode> <dir> <seconds>: hold an arrow */
        double mhz = 0; char mk[8] = "WFM"; int dir = 1, secs = 8;
        sscanf(cmd + 7, "%lf %7s %d %d", &mhz, mk, &dir, &secs);
        g_station = mhz * 1e6; g_mode = parse_mode(mk);
        g_glide_test = dir; g_listen_secs = secs + 1;
        double best = 1e12;
        for (int c = 0; c < ncats; c++)
            if (c != saved_cat && g_station >= cats[c].lo && g_station <= cats[c].hi && cats[c].hi - cats[c].lo < best)
                { best = cats[c].hi - cats[c].lo; g_cat = c; }
        cmd = NULL;
    }
    if (cmd && !strncmp(cmd, "--listen", 8)) {      /* --listen <MHz> <mode> <seconds>: test decoders */
        double mhz = 0; char mk[8] = "NFM"; int secs = 30;
        sscanf(cmd + 8, "%lf %7s %d", &mhz, mk, &secs);
        g_station = floor(mhz * 1e6 / 100 + 0.5) * 100; g_mode = parse_mode(mk);
        for (int c = 0; c < ncats; c++) if (!strcmp(cats[c].name, "Tune anywhere")) g_cat = c;
        g_listen_secs = secs;
        cmd = NULL;
    }
    if (cmd && atof(cmd) > 0.5) {                   /* optional starting frequency in MHz */
        double hz = floor(atof(cmd) * 1000 + 0.5) * 1000;
        for (int c = 0; c < ncats; c++)
            if (hz >= cats[c].lo && hz <= cats[c].hi) { g_cat = c; g_mode = cats[c].mode; break; }
        g_station = hz;
    }

    if (g_listen_secs) {                            /* test runs start their logs fresh */
        static const char *logs[] = { "dsd_raw.log", "voice_test.raw", "voice_timing.log", "level_test.log" };
        for (int i = 0; i < 4; i++) {
            char p[MAX_PATH]; snprintf(p, sizeof p, "%s", saved_path);
            strcpy(strrchr(p, '\\') + 1, logs[i]); DeleteFileA(p);
        }
    }

    HMODULE nv = LoadLibraryA("nvdaControllerClient.dll");
    if (nv) {
        nvda_speak = (speak_fn)GetProcAddress(nv, "nvdaController_speakText");
        nvda_cancel = (cancel_fn)GetProcAddress(nv, "nvdaController_cancelSpeech");
    }
    HMODULE hd = LoadLibraryA("libnrsc5.dll");
    if (hd) {
        hd_open_pipe = (p_open_pipe)GetProcAddress(hd, "nrsc5_open_pipe");
        hd_close = (p_close)GetProcAddress(hd, "nrsc5_close");
        hd_set_cb = (p_set_cb)GetProcAddress(hd, "nrsc5_set_callback");
        hd_pipe = (p_pipe_cf32)GetProcAddress(hd, "nrsc5_pipe_samples_cf32");
    }

    fft_init();
    design_lowpass(ch_h, CH_TAPS, 105000, FS_IN);
    design_lowpass(au_h, AU_TAPS, 16000, FS_MPX);
    design_lowpass(hb_h, HB_TAPS, 372000, FS_IN);
    design_lowpass(nd_h, ND_TAPS, 18000, FS_IN);
    design_lowpass(rds_h, RDS_TAPS, 2600, FS_MPX);
    rds_reset();
    design_lowpass(am_h, NCH_TAPS, 4500, FS_N);
    design_lowpass(nfm_h, NCH_TAPS, 6500, FS_N);
    design_lowpass(ssb_h, SSB_TAPS, 1350, FS_N);
    design_lowpass(cw_h, SSB_TAPS, 300, FS_N);
    de_a = (float)exp(-1.0 / (FS_MPX / D2 * 75e-6));
    hd_ring = calloc(HD_RING, sizeof(float));
    raw_ring = malloc(RAW_RING);
    dsp_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* wide FM: 15 ms buffers, at most 5 queued (~75 ms) so the dial feels immediate;
       HD arrives in bursts from the decoder, so it gets more slack */
    /* up to 10 x 15 ms queued: absorbs the USB hiccup a hardware retune causes during a glide */
    if (!out_open(&ana, FS_AUD, BUF_FRAMES, 10, 3) || !out_open(&hda, 44100, 1102, 16, 4) ||
        !out_open(&nar, 48012, 720, 10, 3) || !out_open(&voi, 8000, 160, 10, 3)) {   /* fed at a steady pace by voice_thread */
        MessageBoxA(NULL, "Cannot open audio output.", "SDR tuner", MB_OK); return 1;
    }
    retune_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    hd_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"SdrTuner";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(L"SdrTuner", L"SDR tuner", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          420, 140, NULL, NULL, inst, NULL);
    update_title();
    if (g_listen_secs || g_test_stops) ShowWindow(g_wnd, SW_SHOWMINNOACTIVE);   /* tests never take your keyboard */
    else {
        ShowWindow(g_wnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_wnd);
        SetFocus(g_wnd);
    }
    SetTimer(g_wnd, 1, 350, NULL);
    SetTimer(g_wnd, 4, 40, NULL);                   /* band-edge ticks while gliding */
    if (g_listen_secs) SetTimer(g_wnd, 3, g_listen_secs * 1000, NULL);

    CreateThread(NULL, 0, retune_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, hd_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, process_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, voice_thread, NULL, 0, NULL);
    tcp_ring = malloc(TCP_RING);
    tcp_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, rtltcp_thread, NULL, 0, NULL);
    HANDLE radio = CreateThread(NULL, 0, radio_thread, NULL, 0, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    InterlockedExchange(&g_stop, 1);
    SetEvent(hd_event);
    WaitForSingleObject(radio, 3000);
    mm_stop();
    rt_stop();
    ds_stop();
    child_stop(&ais);
    child_stop(&acars);
    waveOutReset(ana.wo); waveOutClose(ana.wo);
    waveOutReset(hda.wo); waveOutClose(hda.wo);
    waveOutReset(nar.wo); waveOutClose(nar.wo);
    return 0;
}
