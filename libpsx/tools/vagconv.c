/* vagconv.c - host-side audio converter for libpsx (milestone 2).
 *
 * Encodes 16-bit mono PCM into PlayStation SPU-ADPCM, the format the SPU plays,
 * and emits it as a C array the game links and uploads to SPU RAM. It can read
 * a simple PCM16 mono WAV, or synthesize a test tone, so the sound pipeline is
 * self-contained and in C with no external tools.
 *
 * SPU-ADPCM packs 28 samples into each 16-byte block: a shift/filter byte, a
 * flags byte (loop control), then 28 signed 4-bit samples. Decoding a nibble v
 * at a given shift contributes v << (12 - shift), plus a two-tap predictor
 * chosen per block from five fixed filters. The encoder picks, per block, the
 * filter and shift that minimize the reconstruction error, carrying the decoded
 * history across blocks so the error does not drift.
 *
 * Usage:
 *   vagconv --tone <hz> <ms> <rate> -c <symbol> <out.c>
 *   vagconv <in.wav> -c <symbol> <out.c>
 *   vagconv --selftest
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Predictor coefficients, scaled by 64. */
static const int FILTER_0[5] = { 0, 60, 115, 98, 122 };
static const int FILTER_1[5] = { 0, 0, -52, -55, -60 };

static int
clamp16(int v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return v;
}

/* Decode one nibble under a (filter, shift), advancing the history. */
static int
decode_step(int v, int shift, int filter, int *h1, int *h2)
{
    int pred = (*h1 * FILTER_0[filter] + *h2 * FILTER_1[filter]) >> 6;
    int sample = clamp16((v << (12 - shift)) + pred);
    *h2 = *h1;
    *h1 = sample;
    return sample;
}

/* Encode one block of up to 28 samples. Picks the filter and shift with the
 * least squared error, writes the 16-byte block, and updates the history. */
static void
encode_block(const short *in, int n, int *h1, int *h2,
             unsigned char flags, unsigned char *out)
{
    double best_err = 1e300;
    int best_f = 0, best_s = 0;
    int best_h1 = *h1, best_h2 = *h2;
    signed char best_nib[28];

    for (int f = 0; f < 5; f++) {
        for (int shift = 0; shift <= 12; shift++) {
            int step = 1 << (12 - shift);
            int lh1 = *h1, lh2 = *h2;
            double err = 0.0;
            signed char nib[28];
            for (int i = 0; i < 28; i++) {
                int s = (i < n) ? in[i] : 0;
                int pred = (lh1 * FILTER_0[f] + lh2 * FILTER_1[f]) >> 6;
                int resid = s - pred;
                int v = (int)lround((double)resid / (double)step);
                if (v > 7)
                    v = 7;
                if (v < -8)
                    v = -8;
                nib[i] = (signed char)v;
                int dec = decode_step(v, shift, f, &lh1, &lh2);
                double d = (double)(s - dec);
                err += d * d;
            }
            if (err < best_err) {
                best_err = err;
                best_f = f;
                best_s = shift;
                best_h1 = lh1;
                best_h2 = lh2;
                memcpy(best_nib, nib, sizeof nib);
            }
        }
    }

    out[0] = (unsigned char)((best_f << 4) | best_s);
    out[1] = flags;
    for (int i = 0; i < 14; i++) {
        int lo = best_nib[i * 2] & 0x0F;
        int hi = best_nib[i * 2 + 1] & 0x0F;
        out[2 + i] = (unsigned char)((hi << 4) | lo);
    }

    *h1 = best_h1;
    *h2 = best_h2;
}

/* Encode a whole PCM buffer to ADPCM blocks. The last block is marked
 * end+repeat so a keyed-on voice sustains (looping) until key-off. Returns the
 * number of bytes written to *out (caller frees). */
static size_t
encode_all(const short *pcm, size_t nsamp, unsigned char **out)
{
    size_t nblocks = (nsamp + 27) / 28;
    if (nblocks == 0)
        nblocks = 1;
    unsigned char *buf = malloc(nblocks * 16);
    int h1 = 0, h2 = 0;
    for (size_t b = 0; b < nblocks; b++) {
        size_t off = b * 28;
        int n = (int)(nsamp - off);
        if (n > 28)
            n = 28;
        if (n < 0)
            n = 0;
        unsigned char flags = (b == nblocks - 1) ? 0x03 : 0x00;
        encode_block(pcm + off, n, &h1, &h2, flags, buf + b * 16);
    }
    *out = buf;
    return nblocks * 16;
}

/* Decode an ADPCM buffer back to PCM, for the self-test. */
static void
decode_all(const unsigned char *adpcm, size_t nbytes, short *pcm)
{
    int h1 = 0, h2 = 0;
    size_t nblocks = nbytes / 16;
    for (size_t b = 0; b < nblocks; b++) {
        const unsigned char *blk = adpcm + b * 16;
        int shift = blk[0] & 0x0F;
        int filter = (blk[0] >> 4) & 0x07;
        if (filter > 4)
            filter = 4;
        for (int i = 0; i < 28; i++) {
            int byte = blk[2 + i / 2];
            int v = (i & 1) ? (byte >> 4) : (byte & 0x0F);
            if (v & 0x08)
                v -= 16;                /* sign-extend 4 bits */
            pcm[b * 28 + i] = (short)decode_step(v, shift, filter, &h1, &h2);
        }
    }
}

static short *
synth_tone(double hz, int ms, int rate, size_t *nsamp)
{
    size_t n = (size_t)((long long)ms * rate / 1000);
    short *pcm = malloc(n * sizeof *pcm);
    for (size_t i = 0; i < n; i++)
        pcm[i] = (short)(20000.0 * sin(2.0 * M_PI * hz * (double)i / rate));
    *nsamp = n;
    return pcm;
}

static short *
read_wav(const char *path, size_t *nsamp)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "vagconv: cannot open %s\n", path);
        return NULL;
    }
    unsigned char hdr[44];
    if (fread(hdr, 1, 44, f) != 44 || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "vagconv: %s is not a PCM WAV\n", path);
        fclose(f);
        return NULL;
    }
    /* Minimal: assume a canonical 44-byte header, 16-bit mono PCM. */
    long start = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, start, SEEK_SET);
    size_t n = (size_t)(end - start) / 2;
    short *pcm = malloc(n * sizeof *pcm);
    if (fread(pcm, 2, n, f) != n) {
        fprintf(stderr, "vagconv: short read on %s\n", path);
        free(pcm);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *nsamp = n;
    return pcm;
}

static int
emit_c(const char *sym, const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "vagconv: cannot write %s\n", path);
        return 1;
    }
    fprintf(f, "/* generated by vagconv: SPU-ADPCM, do not edit */\n");
    fprintf(f, "const unsigned char %s[] = {\n", sym);
    for (size_t i = 0; i < len; i++)
        fprintf(f, "%u,%s", data[i], (i % 16 == 15) ? "\n" : "");
    fprintf(f, "\n};\n");
    fprintf(f, "const unsigned %s_len = %zu;\n", sym, len);
    fclose(f);
    return 0;
}

static int
selftest(void)
{
    size_t nsamp;
    short *pcm = synth_tone(440.0, 1000, 22050, &nsamp);
    unsigned char *adpcm;
    size_t nbytes = encode_all(pcm, nsamp, &adpcm);
    short *back = malloc((nbytes / 16) * 28 * sizeof *back);
    decode_all(adpcm, nbytes, back);

    double sse = 0.0;
    int maxerr = 0;
    size_t n = nsamp;
    if (n > (nbytes / 16) * 28)
        n = (nbytes / 16) * 28;
    for (size_t i = 0; i < n; i++) {
        int e = pcm[i] - back[i];
        if (e < 0)
            e = -e;
        if (e > maxerr)
            maxerr = e;
        sse += (double)e * e;
    }
    double rms = sqrt(sse / (double)n);
    printf("vagconv selftest: %zu samples, rms error %.1f, max error %d\n",
           n, rms, maxerr);
    free(pcm);
    free(adpcm);
    free(back);
    /* A clean sine through 4-bit ADPCM reconstructs well; a broken encoder
       blows well past this bound. */
    return (rms < 1500.0) ? 0 : 1;
}

int
main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();

    const char *sym = NULL, *outc = NULL;
    short *pcm = NULL;
    size_t nsamp = 0;
    int i = 1;

    if (argc >= 2 && strcmp(argv[1], "--tone") == 0) {
        if (argc < 5) {
            fprintf(stderr, "vagconv --tone <hz> <ms> <rate> -c <sym> <out.c>\n");
            return 2;
        }
        pcm = synth_tone(atof(argv[2]), atoi(argv[3]), atoi(argv[4]), &nsamp);
        i = 5;
    } else if (argc >= 2) {
        pcm = read_wav(argv[1], &nsamp);
        i = 2;
    } else {
        fprintf(stderr, "vagconv: nothing to do (try --selftest)\n");
        return 2;
    }
    if (!pcm)
        return 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 2 < argc + 1 && i + 1 < argc) {
            sym = argv[i + 1];
            if (i + 2 < argc)
                outc = argv[i + 2];
            i += 2;
        }
    }
    if (!sym || !outc) {
        fprintf(stderr, "vagconv: need -c <symbol> <out.c>\n");
        free(pcm);
        return 2;
    }

    unsigned char *adpcm;
    size_t nbytes = encode_all(pcm, nsamp, &adpcm);
    int rc = emit_c(sym, outc, adpcm, nbytes);
    free(pcm);
    free(adpcm);
    return rc;
}
