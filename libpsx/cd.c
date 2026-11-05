/* cd.c - libpsx CD-ROM driver (milestone 7).
 *
 * Reads 2048-byte data sectors from the PlayStation CD-ROM controller over its
 * documented hardware registers (no BIOS CD routines, so no BIOS-revision
 * dependence). This is the sector source the ISO 9660 backend (fs/isofs.c)
 * streams through on real hardware, so a disc-sized game keeps its assets on
 * the disc instead of in RAM.
 *
 * The controller is a small command/response machine at 0x1F801800, reached
 * through the uncached KSEG1 window. Port 0x1F801800 holds the status and the
 * index that banks the other three ports. A command is issued by writing its
 * parameters to the parameter FIFO and the command byte to port 1; the drive
 * answers with an interrupt code in the index-1 flag register (INT3 = ack,
 * INT1 = a data sector is ready) and response bytes in the response FIFO.
 *
 * This read path is polled (it spins on the interrupt flag rather than taking
 * the CD interrupt) so it needs no handler. It follows the documented register
 * protocol and is verified functionally: make test-psx-cd-redux boots the
 * cd_stream sample with a real data disc (PCSX-Redux -iso), reads a file off it
 * through this driver and the ISO 9660 backend, and checks the result on screen.
 */

#include "psx.h"
#include "isofs.h"

#define CD_STATUS (*(volatile uint8_t *)0xBF801800u)   /* status / index select */
#define CD_REG1   (*(volatile uint8_t *)0xBF801801u)
#define CD_REG2   (*(volatile uint8_t *)0xBF801802u)
#define CD_REG3   (*(volatile uint8_t *)0xBF801803u)

#define CD_CMD_SETLOC    0x02
#define CD_CMD_PLAY      0x03
#define CD_CMD_READN     0x06
#define CD_CMD_PAUSE     0x09
#define CD_CMD_INIT      0x0A
#define CD_CMD_SETFILTER 0x0D
#define CD_CMD_SETMODE   0x0E
#define CD_CMD_READS     0x1B

/* SetMode bits. */
#define CD_MODE_CDDA     0x01   /* allow reading CD-DA (Red Book) sectors */
#define CD_MODE_REPORT   0x04   /* report interrupts during CD-DA playback */
#define CD_MODE_XA_FILTER 0x08  /* apply the Setfilter file/channel selection */
#define CD_MODE_XA_ADPCM 0x40   /* decode XA-ADPCM audio sectors to the SPU */

#define INT_ACK   3     /* INT3: command acknowledged */
#define INT_DATA  1     /* INT1: a data sector is ready */

static void
set_index(int i)
{
    CD_STATUS = (uint8_t)i;
}

/* Wait for a response interrupt, return its code, and acknowledge it. The flag
 * register is at index 1; the low three bits carry the interrupt number. */
static int
wait_int(void)
{
    set_index(1);
    while ((CD_REG3 & 0x07u) == 0)
        ;
    int code = CD_REG3 & 0x07u;
    CD_REG3 = 0x07u;                /* acknowledge all three interrupt lines */
    CD_REG2 = 0x00u;                /* clear any pending parameter bytes */
    return code;
}

static uint8_t
to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

void
psx_cd_init(void)
{
    set_index(1);
    CD_REG3 = 0x1Fu;                /* enable the interrupt lines */
    CD_REG3 = 0x07u;               /* acknowledge any stale flags */

    set_index(0);
    CD_REG1 = CD_CMD_INIT;         /* reset mode, spin up, read TOC */
    wait_int();                    /* INT3 ack */
    wait_int();                    /* INT2 complete */

    set_index(0);
    CD_REG2 = 0x00u;               /* mode: 2048-byte sectors, single speed */
    CD_REG1 = CD_CMD_SETMODE;
    wait_int();
}

int
psx_cd_read_sector(uint32_t lba, void *buf)
{
    /* Logical block to minute:second:frame, offset by the 2-second lead-in. */
    uint32_t pos = lba + 150u;
    int m = (int)(pos / 4500u);
    int s = (int)((pos / 75u) % 60u);
    int f = (int)(pos % 75u);

    set_index(0);
    CD_REG2 = to_bcd(m);
    CD_REG2 = to_bcd(s);
    CD_REG2 = to_bcd(f);
    CD_REG1 = CD_CMD_SETLOC;
    if (wait_int() != INT_ACK)
        return -1;

    set_index(0);
    CD_REG1 = CD_CMD_READN;
    if (wait_int() != INT_ACK)     /* ReadN acknowledges first (INT3) */
        return -1;
    if (wait_int() != INT_DATA)    /* then INT1 when the sector has arrived */
        return -1;

    /* Request the data and drain the 2048-byte sector from the data FIFO. */
    set_index(0);
    CD_REG3 = 0x80u;               /* BFRD: want data */
    unsigned char *dst = buf;
    for (int i = 0; i < 2048; i++)
        dst[i] = CD_REG2;
    CD_REG3 = 0x00u;

    set_index(0);
    CD_REG1 = CD_CMD_PAUSE;        /* stop reading */
    wait_int();
    wait_int();
    return 0;
}

int
psx_cd_mount(const char *name)
{
    return isofs_mount_stream(name, psx_cd_read_sector, 0);
}

/* --- CD audio --- */

void
psx_cd_audio_enable(uint16_t vol)
{
    /* Route the CD controller's audio output through the SPU to the main mix.
     * The SPU side (CD-input volume + the CD-audio enable bit) is set here; the
     * caller must have brought the SPU up with psx_spu_init first. */
    psx_spu_cd_volume(vol);
}

int
psx_cd_play_track(int track)
{
    set_index(0);
    CD_REG2 = CD_MODE_CDDA | CD_MODE_REPORT;    /* allow CD-DA, report position */
    CD_REG1 = CD_CMD_SETMODE;
    if (wait_int() != INT_ACK)
        return -1;

    /* Play seeks to the track (1-based, BCD) and plays it to the audio output,
     * which the SPU CD input mixes into the main output. */
    set_index(0);
    CD_REG2 = to_bcd(track);
    CD_REG1 = CD_CMD_PLAY;
    if (wait_int() != INT_ACK)
        return -1;
    return 0;
}

int
psx_cd_play_xa(uint32_t lba, int file, int channel)
{
    /* Select the XA file and channel to decode. */
    set_index(0);
    CD_REG2 = (uint8_t)file;
    CD_REG2 = (uint8_t)channel;
    CD_REG1 = CD_CMD_SETFILTER;
    if (wait_int() != INT_ACK)
        return -1;

    /* Decode XA-ADPCM sectors to the SPU, applying the filter. */
    set_index(0);
    CD_REG2 = CD_MODE_XA_ADPCM | CD_MODE_XA_FILTER | CD_MODE_CDDA;
    CD_REG1 = CD_CMD_SETMODE;
    if (wait_int() != INT_ACK)
        return -1;

    /* Seek to the first sector of the stream. */
    uint32_t pos = lba + 150u;
    set_index(0);
    CD_REG2 = to_bcd((int)(pos / 4500u));
    CD_REG2 = to_bcd((int)((pos / 75u) % 60u));
    CD_REG2 = to_bcd((int)(pos % 75u));
    CD_REG1 = CD_CMD_SETLOC;
    if (wait_int() != INT_ACK)
        return -1;

    /* ReadS streams sectors continuously; the controller decodes the audio
     * sectors into the SPU as they pass, so playback needs no further polling. */
    set_index(0);
    CD_REG1 = CD_CMD_READS;
    if (wait_int() != INT_ACK)
        return -1;
    return 0;
}

void
psx_cd_audio_stop(void)
{
    set_index(0);
    CD_REG1 = CD_CMD_PAUSE;
    wait_int();                     /* INT3 ack */
    wait_int();                     /* INT2 complete */
}

int
psx_cd_status(void)
{
    set_index(0);
    CD_REG1 = 0x01;                 /* GetStat: one status byte in the response */
    set_index(1);
    while ((CD_REG3 & 0x07u) == 0)  /* wait for the INT3 acknowledgement */
        ;
    int st = CD_REG1;               /* first response byte = status */
    CD_REG3 = 0x07u;                /* acknowledge */
    return st & 0xFF;
}

static int
from_bcd(int v)
{
    return ((v >> 4) & 0x0F) * 10 + (v & 0x0F);
}

int
psx_cd_track_count(void)
{
    set_index(0);
    CD_REG1 = 0x13;                 /* GetTN: response = status, first, last (BCD) */
    set_index(1);
    while ((CD_REG3 & 0x07u) == 0)
        ;
    (void)CD_REG1;                  /* status */
    (void)CD_REG1;                  /* first track */
    int last = CD_REG1;             /* last track (BCD) */
    CD_REG3 = 0x07u;
    return from_bcd(last);
}
