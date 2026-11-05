/* mkcdda.c - build a mixed-mode CD image for the CD-DA audio test.
 *
 * Writes a Red Book audio track as raw CD audio (44100 Hz, 16-bit stereo,
 * little-endian, 2352 bytes per sector) holding a sine tone, and a CUE sheet
 * that places a data track first (an existing ISO) and the audio track second,
 * with the standard two-second pregap between them. PCSX-Redux loads the CUE as
 * a disc, so psx_cd_play_track(2) seeks to the tone and the SPU plays it, which
 * the audio-capture tier then checks by frequency (wavcheck).
 *
 * This keeps the CD-DA test self-contained: no checked-in binary disc, just a
 * generated tone whose frequency the test knows.
 *
 * Usage:
 *   mkcdda --iso NAME --freq HZ --secs N --out-pcm FILE --out-cue FILE
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE       44100
#define FRAMES_SEC 75                       /* CD sectors per second */
#define SAMP_SEC   (RATE / FRAMES_SEC)      /* 588 stereo frames per sector */
#define PREGAP_SEC 2

/* The final path component: a CUE names its files relative to the CUE's own
 * directory, so the audio track must be referenced by basename, not by the
 * build-relative path the tool was told to write it to. */
static const char *
basename_of(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/')
            b = s + 1;
    return b;
}

static void
put16(FILE *f, int v)
{
    unsigned u = (unsigned)(v & 0xFFFF);
    fputc((int)(u & 0xFF), f);
    fputc((int)((u >> 8) & 0xFF), f);
}

int
main(int argc, char **argv)
{
    const char *iso = "disc.iso", *pcm = "audio.pcm", *cue = "disc.cue";
    double freq = 440.0;
    int secs = 2;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iso") && i + 1 < argc)
            iso = argv[++i];
        else if (!strcmp(argv[i], "--freq") && i + 1 < argc)
            freq = atof(argv[++i]);
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc)
            secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out-pcm") && i + 1 < argc)
            pcm = argv[++i];
        else if (!strcmp(argv[i], "--out-cue") && i + 1 < argc)
            cue = argv[++i];
        else {
            fprintf(stderr, "mkcdda: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    FILE *fp = fopen(pcm, "wb");
    if (!fp) {
        fprintf(stderr, "mkcdda: cannot write %s\n", pcm);
        return 1;
    }

    /* Two seconds of silence for the pregap, so the audio track's INDEX 01
     * lands at 00:02:00 within the file and the tone starts there. */
    long silent = (long)PREGAP_SEC * FRAMES_SEC * SAMP_SEC;
    for (long i = 0; i < silent; i++) {
        put16(fp, 0);
        put16(fp, 0);
    }

    /* The tone, half amplitude to stay clear of clipping. */
    long total = (long)secs * FRAMES_SEC * SAMP_SEC;
    for (long i = 0; i < total; i++) {
        int s = (int)(0.5 * 32767.0 * sin(2.0 * M_PI * freq * (double)i / RATE));
        put16(fp, s);
        put16(fp, s);
    }
    fclose(fp);

    FILE *fc = fopen(cue, "wb");
    if (!fc) {
        fprintf(stderr, "mkcdda: cannot write %s\n", cue);
        return 1;
    }
    fprintf(fc, "FILE \"%s\" BINARY\n", basename_of(iso));
    fprintf(fc, "  TRACK 01 MODE1/2048\n");
    fprintf(fc, "    INDEX 01 00:00:00\n");
    fprintf(fc, "FILE \"%s\" BINARY\n", basename_of(pcm));
    fprintf(fc, "  TRACK 02 AUDIO\n");
    fprintf(fc, "    INDEX 00 00:00:00\n");
    fprintf(fc, "    INDEX 01 00:%02d:00\n", PREGAP_SEC);
    fclose(fc);

    return 0;
}
