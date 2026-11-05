/* pad.c - libpsx controller driver (milestone 2).
 *
 * Reads a digital controller over the documented SIO0 hardware, the serial
 * port the controllers and memory cards share. This is the direct-hardware
 * path: it does not use the BIOS pad routines, so it depends on no BIOS
 * revision. The read is polled, which is enough for a per-frame poll in the
 * main loop; an interrupt-driven read is a later refinement.
 *
 * The transfer to a digital pad is: send 0x01 (select the controller), then
 * 0x42 (the read command) and read back the pad id (0x41 for a digital pad),
 * a 0x5A "ready" byte, and two button bytes. The buttons are active-low on the
 * wire (a pressed button reads 0), so psx_pad_read inverts them to the usual
 * sense where a set bit means pressed.
 */

#include "psx.h"

#define JOY_DATA (*(volatile uint8_t  *)0xBF801040u)   /* TX/RX data */
#define JOY_STAT (*(volatile uint32_t *)0xBF801044u)   /* status */
#define JOY_MODE (*(volatile uint16_t *)0xBF801048u)   /* mode */
#define JOY_CTRL (*(volatile uint16_t *)0xBF80104Au)   /* control */
#define JOY_BAUD (*(volatile uint16_t *)0xBF80104Eu)   /* baud reload */

#define STAT_TX_READY 0x0001u   /* ready to accept a TX byte */
#define STAT_RX_READY 0x0002u   /* an RX byte is waiting */
#define STAT_ACK      0x0080u   /* /ACK input level from the device */

#define CTRL_TXEN   0x0001u     /* transmit enable */
#define CTRL_SELECT 0x0002u     /* assert /JOY select */
#define CTRL_RXEN   0x0004u     /* receive enable */
#define CTRL_ACKIRQ 0x0010u     /* acknowledge / reset the IRQ bits */
#define CTRL_RESET  0x0040u     /* reset the SIO */

/* State from the last psx_pad_read: the id (0xFF means no device answered), and
 * the four analog-stick axes (centered at 0x80 for a digital pad). */
static int last_id = 0xFF;
static uint8_t stick_rx = 0x80, stick_ry = 0x80;
static uint8_t stick_lx = 0x80, stick_ly = 0x80;

static void
busy(int n)
{
    volatile int i;
    for (i = 0; i < n; i++)
        ;
}

/* Exchange one byte: wait for TX ready, send, wait for the reply, return it. */
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

/* Wait for the device to pulse /ACK low after a byte, with a timeout so a
 * missing controller cannot wedge the loop. */
static void
wait_ack(void)
{
    int t = 0x10000;
    while (t-- > 0) {
        if (!(JOY_STAT & STAT_ACK))
            return;
    }
}

static void
port_open(void)
{
    JOY_CTRL = CTRL_RESET;              /* reset the port */
    busy(10);
    JOY_CTRL = 0;
    JOY_MODE = 0x000Du;                 /* 8-bit, baud reload factor 1 */
    JOY_BAUD = 0x0088u;                 /* ~250 kHz */
    JOY_CTRL = CTRL_TXEN | CTRL_RXEN | CTRL_SELECT | CTRL_ACKIRQ;
    busy(10);
}

uint16_t
psx_pad_read(void)
{
    port_open();

    xfer(0x01);                         /* select controller (reply is hi-z) */
    wait_ack();
    last_id = xfer(0x42);               /* read command; reply is the pad id */
    wait_ack();
    xfer(0x00);                         /* 0x5A ready byte */
    wait_ack();

    /* The id's low nibble is the number of 16-bit words of data that follow:
     * a digital pad (0x41) has one word (the two button bytes); an analog pad
     * in analog mode (0x73) has three words (buttons plus the four stick
     * axes). Read the first two as buttons, the next four, when present, as
     * the right then left stick. */
    int words = last_id & 0x0F;
    int nbytes = words * 2;

    uint8_t data[8];
    for (int i = 0; i < 8; i++)
        data[i] = 0xFF;
    for (int i = 0; i < nbytes && i < 8; i++) {
        data[i] = xfer(0x00);
        if (i < nbytes - 1)
            wait_ack();
    }

    JOY_CTRL = 0;                       /* deselect */

    if (nbytes >= 6) {                  /* analog: RX, RY, LX, LY follow */
        stick_rx = data[2];
        stick_ry = data[3];
        stick_lx = data[4];
        stick_ly = data[5];
    } else {                            /* digital: sticks read centered */
        stick_rx = stick_ry = stick_lx = stick_ly = 0x80;
    }

    uint16_t raw = (uint16_t)(((uint16_t)data[1] << 8) | data[0]);
    return (uint16_t)~raw;              /* active-high: 1 = pressed */
}

/* Send one controller command (select byte through the padded tail), pulsing
 * through the /ACK handshake. The responses during configuration are not
 * needed, so they are discarded. */
static void
send_cmd(const uint8_t *cmd, int n)
{
    port_open();
    for (int i = 0; i < n; i++) {
        xfer(cmd[i]);
        if (i < n - 1)
            wait_ack();
    }
    JOY_CTRL = 0;
}

void
psx_pad_init_analog(void)
{
    /* Enter configuration mode, turn on analog mode and lock it, then leave
     * configuration mode. After this an analog pad reports id 0x73 and its
     * stick axes in psx_pad_read. A digital-only pad ignores the sequence. */
    static const uint8_t enter[]  = { 0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t analog[] = { 0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t leave[]  = { 0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    send_cmd(enter, sizeof enter);
    send_cmd(analog, sizeof analog);
    send_cmd(leave, sizeof leave);
}

int
psx_pad_id(void)
{
    return last_id;
}

int psx_pad_stick_rx(void) { return stick_rx; }
int psx_pad_stick_ry(void) { return stick_ry; }
int psx_pad_stick_lx(void) { return stick_lx; }
int psx_pad_stick_ly(void) { return stick_ly; }
