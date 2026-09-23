/*
 * dcload-serial over the SH-4's SCI (channel 1), asynchronous mode.
 *
 * A drop-in replacement for scif.c. It keeps the scif_* names so that nothing
 * else in dcload has to change, but drives SCI instead of SCIF.
 *
 * Why: the SCI's TXD1/RXD1 sit on motherboard pads (R122/R115) rather than on
 * the serial connector, so a device soldered there can carry dcload while the
 * connector stays completely free. The esp32-dc adapter is wired to exactly
 * those pads, and in its "uart" mode it bridges them to TCP over Wi-Fi -- so
 * this is dcload-serial with no cable and no connector used.
 *
 * Asynchronous on purpose. The SCI can also do clocked-synchronous transfers,
 * which are four times faster, but those need the clock and chip-select lines
 * as well; async needs only the two data wires, is self-clocking, and has no
 * framing state to lose.
 *
 * Speed is identical to the SCIF path: both derive from the same 50 MHz
 * peripheral clock and top out at 1,562,500 baud (n=0, BRR=0, zero error).
 *
 * THE ONE THING TO REMEMBER ABOUT THIS PERIPHERAL: it has no receive FIFO.
 * A single byte sits in RDR; the next one to complete while it is still there
 * is lost and ORER latches. At 1,562,500 baud that is 6.4 us of slack. So the
 * code that *uses* this driver must never be doing slow work (video, LZO)
 * while the host may be sending -- and the host only sends bursts in reply to
 * a byte we sent. dcload.c is arranged around that rule.
 */

#include "sci.h"
#include "video.h"

/* SCSMR1 bits. 0x00 is what we want throughout: asynchronous, 8 data bits,
   no parity, one stop bit, and CKS = 0 for the fastest clock divider. */
#define SMR_ASYNC_8N1   0x00

/* SCSCR1 */
#define SCSCR_TE        0x20    /* transmit enable */
#define SCSCR_RE        0x10    /* receive enable  */

/* SCSSR1 */
#define SCSSR_TDRE      0x80    /* transmit data register empty */
#define SCSSR_RDRF      0x40    /* receive data register full   */
#define SCSSR_ORER      0x20    /* overrun  */
#define SCSSR_FER       0x10    /* framing  */
#define SCSSR_PER       0x08    /* parity   */
#define SCSSR_TEND      0x04    /* transmit end: shift register empty too */
#define SCSSR_ERRORS    (SCSSR_ORER | SCSSR_FER | SCSSR_PER)

/* Module standby: the SCI comes out of reset stopped, unlike the SCIF. */
#define STBCR_SCI_STP   0x01


/* A small software receive FIFO, filled whenever the transmitter is busy.
 *
 * The one place this driver is guaranteed to be looking away from RDR is
 * while it is sending: scif_putchar() waits for TDRE, and at 1.5 Mbaud a
 * 4-byte echo keeps it there for 26 us -- four incoming byte times. So while
 * it waits, it polls RDR and parks whatever arrives here, and scif_getchar()
 * drains this first. That is what lets the host send a command's arguments
 * back to back instead of one echo round trip at a time. */
#define RXQ_SIZE    64
static unsigned char rxq[RXQ_SIZE];
static unsigned int  rxq_w, rxq_r;

/* Give-up timer for reads made on the host's behalf. Zero means wait
   forever, which is right while idle and while a program is running; dcload's
   own commands set a limit so a host that dies mid-transfer leaves us back at
   the prompt rather than waiting for bytes that will never come. Counted in
   polls of SCSSR1, roughly 100 ns each. */
unsigned int sci_timeout_limit = 0;

/* The rate the port was last set to, so it can be restored as it was. */
static int sci_cur_bps = 0;
unsigned int sci_aborted = 0;

static inline void rxq_poll(void) {
    unsigned char st = *SCSSR1;

    if(st & SCSSR_ERRORS) {
        (void)*SCRDR1;
        *SCSSR1 = (unsigned char)(st & ~(SCSSR_ERRORS | SCSSR_RDRF));
        return;
    }

    if(st & SCSSR_RDRF) {
        unsigned char b = *SCRDR1;

        *SCSSR1 = (unsigned char)(*SCSSR1 & ~SCSSR_RDRF);

        if(rxq_w - rxq_r < RXQ_SIZE)
            rxq[rxq_w++ % RXQ_SIZE] = b;

    }
}

/* A delay the optimiser cannot remove. The old `for(i<100000;i++);` loops
   compiled to nothing at -O2, which is how the "wait one bit time before
   enabling the transceiver" step silently vanished. ~2 cycles per iteration
   at 200 MHz, so 1000 iterations is about 10 us. */
static void spin(unsigned int n) {
    volatile unsigned int i;

    for(i = 0; i < n; i++)
        ;
}

void scif_init(int bps) {
    unsigned char status;

    if(bps)
        sci_cur_bps = bps;

    /* Bring the SCI out of module standby. Missing this is the classic way to
       have every register read back as zero and nothing ever transmit. */
    if(*STBCR & STBCR_SCI_STP) {
        *STBCR &= ~STBCR_SCI_STP;
        spin(10000);
    }

    /* If the transmitter is running, let the byte in the shift register go
       out before pulling TE. This is what used to truncate the last byte of
       dcexit's reply and of the 'S' echo on every re-init. */
    if(*SCSCR1 & SCSCR_TE) {
        unsigned int guard = 2000000;   /* ~20 ms; never hang here */

        while(!(*SCSSR1 & SCSSR_TEND) && --guard)
            ;
    }

    /* Hold TXD1 at mark (high) as a plain output for the duration. With TE
       clear the transmitter lets go of the pin, and a floating line looks
       like a break to the far end: the bridge received a spurious 0x00 on
       every re-initialisation. SPB1IO (bit 1) makes the pin a GPIO driven
       from SPB1DT (bit 0); the transmitter takes it back when TE is set. */
    *SCSPTR1 = 0x03;

    /* Disable transmit and receive while we reconfigure. Clearing the whole
       register also selects the internal clock (CKE = 00), which for
       asynchronous mode leaves SCK1 unused -- we only need TXD1 and RXD1. */
    *SCSCR1 = 0;

    *SCSMR1 = SMR_ASYNC_8N1;

    /* BRR = PCLK / (32 * bps) - 1 for CKS = 0.
       At 1562500: 50000000 / 50000000 - 1 = 0, exactly on rate. */
    if(bps)
        *SCBRR1 = (unsigned char)((50 * 1000000) / (32 * bps) - 1);

    /* At least one bit interval must pass before enabling the transceiver:
       3.2 us at 312500, 104 us at 9600. */
    spin(20000);

    /* Clear stale errors only. These bits clear by writing 0 after reading 1,
       so the read matters -- but blanket-zeroing the register would also clear
       TDRE, and on this peripheral clearing TDRE is what signals "a byte has
       been placed in TDR, start sending it". Doing that with nothing in TDR
       invites a spurious first byte. */
    status = *SCSSR1;

    if(status & (SCSSR_ERRORS | SCSSR_RDRF)) {
        (void)*SCRDR1;
        *SCSSR1 = (unsigned char)(status & ~(SCSSR_ERRORS | SCSSR_RDRF));
    }

    rxq_w = rxq_r = 0;      /* anything queued was at the old rate */

    *SCSCR1 = SCSCR_TE | SCSCR_RE;

    /* Hand the pin back to the transceiver. If anything had left SPB1IO set
       (KallistiOS clears it in sci_init() but not in sci_shutdown()), the
       SCI would receive perfectly well and be completely mute -- which looks
       exactly like a dead cable. */
    *SCSPTR1 = 0;

    spin(2000);
}

/* A loaded program may have used the SCI itself -- KallistiOS's sci_init()
   for SPI, or sci_shutdown(), which puts the module in standby -- and then
   make a console syscall through us. Writing to a port in that state never
   completes. So every byte first checks the port is still ours, and puts it
   back at the current rate if not. Three register reads: cheap next to the
   6.4 us a byte takes on the wire. */
static inline void sci_ensure(void) {
    if((*STBCR & STBCR_SCI_STP) || *SCSMR1 != SMR_ASYNC_8N1 ||
       (*SCSCR1 & (SCSCR_TE | SCSCR_RE)) != (SCSCR_TE | SCSCR_RE) ||
       (*SCSPTR1 & 0x02))
        scif_init(sci_cur_bps);
}

void scif_flush(void) {
    /* The SCI has no FIFO -- a single byte register each way -- so there is
       nothing to drain. What callers want is "the last byte has left the
       pin", which is TEND, not TDRE: TDRE is set the moment TDR is copied
       into the shift register, a full byte time before the line goes idle. */
    while(!(*SCSSR1 & SCSSR_TEND))
        rxq_poll();
}

unsigned char scif_getchar(void) {
    unsigned char foo;
    unsigned char status;
    unsigned int spins = 0;

#ifdef BORDER_FLASH
    *VIDBORDER = ~(*VIDBORDER & 0x00ffffff);
#endif

    if(sci_aborted)
        return 0;

    sci_ensure();

    if(rxq_w != rxq_r)
        return rxq[rxq_r++ % RXQ_SIZE];

    for(;;) {
        status = *SCSSR1;

        /* An overrun or framing error latches and stops reception dead until
           it is cleared, so clear it and carry on rather than hanging. The
           byte in RDR is thrown away with it: it is either the one that was
           overrun or a torn frame, and neither is trustworthy. */
        if(status & SCSSR_ERRORS) {
            (void)*SCRDR1;
            *SCSSR1 = (unsigned char)(status & ~(SCSSR_ERRORS | SCSSR_RDRF));
            continue;
        }

        if(status & SCSSR_RDRF)
            break;

        if(sci_timeout_limit && ++spins > sci_timeout_limit) {
            sci_aborted = 1;
            return 0;
        }
    }

    foo = *SCRDR1;
    *SCSSR1 = (unsigned char)(*SCSSR1 & ~SCSSR_RDRF);

    return foo;
}

unsigned int scif_isdata(void) {
    return (rxq_w != rxq_r) || (*SCSSR1 & SCSSR_RDRF);
}

void scif_putchar(unsigned char foo) {
#ifdef BORDER_FLASH
    *VIDBORDER = ~(*VIDBORDER & 0x00ffffff);
#endif

    sci_ensure();

    while(!(*SCSSR1 & SCSSR_TDRE))
        rxq_poll();

    *SCTDR1 = foo;
    *SCSSR1 = (unsigned char)(*SCSSR1 & ~SCSSR_TDRE);
}

void scif_puts(unsigned char *foo) {
    int i = 0;

    while(foo[i] != 0) {
        scif_putchar(foo[i]);
        if(foo[i] == '\n')
            scif_putchar('\r');
        i++;
    }
}
