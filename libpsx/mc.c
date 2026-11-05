/* mc.c - libpsx memory card block I/O.
 *
 * A PlayStation memory card is on SIO0, the serial port it shares with the
 * controllers, selected by the 0x81 address byte instead of the controller's
 * 0x01. A card holds 1024 frames of 128 bytes (128 KB): frame 0 and the next
 * fifteen are the directory, and the rest are fifteen 8 KB save blocks. This
 * driver reads and writes one 128-byte frame at a time over the documented
 * card protocol; the save-file manager (mc_fs.c) builds the directory and the
 * named-save API on top of it.
 *
 * The access is polled with the same /ACK handshake as the controller, and it
 * depends on no BIOS routine. Register access is through the uncached KSEG1
 * window.
 */

#include "psx.h"

#define JOY_DATA (*(volatile uint8_t  *)0xBF801040u)
#define JOY_STAT (*(volatile uint32_t *)0xBF801044u)
#define JOY_MODE (*(volatile uint16_t *)0xBF801048u)
#define JOY_CTRL (*(volatile uint16_t *)0xBF80104Au)
#define JOY_BAUD (*(volatile uint16_t *)0xBF80104Eu)

#define STAT_TX_READY 0x0001u
#define STAT_RX_READY 0x0002u
#define STAT_ACK      0x0080u

#define CTRL_TXEN   0x0001u
#define CTRL_SELECT 0x0002u
#define CTRL_RXEN   0x0004u
#define CTRL_ACKIRQ 0x0010u
#define CTRL_RESET  0x0040u

#define MC_FRAME_SIZE 128
#define MC_GOOD       0x47      /* 'G': the card accepted the frame */

static void
busy(int n)
{
    volatile int i;
    for (i = 0; i < n; i++)
        ;
}

static uint8_t
xfer(uint8_t out)
{
    while (!(JOY_STAT & STAT_TX_READY))
        ;
    JOY_DATA = out;
    while (!(JOY_STAT & STAT_RX_READY))
        ;
    return JOY_DATA;
}

/* Wait for the card to pulse /ACK low, with a timeout so a missing card cannot
 * wedge the loop. Returns 1 on an acknowledgement, 0 on timeout. */
static int
wait_ack(void)
{
    int t = 0x20000;
    while (t-- > 0)
        if (!(JOY_STAT & STAT_ACK))
            return 1;
    return 0;
}

static void
port_open(void)
{
    JOY_CTRL = CTRL_RESET;
    busy(10);
    JOY_CTRL = 0;
    JOY_MODE = 0x000Du;                 /* 8-bit, baud reload factor 1 */
    JOY_BAUD = 0x0088u;
    JOY_CTRL = CTRL_TXEN | CTRL_RXEN | CTRL_SELECT | CTRL_ACKIRQ;
    busy(10);
}

static void
port_close(void)
{
    JOY_CTRL = 0;
}

int
psx_mc_read_frame(int frame, void *buf)
{
    uint8_t *dst = buf;
    uint8_t msb = (uint8_t)((frame >> 8) & 0xFF);
    uint8_t lsb = (uint8_t)(frame & 0xFF);

    port_open();

    xfer(0x81);                         /* select the memory card */
    if (!wait_ack()) { port_close(); return -1; }
    xfer(0x52);                         /* 'R': read a frame */
    wait_ack();
    xfer(0x00);                         /* card id 0x5A */
    wait_ack();
    xfer(0x00);                         /* card id 0x5D */
    wait_ack();
    xfer(msb);                          /* frame address, high then low */
    wait_ack();
    xfer(lsb);
    wait_ack();
    xfer(0x00);                         /* command acknowledge 0x5C */
    wait_ack();
    xfer(0x00);                         /* command acknowledge 0x5D */
    wait_ack();
    uint8_t amsb = xfer(0x00);          /* the card echoes the address back */
    wait_ack();
    uint8_t alsb = xfer(0x00);
    wait_ack();

    uint8_t sum = amsb ^ alsb;
    for (int i = 0; i < MC_FRAME_SIZE; i++) {
        dst[i] = xfer(0x00);
        sum ^= dst[i];
        wait_ack();
    }
    uint8_t chk = xfer(0x00);           /* checksum */
    wait_ack();
    uint8_t end = xfer(0x00);           /* end flag */

    port_close();

    if (amsb != msb || alsb != lsb)     /* the card did not seek to our frame */
        return -2;
    if (end != MC_GOOD)
        return -3;
    if (chk != sum)                     /* corrupt frame */
        return -4;
    return 0;
}

int
psx_mc_write_frame(int frame, const void *buf)
{
    const uint8_t *src = buf;
    uint8_t msb = (uint8_t)((frame >> 8) & 0xFF);
    uint8_t lsb = (uint8_t)(frame & 0xFF);
    uint8_t sum = msb ^ lsb;

    port_open();

    xfer(0x81);                         /* select the memory card */
    if (!wait_ack()) { port_close(); return -1; }
    xfer(0x57);                         /* 'W': write a frame */
    wait_ack();
    xfer(0x00);                         /* card id 0x5A */
    wait_ack();
    xfer(0x00);                         /* card id 0x5D */
    wait_ack();
    xfer(msb);                          /* frame address, high then low */
    wait_ack();
    xfer(lsb);
    wait_ack();
    for (int i = 0; i < MC_FRAME_SIZE; i++) {
        xfer(src[i]);
        sum ^= src[i];
        wait_ack();
    }
    xfer(sum);                          /* checksum */
    wait_ack();
    xfer(0x00);                         /* command acknowledge 0x5C */
    wait_ack();
    xfer(0x00);                         /* command acknowledge 0x5D */
    wait_ack();
    uint8_t end = xfer(0x00);           /* end flag: 0x47 good */

    port_close();

    return end == MC_GOOD ? 0 : -3;
}

int
psx_mc_present(void)
{
    /* A frame 0 read succeeds only if a card answered. */
    uint8_t frame[MC_FRAME_SIZE];
    return psx_mc_read_frame(0, frame) == 0 ? 1 : 0;
}
