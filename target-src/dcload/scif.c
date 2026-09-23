/*
 * This file is part of the dcload Dreamcast serial loader
 *
 * Copyright (C) 2001 Andrew Kieschnick <andrewk@napalm-x.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/*
 * dcload-serial over the SCIF -- the serial connector on the back.
 *
 * The SCI build (sci.c) is the esp32-dc bridge soldered to motherboard pads;
 * this is the same bridge, or any coder's cable, plugged into the connector.
 * It gives dcload.c the same contract sci.c does: a receive timeout for
 * host-driven transfers (sci_timeout_limit / sci_aborted, named for the SCI
 * build that introduced them), receive errors cleared rather than left to
 * stall the port, and repair of a port that a loaded program reconfigured.
 *
 * The SCIF is the easier peripheral: a 16-byte FIFO each way, so the SCI's
 * single-byte-receiver rules are met with room to spare.
 *
 * No hardware flow control. The original driver set MCE, which stops the
 * transmitter whenever CTS is high -- and the esp32-dc wiring, like most
 * three-wire cables, leaves CTS unconnected and floating.
 */

#include "scif.h"

/* #define BORDER_FLASH -- off, as in sci.c: a PVR write per byte adds up */
#define VIDBORDER (volatile unsigned int *)0xa05f8040

/* SCSCR2 */
#define SCSCR_TE        0x0020
#define SCSCR_RE        0x0010

/* SCFSR2 */
#define SCFSR_ER        0x0080  /* receive error (framing or parity)    */
#define SCFSR_TEND      0x0040  /* transmit end: FIFO and shifter empty */
#define SCFSR_TDFE      0x0020  /* transmit FIFO has room               */
#define SCFSR_BRK       0x0010  /* break                                */
#define SCFSR_FER       0x0008
#define SCFSR_PER       0x0004
#define SCFSR_RDF       0x0002  /* receive FIFO at trigger (1 byte)     */
#define SCFSR_DR        0x0001
#define SCFSR_ERRORS    (SCFSR_ER | SCFSR_BRK)

/* SCLSR2 */
#define SCLSR_ORER      0x0001  /* overrun; reception stops until cleared */

/* SCSPTR2: TXD2 as a GPIO driven high, for while the transmitter is off. */
#define SCSPTR_SPB2IO   0x0002
#define SCSPTR_SPB2DT   0x0001

/* SCFCR2 */
#define SCFCR_TFRST     0x0004
#define SCFCR_RFRST     0x0002

#define STBCR           (volatile unsigned char *)0xffc00004
#define STBCR_SCIF_STP  0x08

/* See sci.c. Polls of SCFSR2, roughly 100 ns each; 0 means wait forever. */
unsigned int sci_timeout_limit = 0;
unsigned int sci_aborted = 0;

static int scif_cur_bps = 0;

/* A delay the optimiser cannot remove (~10 ns per iteration at 200 MHz). */
static void spin(unsigned int n) {
    volatile unsigned int i;

    for(i = 0; i < n; i++)
        ;
}

static inline void scif_clear_errors(void) {
    unsigned short st = *SCFSR2;

    if(st & SCFSR_ERRORS)
        *SCFSR2 = (unsigned short)(st & ~(SCFSR_ERRORS | SCFSR_FER | SCFSR_PER));

    if(*SCLSR2 & SCLSR_ORER)
        *SCLSR2 = 0;
}

void scif_flush(void) {
    int v;

    *SCFSR2 &= 0xbf;
    while(!((v = *SCFSR2) & 0x40));
    *SCFSR2 = v & 0xbf;
}

void scif_init(int bps) {
    /* Modified to allow external baudrate (bps == 0) */
    if(bps)
        scif_cur_bps = bps;

    if(*STBCR & STBCR_SCIF_STP) {
        *STBCR &= ~STBCR_SCIF_STP;
        spin(10000);
    }

    /* Let a byte still in the transmitter finish, or the last character of a
       reply sent just before a rate change is cut short. */
    if(*SCSCR2 & SCSCR_TE) {
        unsigned int guard = 2000000;

        while(!(*SCFSR2 & SCFSR_TEND) && --guard)
            ;
    }

    /* Hold TXD2 at mark while the transmitter is off, so the far end does not
       see a break. The transmitter takes the pin back when TE is set. */
    *SCSPTR2 = SCSPTR_SPB2IO | SCSPTR_SPB2DT;

    *SCSCR2 = bps ? 0x0 : 0x02;	/* clear TE and RE bits / if (bps == 0) CKE1 on (bit 1) */
    *SCFCR2 = SCFCR_TFRST | SCFCR_RFRST;
    *SCSMR2 = 0x0;		/* set data transfer format 8n1 */

    if(bps) *SCBRR2 = (50 * 1000000) / (32 * bps) - 1;	/* if (bps != 0) set baudrate */

    spin(20000);		/* at least one bit interval */

    *SCFCR2 = 0;		/* FIFOs running, 1-byte receive trigger, no MCE */
    *SCFSR2 = 0x60;
    *SCLSR2 = 0;
    *SCSCR2 = bps ? 0x30 : 0x32;	/* set TE and RE bits / if (bps == 0) CKE1 on (bit 1) */
    *SCSPTR2 = 0;

    spin(2000);
}

/* A loaded program may have taken the SCIF over -- the SD card and W5500
   drivers bit-bang it as SPI -- and then make a console syscall through us. */
static inline void scif_ensure(void) {
    if((*STBCR & STBCR_SCIF_STP) || *SCSMR2 != 0 ||
       (*SCSCR2 & (SCSCR_TE | SCSCR_RE)) != (SCSCR_TE | SCSCR_RE) ||
       (*SCSPTR2 & SCSPTR_SPB2IO) || (*SCFCR2 & (SCFCR_TFRST | SCFCR_RFRST)))
        scif_init(scif_cur_bps);
}

unsigned char scif_getchar(void) {
    unsigned char foo;
    unsigned int spins = 0;

#ifdef BORDER_FLASH
    *VIDBORDER = ~(*VIDBORDER & 0x00ffffff);
#endif

    if(sci_aborted)
        return 0;

    scif_ensure();

    for(;;) {
        unsigned short st = *SCFSR2;

        if(st & (SCFSR_RDF | SCFSR_DR))
            break;

        /* A framing error or an overrun latches and blocks the port. A torn
           byte is still in the FIFO and comes back like any other; the echo
           and checksum checks in dcload.c catch it. */
        if((st & SCFSR_ERRORS) || (*SCLSR2 & SCLSR_ORER))
            scif_clear_errors();

        if(sci_timeout_limit && ++spins > sci_timeout_limit) {
            sci_aborted = 1;
            return 0;
        }
    }

    foo = *SCFRDR2;		/* read data */
    *SCFSR2 &= 0xfffc;		/* clear RDF and DR */

    return foo;
}

unsigned int scif_isdata(void) {
    return (*SCFSR2 & (SCFSR_RDF | SCFSR_DR));
}

void scif_putchar(unsigned char foo) {
#ifdef BORDER_FLASH
    *VIDBORDER = ~(*VIDBORDER & 0x00ffffff);
#endif

    scif_ensure();

    while(!(*SCFSR2 & SCFSR_TDFE));	/* check TDFE */
    *SCFTDR2 = foo;		/* send data */
    *SCFSR2 &= 0xff9f;		/* clear TDFE and TEND */
}

void scif_puts(unsigned char *foo) {
    int i = 0;

    while(foo[i] != 0) {
        scif_putchar(foo[i]);
        if (foo[i] == '\n')
            scif_putchar('\r');
        i++;
    }
}
