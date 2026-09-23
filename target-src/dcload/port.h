/*
 * The serial port dcload talks through, chosen at build time with
 * DCLOAD_PORT in Makefile.cfg: the SCI (motherboard pads, sci.c) or the SCIF
 * (the serial connector, scif.c). Both export the same scif_* names.
 */

#ifndef __PORT_H__
#define __PORT_H__

#ifdef DCLOAD_PORT_SCIF
#include "scif.h"
#else
#include "sci.h"
#endif

#endif /* __PORT_H__ */
