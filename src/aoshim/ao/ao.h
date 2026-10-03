/* Minimal libao-compatible API for nrsc5 on Windows ARM64: live output via waveOut,
   file output as WAV or raw PCM ("-" means stdout). */
#ifndef AOSHIM_AO_H
#define AOSHIM_AO_H

#ifdef __cplusplus
extern "C" {
#endif

#define AO_FMT_LITTLE 1
#define AO_FMT_BIG    2
#define AO_FMT_NATIVE 4

typedef struct ao_device ao_device;
typedef struct ao_option ao_option;

typedef struct ao_sample_format {
    int bits;
    int rate;
    int channels;
    int byte_format;
    char *matrix;
} ao_sample_format;

void ao_initialize(void);
void ao_shutdown(void);
int ao_default_driver_id(void);
int ao_driver_id(const char *short_name);
ao_device *ao_open_live(int driver_id, ao_sample_format *format, ao_option *options);
ao_device *ao_open_file(int driver_id, const char *filename, int overwrite, ao_sample_format *format, ao_option *options);
int ao_play(ao_device *device, char *output_samples, unsigned int num_bytes);
int ao_close(ao_device *device);

#ifdef __cplusplus
}
#endif
#endif
