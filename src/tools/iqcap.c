/* iqcap: capture raw cu8 IQ with control over the tuner's analog IF bandwidth (which rtl_sdr lacks).
   usage: iqcap <Hz> <rate> <gain_dB> <bandwidth_Hz or 0 for auto> <samples> <file> */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "rtl-sdr.h"

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: iqcap <Hz> <rate> <gain_dB> <bw_Hz|0> <samples> <file>\n"); return 1; }
    uint32_t f = (uint32_t)atof(argv[1]), rate = (uint32_t)atof(argv[2]), bw = (uint32_t)atof(argv[4]);
    int gain = (int)(atof(argv[3]) * 10 + 0.5);
    long n = atol(argv[5]);
    rtlsdr_dev_t *dev;
    int r = -1;
    for (int t = 0; t < 5 && r < 0; t++) { r = rtlsdr_open(&dev, 0); if (r < 0) Sleep(400); }
    if (r < 0) { fprintf(stderr, "cannot open device\n"); return 1; }
    rtlsdr_set_sample_rate(dev, rate);
    rtlsdr_set_tuner_bandwidth(dev, bw);
    rtlsdr_set_center_freq(dev, f);
    rtlsdr_set_tuner_gain_mode(dev, 1);
    rtlsdr_set_tuner_gain(dev, gain);
    rtlsdr_reset_buffer(dev);
    FILE *fp = fopen(argv[6], "wb");
    unsigned char *buf = malloc(262144);
    long want = n * 2, got = 0, skip = 262144 * 2;   /* discard the first 512 KB while things settle */
    while (got < want) {
        int len = 0;
        if (rtlsdr_read_sync(dev, buf, 262144, &len) < 0 || len <= 0) break;
        if (skip > 0) { skip -= len; continue; }
        int k = (int)(want - got < len ? want - got : len);
        fwrite(buf, 1, k, fp);
        got += k;
    }
    fclose(fp);
    rtlsdr_close(dev);
    return got == want ? 0 : 1;
}
