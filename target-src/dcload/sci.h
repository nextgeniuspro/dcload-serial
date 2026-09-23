/*
 * dcload-serial over the SH-4 SCI (channel 1). See sci.c.
 *
 * Deliberately exports the same names as scif.h so that dcload.c and
 * cdfs_syscalls.c need no changes -- only the object swapped in the Makefile.
 */

#ifndef __SCI_H__
#define __SCI_H__

/* SCI channel 1 registers. All eight bits wide, unlike the SCIF's mix. */

/* serial mode register */
#define SCSMR1  (volatile unsigned char *)  0xffe00000

/* bit rate register */
#define SCBRR1  (volatile unsigned char *)  0xffe00004

/* serial control register */
#define SCSCR1  (volatile unsigned char *)  0xffe00008

/* transmit data register */
#define SCTDR1  (volatile unsigned char *)  0xffe0000c

/* serial status register */
#define SCSSR1  (volatile unsigned char *)  0xffe00010

/* receive data register */
#define SCRDR1  (volatile unsigned char *)  0xffe00014

/* serial port register */
#define SCSPTR1 (volatile unsigned char *)  0xffe00018

/* standby control register -- the SCI starts in module standby */
#define STBCR   (volatile unsigned char *)  0xffc00004

/* Receive give-up limit (0 = never) and the flag scif_getchar() raises when it
   fires; every read returns 0 until the flag is cleared. See sci.c. */
extern unsigned int sci_timeout_limit;
extern unsigned int sci_aborted;

void scif_flush(void);
void scif_init(int bps);
unsigned char scif_getchar(void);
unsigned int scif_isdata(void);
void scif_putchar(unsigned char foo);
void scif_puts(unsigned char *foo);

#endif /* __SCI_H__ */
