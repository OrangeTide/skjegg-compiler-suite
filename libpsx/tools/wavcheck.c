/* wavcheck.c - measure the dominant frequency of captured audio.
 *
 * Reads the raw float samples PCSX-Redux writes through SDL's disk audio driver
 * (interleaved F32, little-endian, stereo 44100 by default) and finds the
 * strongest tone in the loudest window. This is the audio counterpart of
 * shotcheck: redux_shot.lua / shotcheck verify the GPU output pixel by pixel,
 * and this verifies the SPU output by frequency, so a sound test checks what the
 * emulator actually played, not just a clean exit.
 *
 * It locates the highest-energy window, runs a Goertzel sweep over it to find
 * the peak frequency, and reports it. With --expect HZ it exits nonzero unless
 * the peak is within tolerance and the window is loud enough to be a real tone.
 *
 * Usage:
 *   wavcheck <raw.f32> [--rate N] [--channels N] [--expect HZ] [--tol HZ]
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int
main(int argc, char **argv)
{
    const char *path = NULL;
    int rate = 44100, channels = 2;
    double expect = 0.0, tol = 12.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--channels") && i + 1 < argc) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc) expect = atof(argv[++i]);
        else if (!strcmp(argv[i], "--tol") && i + 1 < argc) tol = atof(argv[++i]);
        else if (argv[i][0] == '-') { fprintf(stderr, "wavcheck: unknown %s\n", argv[i]); return 9; }
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: wavcheck <raw.f32> [--rate N] [--channels N] "
                        "[--expect HZ] [--tol HZ]\n");
        return 9;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "wavcheck: cannot open %s\n", path); return 9; }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    long nfloats = bytes / 4;
    float *raw = malloc((size_t)nfloats * sizeof *raw);
    if (!raw || fread(raw, 4, (size_t)nfloats, f) != (size_t)nfloats) {
        fprintf(stderr, "wavcheck: short read\n"); return 9;
    }
    fclose(f);

    /* Deinterleave channel 0 into a mono buffer. */
    long n = nfloats / channels;
    float *x = malloc((size_t)n * sizeof *x);
    for (long i = 0; i < n; i++)
        x[i] = raw[i * channels];
    free(raw);

    /* Find the window of W samples with the highest RMS (where the tone is). */
    long W = rate / 4;                  /* a quarter second */
    if (W > n) W = n;
    double sumsq = 0.0;
    for (long i = 0; i < W; i++)
        sumsq += (double)x[i] * x[i];
    double best = sumsq;
    long best_at = 0;
    for (long i = W; i < n; i++) {       /* slide the window */
        sumsq += (double)x[i] * x[i] - (double)x[i - W] * x[i - W];
        if (sumsq > best) { best = sumsq; best_at = i - W + 1; }
    }
    double rms = sqrt(best / W);

    /* Goertzel sweep over the loudest window for the peak frequency. */
    const float *win = x + best_at;
    double peak_f = 0.0, peak_p = -1.0;
    for (double fr = 80.0; fr <= 2000.0; fr += 2.0) {
        double w = 2.0 * M_PI * fr / rate;
        double coeff = 2.0 * cos(w);
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (long i = 0; i < W; i++) {
            s0 = win[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        double p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        if (p > peak_p) { peak_p = p; peak_f = fr; }
    }

    printf("wavcheck: %ld frames, peak %.1f Hz, rms %.4f\n", n, peak_f, rms);

    if (expect > 0.0) {
        if (rms < 0.01) {
            printf("wavcheck: FAIL silent (rms %.4f)\n", rms);
            return 1;
        }
        if (fabs(peak_f - expect) > tol) {
            printf("wavcheck: FAIL peak %.1f Hz, want %.1f +-%.1f\n",
                   peak_f, expect, tol);
            return 2;
        }
    }
    free(x);
    return 0;
}
