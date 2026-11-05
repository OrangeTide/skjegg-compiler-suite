/* spu.c - libpsx sound driver (milestone 2).
 *
 * Brings up the SPU, uploads SPU-ADPCM samples into the 512 KB of sound RAM by
 * DMA, and keys voices on and off. Samples are encoded on the host by vagconv
 * (libpsx/tools/vagconv.c). Register access is through the uncached KSEG1
 * window; the SPU block begins at 0x1F801C00.
 *
 * Audio output cannot be checked by a headless run, so the automated gate only
 * confirms the init, upload, and key-on paths run without hanging. The encoder
 * itself, the part most prone to logic error, is host-tested (test-psx-adpcm).
 */

#include "psx.h"

#define SPU_MAIN_VOL_L (*(volatile uint16_t *)0xBF801D80u)
#define SPU_MAIN_VOL_R (*(volatile uint16_t *)0xBF801D82u)
#define SPU_KEY_ON_LO  (*(volatile uint16_t *)0xBF801D88u)
#define SPU_KEY_ON_HI  (*(volatile uint16_t *)0xBF801D8Au)
#define SPU_KEY_OFF_LO (*(volatile uint16_t *)0xBF801D8Cu)
#define SPU_KEY_OFF_HI (*(volatile uint16_t *)0xBF801D8Eu)
#define SPU_CTRL       (*(volatile uint16_t *)0xBF801DAAu)   /* SPUCNT */
#define SPU_TADDR      (*(volatile uint16_t *)0xBF801DA6u)   /* transfer address */
#define SPU_TCTRL      (*(volatile uint16_t *)0xBF801DACu)   /* transfer control */
#define SPU_CD_VOL_L   (*(volatile uint16_t *)0xBF801DB0u)   /* CD input volume L */
#define SPU_CD_VOL_R   (*(volatile uint16_t *)0xBF801DB2u)   /* CD input volume R */

#define SPU_CTRL_ENABLE 0x8000u
#define SPU_CTRL_UNMUTE 0x4000u
#define SPU_CTRL_CD_AUDIO 0x0001u         /* route the CD input into the mix */
#define SPU_CTRL_DMA_WRITE 0x0020u       /* transfer mode bits 5-4 = 10 */
#define SPU_CTRL_XFER_MASK 0x0030u

/* SPU DMA is channel 4. */
#define D4_MADR (*(volatile uint32_t *)0xBF8010C0u)
#define D4_BCR  (*(volatile uint32_t *)0xBF8010C4u)
#define D4_CHCR (*(volatile uint32_t *)0xBF8010C8u)
#define CHCR_BUSY 0x01000000u
#define CHCR_SPU_WRITE 0x01000201u       /* start | sync mode 1 | RAM->device */

static void
busy(int n)
{
    volatile int i;
    for (i = 0; i < n; i++)
        ;
}

/* A voice's 16-bit registers, at 0x1F801C00 + voice*0x10 + off. */
static volatile uint16_t *
vreg(int voice, uint32_t off)
{
    return (volatile uint16_t *)(0xBF801C00u + (uint32_t)voice * 0x10u + off);
}

void
psx_spu_init(void)
{
    SPU_CTRL = 0;                       /* SPU off while we set it up */
    busy(1000);
    SPU_KEY_OFF_LO = 0xFFFFu;           /* silence every voice */
    SPU_KEY_OFF_HI = 0xFFFFu;
    SPU_MAIN_VOL_L = 0x3FFFu;
    SPU_MAIN_VOL_R = 0x3FFFu;
    SPU_TCTRL = 0x0004u;                /* normal transfer mode */
    SPU_CTRL = SPU_CTRL_ENABLE | SPU_CTRL_UNMUTE;
    busy(1000);
}

void
psx_spu_cd_volume(uint16_t vol)
{
    /* Set the CD-input mixer volume (both sides) and enable the CD-audio path
     * in SPUCNT, so audio the CD controller produces (a CD-DA track or decoded
     * XA-ADPCM) reaches the main output.  Keep the other SPUCNT bits. */
    SPU_CD_VOL_L = vol;
    SPU_CD_VOL_R = vol;
    SPU_CTRL = SPU_CTRL | SPU_CTRL_CD_AUDIO;
}

void
psx_spu_upload(uint32_t dest, const void *data, uint32_t nbytes)
{
    uint32_t nwords = (nbytes + 3u) / 4u;

    while (D4_CHCR & CHCR_BUSY)         /* finish any previous transfer */
        ;

    SPU_TADDR = (uint16_t)(dest >> 3);  /* sound-RAM address, in 8-byte units */
    SPU_CTRL = (SPU_CTRL & ~SPU_CTRL_XFER_MASK) | SPU_CTRL_DMA_WRITE;
    busy(100);

    D4_MADR = (uint32_t)(uintptr_t)data & 0x00FFFFFFu;
    D4_BCR = (1u << 16) | nwords;       /* one block of nwords words */
    D4_CHCR = CHCR_SPU_WRITE;

    while (D4_CHCR & CHCR_BUSY)         /* wait for the upload to drain */
        ;

    SPU_CTRL = SPU_CTRL & ~SPU_CTRL_XFER_MASK;   /* back to stop mode */
}

void
psx_spu_play(int voice, uint32_t addr, uint16_t pitch, uint16_t vol)
{
    *vreg(voice, 0x00) = vol;           /* volume left */
    *vreg(voice, 0x02) = vol;           /* volume right */
    *vreg(voice, 0x04) = pitch;         /* sample rate (0x1000 = 44100 Hz) */
    *vreg(voice, 0x06) = (uint16_t)(addr >> 3);     /* ADPCM start address */
    *vreg(voice, 0x0E) = (uint16_t)(addr >> 3);     /* repeat address */
    *vreg(voice, 0x08) = 0x00FFu;       /* ADSR: fast attack to full */
    *vreg(voice, 0x0A) = 0x0000u;

    if (voice < 16)
        SPU_KEY_ON_LO = (uint16_t)(1u << voice);
    else
        SPU_KEY_ON_HI = (uint16_t)(1u << (voice - 16));
}

void
psx_spu_stop(int voice)
{
    if (voice < 16)
        SPU_KEY_OFF_LO = (uint16_t)(1u << voice);
    else
        SPU_KEY_OFF_HI = (uint16_t)(1u << (voice - 16));
}

uint32_t
psx_spu_make_adsr(int attack, int decay, int sustain, int release)
{
    /* Build the two ADSR words from friendly 0..15 parameters. Attack, decay,
     * and release are rates (0 = fast, 15 = slow); sustain is the level the
     * note holds at (0 = quiet, 15 = full). The rate parameters map to the
     * hardware's shift fields, and the modes are chosen for a natural envelope:
     * linear attack, exponential decay, an exponential decreasing sustain, and
     * exponential release. */
    unsigned a = (unsigned)(attack & 0x0F) * 2;     /* attack shift 0..30 */
    unsigned d = (unsigned)(decay & 0x0F);          /* decay shift 0..15 */
    unsigned s = (unsigned)(sustain & 0x0F);        /* sustain level */
    unsigned r = (unsigned)(release & 0x0F) * 2;    /* release shift 0..30 */

    uint16_t adsr1 = (uint16_t)((0u << 15)          /* linear attack */
                   | (a << 10)                      /* attack shift */
                   | (0u << 8)                      /* attack step +7 */
                   | (d << 4)                       /* decay shift */
                   | s);                            /* sustain level */
    uint16_t adsr2 = (uint16_t)((1u << 15)          /* sustain exponential */
                   | (1u << 14)                     /* sustain decreasing */
                   | (0x1Fu << 8)                   /* sustain shift (slow) */
                   | (0u << 6)                       /* sustain step */
                   | (1u << 5)                       /* release exponential */
                   | r);                            /* release shift */
    return (uint32_t)adsr1 | ((uint32_t)adsr2 << 16);
}

void
psx_spu_play_adsr(int voice, uint32_t addr, uint16_t pitch, uint16_t vol,
                  uint32_t adsr)
{
    *vreg(voice, 0x00) = vol;
    *vreg(voice, 0x02) = vol;
    *vreg(voice, 0x04) = pitch;
    *vreg(voice, 0x06) = (uint16_t)(addr >> 3);
    *vreg(voice, 0x0E) = (uint16_t)(addr >> 3);
    *vreg(voice, 0x08) = (uint16_t)(adsr & 0xFFFF);         /* ADSR1 */
    *vreg(voice, 0x0A) = (uint16_t)(adsr >> 16);            /* ADSR2 */

    if (voice < 16)
        SPU_KEY_ON_LO = (uint16_t)(1u << voice);
    else
        SPU_KEY_ON_HI = (uint16_t)(1u << (voice - 16));
}

/* Reverb. The SPU's reverb is an echo processor fed by a work area at the top
 * of sound RAM and steered by a block of coefficient and offset registers at
 * 0x1F801DC0. A preset is 32 of those words; the ones here are the widely
 * published "Hall" and "Room" presets. Reverb is routed per voice (the EON
 * mask) and switched on in SPUCNT (bit 7). The reverb output volume is at
 * 0x1F801D84/86.
 *
 * As with the rest of the SPU, a headless run cannot hear the result; the gate
 * confirms the config path runs and that a reverbed voice still keys on and
 * plays at the right pitch (the capture test). */
#define SPU_REVERB_VOL_L (*(volatile uint16_t *)0xBF801D84u)
#define SPU_REVERB_VOL_R (*(volatile uint16_t *)0xBF801D86u)
#define SPU_REVERB_ON_LO (*(volatile uint16_t *)0xBF801D98u)   /* EON */
#define SPU_REVERB_ON_HI (*(volatile uint16_t *)0xBF801D9Au)
#define SPU_REVERB_BASE  (*(volatile uint16_t *)0xBF801DA2u)   /* mBASE */
#define SPU_CTRL_REVERB  0x0080u
/* The 32 reverb registers, 16-bit each, starting at 0x1F801DC0. */
#define SPU_REVERB_REGS  ((volatile uint16_t *)0xBF801DC0u)

/* mBASE in 8-byte units: a 0x20000-byte (128 KB) work area at the top of the
 * 512 KB sound RAM. The preset offsets below fit inside it. */
#define REVERB_BASE_ADDR 0x60000u

static const uint16_t reverb_hall[32] = {
    0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000,
    0x6000, 0x5C00, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
    0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000,
};

static const uint16_t reverb_room[32] = {
    0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
    0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
    0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000,
};

void
psx_spu_reverb_preset(const uint16_t *coeffs)
{
    int i;

    /* Place the work area, load the 32 coefficients, open the reverb output,
     * and switch reverb on in SPUCNT (keeping the other bits). */
    SPU_REVERB_BASE = (uint16_t)(REVERB_BASE_ADDR >> 3);
    for (i = 0; i < 32; i++)
        SPU_REVERB_REGS[i] = coeffs[i];
    SPU_REVERB_VOL_L = 0x4000u;
    SPU_REVERB_VOL_R = 0x4000u;
    SPU_CTRL = SPU_CTRL | SPU_CTRL_REVERB;
}

void
psx_spu_reverb(int preset)
{
    psx_spu_reverb_preset(preset == PSX_REVERB_ROOM ? reverb_room : reverb_hall);
}

void
psx_spu_reverb_off(void)
{
    SPU_CTRL = SPU_CTRL & ~SPU_CTRL_REVERB;
    SPU_REVERB_ON_LO = 0;
    SPU_REVERB_ON_HI = 0;
}

void
psx_spu_voice_reverb(int voice, int on)
{
    volatile uint16_t *reg = (voice < 16) ? &SPU_REVERB_ON_LO : &SPU_REVERB_ON_HI;
    uint16_t bit = (uint16_t)(1u << (voice & 15));

    if (on)
        *reg = (uint16_t)(*reg | bit);
    else
        *reg = (uint16_t)(*reg & ~bit);
}

/* A built-in tone, so any language can make a sound with no sample to manage.
 * The ADPCM is generated at build time (vagconv) and linked into libpsx. */
extern const unsigned char psx_beep_data[];
extern const unsigned psx_beep_data_len;

#define BEEP_ADDR 0x1000u

void
psx_beep(void)
{
    static int uploaded = 0;
    if (!uploaded) {
        psx_spu_upload(BEEP_ADDR, psx_beep_data, psx_beep_data_len);
        uploaded = 1;
    }
    psx_spu_play(0, BEEP_ADDR, PSX_SPU_PITCH_22050, 0x3FFF);
}
