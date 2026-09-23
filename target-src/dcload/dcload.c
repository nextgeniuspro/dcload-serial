/*
 * dcload, a Dreamcast serial loader
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

#include "sci.h"
#include "minilzo.h"
#include "video.h"

#include <string.h>

#define NAME "dcload-serial " DCLOAD_VERSION

/* The rate dcload boots at and returns to. Overridable from Makefile.cfg
   (DCLOAD_INITIAL_SPEED) so a fixed-rate disc can be built. 312500 is the safe
   choice: it is exact on the SCI's divider and gives the FIFO-less receiver
   32 us per byte of slack. dc-tool raises it with the 'S' command afterwards.

   THE RULE THIS FILE FOLLOWS, because the SCI has a single-byte receive
   register: the host may only send a multi-byte burst in reply to a byte we
   sent *after* finishing all slow work (video, LZO). So every command does its
   screen work first, then echoes the command byte, then reads its arguments;
   and every block ack ('G') goes out only once the progress bar is drawn. */
#ifndef INITIAL_SPEED
#define INITIAL_SPEED   312500
#endif

#define VIDMODEREG (volatile unsigned int *)0xa05f8044
#define VIDBORDER (volatile unsigned int *)0xa05f8040

extern void cdfs_redir_save(void);
extern void cdfs_redir_disable(void);
extern void cdfs_redir_enable(void);

/* buffer for storing compressed data (16384 + 16384 / 64 + 16 + 3 bytes) */
unsigned char *buffer = (unsigned char *) 0x8c009f6c;

/* work memory for compressing data (65536 bytes) */
unsigned char *wrkmem = 0;

extern unsigned int console_nowait;

/* Reads made on the host's behalf while a *program* is running normally wait
   forever: a program blocked in read(0, ...) is waiting for someone to type,
   and gdbpacket() waits for a debugger. But once the host has started a
   block -- sent its header, or been sent ours and owes a 'G' -- it has no
   reason to fall silent for seconds. If it does, it is gone (killed, crashed,
   lost the link), and the program can never continue anyway. So those waits
   get a limit, and when it fires we abandon the program and go back to the
   loader, which is what the person at the host would do with the reset
   button otherwise. dcload's own commands set a limit up front (see main()),
   so this only changes anything in program context. */
#define IN_BLOCK_TIMEOUT    15000000    /* polls; about five seconds */

extern void disable_cache(void);
extern void go(unsigned int addr);

static unsigned int block_timeout_begin(void) {
    unsigned int prev = sci_timeout_limit;

    if(!prev)
        sci_timeout_limit = IN_BLOCK_TIMEOUT;

    return prev;
}

static void block_timeout_end(unsigned int prev) {
    sci_timeout_limit = prev;

    if(sci_aborted && !prev) {
        /* Program context: nothing to return to. Back to the loader. */
        sci_aborted = 0;
        disable_cache();
        go(0x8c004000);
    }
}

#define BOOTED        1
#define NOT_BOOTED   -1
#define IS(a)        ((a) == BOOTED)
#define IS_NOT(a)    ((a) == NOT_BOOTED)
unsigned int booted = NOT_BOOTED;

void assign_wrkmem(unsigned char *user_buffer) {
    wrkmem = user_buffer;
}

/* converts expevt value to description, used by exception handler */
unsigned char * exception_code_to_string(unsigned int expevt) {
    switch(expevt) {
        case 0x1e0:
            return "User break";
        case 0x0e0:
            return "Address error (read)";
        case 0x040:
            return "TLB miss exception (read)";
        case 0x0a0:
            return "TLB protection violation exception (read)";
        case 0x180:
            return "General illegal instruction exception";
        case 0x1a0:
            return "Slot illegal instruction exception";
        case 0x800:
            return "General FPU disable exception";
        case 0x820:
            return "Slot FPU disable exception";
        case 0x100:
            return "Address error (write)";
        case 0x060:
            return "TLB miss exception (write)";
        case 0x0c0:
            return "TLB protection violation exception (write)";
        case 0x120:
            return "FPU exception";
        case 0x080:
            return "Initial page write exception";
        case 0x160:
            return "Unconditional trap (TRAPA)";
        default:
            return "Unknown exception";
    }
}

void uint_to_string(unsigned int foo, unsigned char *bar) {
    char hexdigit[16] = "0123456789abcdef";
    int i;

    for(i=7; i>=0; i--) {
        bar[i] = hexdigit[(foo & 0x0f)];
        foo = foo >> 4;
    }

    bar[8] = 0;
}

void put_uint(unsigned int val) {
    scif_putchar(val & 0xff);
    scif_putchar((val >> 8) & 0xff);
    scif_putchar((val >> 16) & 0xff);
    scif_putchar((val >> 24) & 0xff);
}

/* set n lines starting at line y to value c */
void clear_lines(unsigned int y, unsigned int n, unsigned int c) {
    memset((unsigned int *)0xa5000000 + y * 640 / 2, c, n * 640 * 2);
}

/* get an unsigned int from pc, without echoing it back yet. The echo is
   what releases the host's next burst, so a caller that has slow work to do
   between the two reads first, works, then calls put_uint() itself. */
unsigned int get_uint_noecho(void) {
    unsigned int retval;

    retval = 0;
    retval += (scif_getchar()) << 24;
    retval >>= 8;
    retval += (scif_getchar()) << 24;
    retval >>= 8;
    retval += (scif_getchar()) << 24;
    retval >>= 8;
    retval += (scif_getchar()) << 24;

    return retval;
}

/* get an unsigned int from pc and echo it */
unsigned int get_uint(void) {
    unsigned int retval = get_uint_noecho();

    put_uint(retval);

    return retval;
}

/* send an uncompressed data block to the pc from addr */
unsigned int send_data_block_uncompressed(unsigned char * addr, unsigned int size) {
    unsigned int i;
    unsigned char *location = addr;
    unsigned char sum = 0;
    unsigned char data;

    scif_putchar('U');
    put_uint(size);

    for(i = 0; i < size; i++) {
        data = *(location++);
        scif_putchar(data);
        sum ^= data;
    }

    scif_putchar(sum);
    {
        unsigned int prev = block_timeout_begin();

        data = scif_getchar();
        block_timeout_end(prev);
    }

    if(data == 'G')
        return 1;
    else
        return 0;
}

/* send a compressed data block to the pc from addr
 * falls back to uncompressed if compression unavailable or not beneficial
 */
unsigned int send_data_block_compressed(unsigned char * addr, unsigned int size) {
    unsigned int i;
    unsigned char *location = addr;
    unsigned char sum = 0;
    unsigned char data;
    lzo_uint csize;
    unsigned int sendsize;

    /* send uncompressed if no work memory provided */
    if(!wrkmem)
        return(send_data_block_uncompressed(addr, size));

    /* send uncompressed if too small to bother with */
    if(size < 19)
        return(send_data_block_uncompressed(addr, size));

    while(size && !sci_aborted) {
        if(size > 8192)
            sendsize = 8192;
        else
            sendsize = size;
        lzo1x_1_compress(addr, sendsize, buffer, &csize, wrkmem);
        if(csize < sendsize) {
            /* send compressed */
            scif_putchar('C');
            put_uint(csize);
            data = 'B';
            while(data != 'G' && !sci_aborted) {
                location = buffer;
                for(i = 0; i < csize; i++) {
                    data = *(location++);
                    scif_putchar(data);
                    sum ^= data;
                }

                scif_putchar(sum);
                {
                    unsigned int prev = block_timeout_begin();

                    data = scif_getchar();
                    block_timeout_end(prev);
                }
            }
        } else {
            /* send uncompressed */
            while(!(send_data_block_uncompressed(addr, sendsize)) && !sci_aborted);
        }
        size -= sendsize;
        addr += sendsize;
    }

    return 1;
}

void draw_progress(unsigned int current, unsigned int total) {
    unsigned char current_string[9];
    unsigned char total_string[9];

    uint_to_string(total, total_string);
    uint_to_string(current, current_string);
    clear_lines(72, 24, 0);
    draw_string(0, 72, "(", 0xffff);
    draw_string(12, 72, current_string, 0xffff);
    draw_string(108, 72, "/", 0xffff);
    draw_string(120, 72, total_string, 0xffff);
    draw_string(216, 72, ")", 0xffff);
}

void load_data_block_general(unsigned char * addr, unsigned int total, unsigned int verbose) {
    unsigned char type, sum, ok;
    lzo_uint size, newsize, realtotal;
    unsigned char *tmp = buffer;
    int i;
    unsigned char *data = addr;

    if(verbose)
        realtotal = total;

    /* The host starts the next block the instant it sees our 'G', and the
       SCI can hold exactly one byte for us while we are busy. So the progress
       bar is drawn *before* the ack, never after it, and nothing slow sits
       between reading the size and reading the data. */
    while(total && !sci_aborted) {
        unsigned int prev;

        type = scif_getchar();          /* may legitimately wait for hours */

        prev = block_timeout_begin();
        size = get_uint();

        if(sci_aborted) {
            block_timeout_end(prev);    /* does not return in program context */
            break;
        }

        switch (type) {
            case 'U':               /* uncompressed */
                if(size > total)
                    size = total;   /* a host bug must not scribble past the end */
                for(i=0; i<size && !sci_aborted; i++)
                    *(data++) = scif_getchar();
                sum = scif_getchar();
                (void)sum;
                if(sci_aborted)
                    break;
                total -= size;
                if(verbose)
                    draw_progress(realtotal - total, realtotal);
                scif_putchar('G');
                break;
            case 'C':               /* compressed */
                if(size > 16384 + 16384 / 64 + 16 + 3)
                    break;          /* larger than the buffer: not ours */
                /* On a bad decompress the host resends the same payload and
                   checksum with no new header, so loop here rather than
                   falling back to the type byte and losing sync. */
                for(;;) {
                    for(i=0; i<size && !sci_aborted; i++)
                        tmp[i] = scif_getchar();
                    sum = scif_getchar();
                    (void)sum;
                    if(sci_aborted)
                        break;
                    if(lzo1x_decompress(tmp, size, data, &newsize, 0) == LZO_E_OK) {
                        ok = 'G';
                        total -= newsize;
                        data += newsize;
                        if(verbose)
                            draw_progress(realtotal - total, realtotal);
                        scif_putchar(ok);
                        break;
                    }
                    ok = 'B';
                    scif_putchar(ok);
                }
                break;
            default:
                break;
        }

        block_timeout_end(prev);        /* does not return if it fired */
    }
}

void setup_video(unsigned int mode, unsigned int color) {
    init_video(check_cable(), mode);
    clrscr(color);
}

/* ---------------------------------------------------------------------------
 * Streaming load ('b').
 *
 * The 'B' protocol acknowledges every 16 KB block, and over Wi-Fi each ack is
 * a round trip that costs as much as the block itself. 'b' sends a whole
 * batch of blocks back to back and acknowledges the batch.
 *
 * The catch is the SCI's single-byte receiver: nothing slow may happen while
 * the batch is arriving. So during a batch the target only stores bytes --
 * uncompressed blocks straight to their destination, compressed ones into a
 * scratch area the host picked (clear of every section it is loading) -- and
 * keeps a running sum. Checking, decompressing and drawing happen after the
 * batch's end marker, while the host is waiting for the ack.
 *
 * Wire format, after the 'b' echo, addr (echoed), size (echoed after the
 * first progress draw), scratch address and scratch length (echoed):
 *
 *   batch := block* 'E' sum32
 *   block := type('U'|'C') wire_len32 raw_len32 payload[wire_len]
 *   reply := 'G' (committed) | 'B' (discarded; host resends the batch)
 *
 * All integers little-endian and not echoed; sum32 is the byte sum of all
 * payloads in the batch.
 * ------------------------------------------------------------------------- */

#define STREAM_MAX_BLOCKS   128

typedef struct {
    unsigned char *src;
    unsigned int   wire_len;
    unsigned char *dst;
    unsigned int   raw_len;
} stream_blk_t;

static stream_blk_t stream_blk[STREAM_MAX_BLOCKS];

/* After a malformed batch: swallow bytes until the host has stopped sending
   (it waits for our reply after the end marker), so the next batch starts on
   a clean boundary. ~2 ms of silence. */
static void drain_until_quiet(void) {
    unsigned int quiet = 0;

    while(quiet < 20000 && !sci_aborted) {
        if(scif_isdata()) {
            (void)scif_getchar();
            quiet = 0;
        }
        else {
            quiet++;
        }
    }
}

static void load_stream(unsigned char *addr, unsigned int total,
                        unsigned char *scratch, unsigned int scratch_len) {
    unsigned char *committed = addr;
    unsigned char *end = addr + total;

    while(committed < end && !sci_aborted) {
        unsigned char *dst = committed;
        unsigned char *sp = scratch;
        unsigned int n = 0, sum = 0, i;
        int ok = 1;
        unsigned char type;

        for(;;) {
            unsigned int wire_len, raw_len;

            type = scif_getchar();
            if(sci_aborted || type == 'E')
                break;

            wire_len = get_uint_noecho();
            raw_len = get_uint_noecho();

            /* dc-tool never sends a block over 64 KB. Anything bigger is a
               corrupted header, and reading it would take forever. */
            if(wire_len > 65536 || raw_len > 65536) {
                ok = 0;
                drain_until_quiet();
                break;
            }

            if(type == 'U') {
                if(wire_len != raw_len || dst + raw_len > end)
                    ok = 0;
                for(i = 0; i < wire_len && !sci_aborted; i++) {
                    unsigned char b = scif_getchar();
                    sum += b;
                    if(ok)
                        dst[i] = b;
                }
            }
            else if(type == 'C') {
                if(n >= STREAM_MAX_BLOCKS || sp + wire_len > scratch + scratch_len ||
                   dst + raw_len > end)
                    ok = 0;
                for(i = 0; i < wire_len && !sci_aborted; i++) {
                    unsigned char b = scif_getchar();
                    sum += b;
                    if(ok)
                        sp[i] = b;
                }
                if(ok) {
                    stream_blk[n].src = sp;
                    stream_blk[n].wire_len = wire_len;
                    stream_blk[n].dst = dst;
                    stream_blk[n].raw_len = raw_len;
                    n++;
                    sp += wire_len;
                }
            }
            else {
                /* Lost the framing: nothing after this can be trusted. */
                ok = 0;
                drain_until_quiet();
                break;
            }

            dst += raw_len;
        }

        if(sci_aborted)
            return;

        if(type == 'E' && get_uint_noecho() != sum)
            ok = 0;

        /* The host is waiting now: safe to be slow. */
        for(i = 0; ok && i < n; i++) {
            lzo_uint out_len = stream_blk[i].raw_len;

            if(lzo1x_decompress_safe(stream_blk[i].src, stream_blk[i].wire_len,
                                     stream_blk[i].dst, &out_len, 0) != LZO_E_OK ||
               out_len != stream_blk[i].raw_len)
                ok = 0;
        }

        if(ok) {
            committed = dst;
            draw_progress((unsigned int)(committed - addr), total);
            scif_putchar('G');
        }
        else {
            scif_putchar('B');
        }
    }
}

/* Bring the screen up if a program left us without one. clrscr() is 600 KB of
   uncached stores -- tens of milliseconds -- so it must never run between an
   echo and the arguments that follow it. */
static void ensure_video(void) {
    if(IS_NOT(booted)) {
        setup_video(0,0);
        draw_string(0, 24, NAME, 0xffff);
        booted = BOOTED;
    }
}

static void status_line(const char *s) {
    clear_lines(48, 24, 0);
    draw_string(0, 48, (unsigned char *)s, 0xffff);
}


int main(void) {
    unsigned char crap;
    unsigned int addr;
    unsigned int size;
    unsigned int console;
    unsigned int start;

    scif_init(INITIAL_SPEED);

    cdfs_redir_save(); /* will only save value once */
    cdfs_redir_disable();

    if(IS_NOT(booted)) {
        setup_video(0,0);
        draw_string(0, 24, NAME, 0xffff);
        booted = BOOTED;
    } else {
        booted = NOT_BOOTED;
    }

    lzo_init();

    wrkmem = 0;

    /* A new dc-tool may not know command 23; it has to ask again. */
    console_nowait = 0;

    while(1) {
        *VIDBORDER = 0x00ffffff;
        if(IS(booted))
            status_line("idle...");

        /* Not echoed yet. A single byte is always safe to leave in RDR; the
           4-byte arguments that follow the echo are not. Each command does
           its screen work first and echoes when it is ready to receive. */
        sci_timeout_limit = 0;              /* idle: wait forever */
        crap = scif_getchar();

        /* From here until the command completes, a silent host means it has
           gone away (killed, crashed, lost the link). Give up after a few
           seconds and go back to the prompt at the boot rate, which is also
           where the bridge goes when its session ends. ~100 ns per poll. */
        sci_timeout_limit = 15000000;

        switch(crap) {
            case 'A': /* execute */
                ensure_video();
                status_line("executing...");
                scif_putchar(crap);
                addr = get_uint();
                console = get_uint();

                if(sci_aborted)
                    break;

                /* Programs wait for the host as long as they like. */
                sci_timeout_limit = 0;

                if(console)
                    *(unsigned int *)0x8c004004 = 0xdeadbeef; /* enable console */
                else
                    *(unsigned int *)0x8c004004 = 0xfeedface; /* disable console */

                scif_flush();
                disable_cache();
                go(addr);
                break;
            case 'B': /* load binary */
                ensure_video();
                status_line("receiving data...");
                scif_putchar(crap);
                addr = get_uint();
                /* The size echo is what releases the host's first data
                   block, so the initial progress bar has to be drawn before
                   it goes out. */
                size = get_uint_noecho();
                draw_progress(0, size);
                put_uint(size);
                load_data_block_general((unsigned char *)addr, size, 1);
                break;
            case 'b': /* load binary, streamed in batches */
                ensure_video();
                status_line("receiving data (streaming)...");
                scif_putchar(crap);
                addr = get_uint();
                size = get_uint_noecho();
                draw_progress(0, size);
                put_uint(size);
                {
                    unsigned int scratch = get_uint();
                    unsigned int scratch_len = get_uint();

                    if(!sci_aborted)
                        load_stream((unsigned char *)addr, size,
                                    (unsigned char *)scratch, scratch_len);
                }
                break;
            case 'D': /* send uncompressed binary */
                ensure_video();
                status_line("sending uncompressed data...");
                /* fall through */
            case 'E': /* send uncompressed binary, don't write to screen */
                scif_putchar(crap);
                addr = get_uint();
                size = get_uint();
                send_data_block_uncompressed((unsigned char *)addr, size);
                break;
            case 'F': /* send compressed binary */
                ensure_video();
                status_line("sending compressed data...");
                /* fall through */
            case 'G': /* send compressed binary, don't write to screen */
                scif_putchar(crap);
                addr = get_uint();
                size = get_uint();
                wrkmem = (unsigned char *)get_uint();
                send_data_block_compressed((unsigned char *)addr, size);
                wrkmem = 0;
                break;
            case 'W': /* host understands fire-and-forget console writes */
                scif_putchar(crap);
                console_nowait = 1;
                break;
            case 'H': /* enable cdfs redir */
                scif_putchar(crap);
                cdfs_redir_enable();
                break;
            case 'S': /* change serial speed */
                scif_putchar(crap);
                addr = get_uint();
                scif_flush();
                scif_init(addr);
                addr = get_uint();
                put_uint(addr);
                break;
            case 'V': /* version */
                scif_putchar(crap);
                scif_puts(NAME);
                scif_puts("\n");
                break;
            default:
                scif_putchar(crap);
                scif_init(INITIAL_SPEED);
                break;
        }

        if(sci_aborted) {
            sci_aborted = 0;
            scif_init(INITIAL_SPEED);
            ensure_video();
            status_line("host went quiet; back at boot rate");
        }
    }
}
