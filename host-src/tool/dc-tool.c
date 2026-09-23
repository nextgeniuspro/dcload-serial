/*
 * dc-tool, a tool for use with the dcload serial loader
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

#include "config.h"

#ifdef WITH_BFD
#include <bfd.h>
#else
#include <libelf.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#ifdef __linux__
#ifndef __GLIBC__
/* musl libc */
#include <asm/ioctls.h>
#endif
#include <asm/termbits.h>
#include <sys/ioctl.h>
#else
#include <termios.h>
#endif
#endif

#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#ifdef __MINGW32__
#include <winsock.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#endif
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#include <sys/ioctl.h>
#endif
#ifdef __FreeBSD__
#include <netinet/in.h>
#endif
#include "minilzo.h"
#include "syscalls.h"
#include "dc-io.h"

int _nl_msg_cat_cntr;

/* GNU Debugger (GDB) */
int gdb_socket_started = 0;
#ifndef __MINGW32__
int gdb_server_socket = -1;
int socket_fd = 0;
#else
/* Winsock SOCKET is defined as an unsigned int, so -1 won't work here */
SOCKET gdb_server_socket = 0;
SOCKET socket_fd = 0;
#endif

/* The rate dcload boots at. Must match the target's DCLOAD_INITIAL_SPEED;
   both come from Makefile.cfg. Overridable at run time with -B. */
#ifndef INITIAL_SPEED
#define INITIAL_SPEED   312500
#endif

#define DCLOADBUFFER    16384 /* was 8192 */
#ifdef _WIN32
#define DATA_BITS       8
#define PARITY_SET      NOPARITY
#define STOP_BITS       ONESTOPBIT
#endif

#define VERSION PACKAGE_VERSION

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define HEAP_ALLOC(var,size) \
        long __LZO_MMODEL var [ ((size) + (sizeof(long) - 1)) / sizeof(long) ]

static HEAP_ALLOC(wrkmem, LZO1X_1_MEM_COMPRESS);

#ifndef HAVE_GETOPT
/* The following code for getopt is from the libc-source of FreeBSD,
 * it might be changed a little bit.
 * Florian Schulze (florian.proff.schulze@gmx.net)
 */

/*
 * Copyright (c) 1987, 1993, 1994
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by the University of
 *      California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

int     opterr = 1,             /* if error message should be printed */
        optind = 1,             /* index into parent argv vector */
        optopt,                 /* character checked for validity */
        optreset;               /* reset getopt */
char    *optarg;                /* argument associated with option */

#define BADCH   (int)'?'
#define BADARG  (int)':'
#define EMSG    ""

char *__progname=PACKAGE;

/*
 * getopt --
 *      Parse argc/argv argument vector.
 */
int getopt(int nargc, char * const *nargv, const char *ostr) {
    extern char *__progname;
    static char *place = EMSG;              /* option letter processing */
    char *oli;                              /* option letter list index */
    int ret;

    if(optreset || !*place) {              /* update scanning pointer */
        optreset = 0;
        if(optind >= nargc || *(place = nargv[optind]) != '-') {
            place = EMSG;
            return (-1);
        }
        if(place[1] && *++place == '-') {   /* found "--" */
            ++optind;
            place = EMSG;
            return (-1);
        }
    }

    /* option letter okay? */
    if((optopt = (int)*place++) == (int)':' ||
      !(oli = strchr(ostr, optopt))) {
        /*
        * if the user didn't specify '-' as an option,
        * assume it means -1.
        */
        if(optopt == (int)'-')
            return (-1);
        if(!*place)
            ++optind;
        if(opterr && *ostr != ':')
            (void)fprintf(stderr,
                "%s: illegal option -- %c\n", __progname, optopt);

        return (BADCH);
    }
    if(*++oli != ':') {                /* don't need argument */
        optarg = NULL;
        if(!*place)
            ++optind;
    }
    else {                              /* need an argument */
        if(*place)                     /* no white space */
            optarg = place;
        else if(nargc <= ++optind) {   /* no arg */
            place = EMSG;
            if(*ostr == ':')
                ret = BADARG;
            else
                ret = BADCH;

            if(opterr)
                (void)fprintf(stderr,
                  "%s: option requires an argument -- %c\n",
                  __progname, optopt);

            return (ret);
        }
        else                              /* white space */
            optarg = nargv[optind];

        place = EMSG;
        ++optind;
    }

    return (optopt);                       /* dump back option letter */
}
#else
#include <unistd.h>
#endif

extern char *optarg;
#ifndef _WIN32
int dcfd;
#ifdef BOTHER
typedef struct termios2 termiosx_t ;
#else
typedef struct termios termiosx_t;
#endif /* BOTHER */
termiosx_t oldtio;
#else
HANDLE hCommPort;
BOOL bDebugSocketStarted = FALSE;
#endif /* _WIN32 */

/* ------------------------------------------------------------------------- */
/* Transport                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * Two ways to reach dcload:
 *
 *   serial   a tty, as always (-t /dev/ttyUSB0)
 *   tcp      the esp32-dc bridge directly (-t tcp:esp32-dc.local[:2323])
 *
 * The bridge is a transparent byte pipe, so the dcload protocol is identical
 * either way. What differs is the baud rate: over TCP there is no tty to set
 * it on, so dc-tool tells the ESP32 through a small line-oriented control
 * port (data port + 1, default 2324) whenever dcload's rate changes. That is
 * what makes the 'S' speed change work through the bridge, and it also lets
 * dc-tool put the bridge back to INITIAL_SPEED at start-up, so a rate left
 * over from a previous run can never wedge the next one.
 */

typedef enum { XP_SERIAL, XP_TCP } transport_t;

static transport_t xport = XP_SERIAL;
unsigned int initial_speed = INITIAL_SPEED;
int io_timeout_ms = 5000;

/* Pipelining: send a block's header, size, payload and checksum in one write
   and only then collect the echo and the ack, instead of waiting for the size
   echo first. One round trip per block instead of two -- and over Wi-Fi the
   round trip is the cost. Only safe with a target that can take bytes while
   it is echoing (dcload-serial 1.0.8+, which has a software receive FIFO);
   detected at start-up with the 'V' command, never assumed. */
static int pipelined = 0;
static int no_pipeline = 0;

/* Streaming ('b', dcload-serial 1.0.9+): whole batches of blocks per ack.
   The target parks compressed blocks in a scratch area we choose, clear of
   everything being loaded; see load_stream() in the target's dcload.c. */
static int streaming = 0;
static int console_nowait = 0;             /* dcload 1.0.10+: command 23 */
static unsigned int cur_speed = 0;         /* line rate right now */

#define STREAM_SCRATCH_LEN  (512 * 1024)
#define STREAM_BATCH_WIRE   (256 * 1024)   /* bytes on the wire per ack */
#define STREAM_MAX_BLOCKS   128
#define STREAM_RAM_START    0x8c010000u
#define STREAM_RAM_END      0x8d000000u     /* 16 MB retail console */

#define MAX_RANGES 64
static unsigned int range_lo[MAX_RANGES], range_hi[MAX_RANGES];
static int nranges;
static unsigned int stream_scratch;         /* 0 = none found */

static void note_range(unsigned int lo, unsigned int size) {
    if(nranges < MAX_RANGES) {
        range_lo[nranges] = lo;
        range_hi[nranges] = lo + size;
        nranges++;
    }
    else {
        /* Too many to track safely: do not stream this upload. */
        nranges = MAX_RANGES + 1;
    }
}

/* Highest 64 KB-aligned window of RAM that no section touches. */
static void choose_scratch(void) {
    unsigned int top;

    stream_scratch = 0;

    if(nranges > MAX_RANGES)
        return;

    for(top = STREAM_RAM_END; top - STREAM_SCRATCH_LEN >= STREAM_RAM_START;
        top -= 0x10000) {
        unsigned int lo = top - STREAM_SCRATCH_LEN;
        int i, clash = 0;

        for(i = 0; i < nranges && !clash; i++) {
            /* compare on physical addresses; sections may be P1 or P2 */
            unsigned int a = (range_lo[i] & 0x1fffffff) | 0x80000000;
            unsigned int b = a + (range_hi[i] - range_lo[i]);

            if(a < top && b > lo)
                clash = 1;
        }

        if(!clash) {
            stream_scratch = lo;
            return;
        }
    }
}

#ifndef _WIN32
static int tcp_fd = -1;
static char tcp_host[256];
static unsigned int tcp_port = 2323;
static unsigned int tcp_ctl_port = 2324;
static int ctl_warned = 0;
static int ctl_available = 1;

/* "tcp:host[:port]" selects TCP outright; "host:port" does too when no such
   file exists. Everything else is a tty. */
static int parse_transport(const char *dev) {
    const char *p = dev;
    const char *colon;

    if(!strncmp(p, "tcp:", 4))
        p += 4;
    else if(!strchr(p, ':') || access(p, F_OK) == 0)
        return 0;

    colon = strrchr(p, ':');

    if(colon) {
        snprintf(tcp_host, sizeof(tcp_host), "%.*s", (int)(colon - p), p);
        tcp_port = strtoul(colon + 1, NULL, 10);
        if(!tcp_port)
            tcp_port = 2323;
    }
    else {
        snprintf(tcp_host, sizeof(tcp_host), "%s", p);
    }

    tcp_ctl_port = tcp_port + 1;
    xport = XP_TCP;

    return 1;
}

static int tcp_connect(const char *host, unsigned int port, int timeout_ms) {
    struct addrinfo hints, *res, *ai;
    char portstr[8];
    int fd = -1, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%u", port);

    rc = getaddrinfo(host, portstr, &hints, &res);
    if(rc) {
        fprintf(stderr, "dc-tool: %s: %s\n", host, gai_strerror(rc));
        return -1;
    }

    for(ai = res; ai; ai = ai->ai_next) {
        int fl, one = 1;

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if(fd < 0)
            continue;

        /* Non-blocking connect with a deadline: a wrong address should fail
           in seconds, not after the kernel's minutes-long default. */
        fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if(rc < 0 && errno == EINPROGRESS) {
            fd_set wf;
            struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };

            FD_ZERO(&wf);
            FD_SET(fd, &wf);

            if(select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
                int err = 0;
                socklen_t el = sizeof(err);

                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                errno = err;
                rc = err ? -1 : 0;
            }
            else {
                errno = ETIMEDOUT;
                rc = -1;
            }
        }

        if(rc == 0) {
            /* Remember the numeric address. Every control-port command is a
               fresh connection, and resolving a .local name through mDNS
               can take seconds each time -- long enough that dcload gives up
               waiting for a speed change to be confirmed. */
            if(host == tcp_host)
                getnameinfo(ai->ai_addr, ai->ai_addrlen, tcp_host,
                            sizeof(tcp_host), NULL, 0, NI_NUMERICHOST);
            fcntl(fd, F_SETFL, fl);
            /* Nagle would hold the 2nd..4th byte of every send_uint() until
               the ESP32's delayed ACK: mandatory, not an optimisation. */
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if(fd < 0)
        fprintf(stderr, "dc-tool: connect %s:%u: %s\n", host, port, strerror(errno));

    return fd;
}

/* One request, one reply line, on a fresh connection to the control port.
   Returns 0 on "ok ...", -2 on "err ..." (reply holds the text), and -1 when
   the port is unreachable -- older firmware, or a plain TCP-serial box -- in
   which case the bridge is assumed to be fixed at initial_speed. */
static int esp_ctl(char *reply, size_t n, const char *fmt, ...) {
    char line[128];
    va_list ap;
    int fd, len, got = 0;
    fd_set rf;
    struct timeval tv;

    reply[0] = '\0';

    fd = tcp_connect(tcp_host, tcp_ctl_port, 3000);
    if(fd < 0) {
        if(!ctl_warned) {
            fprintf(stderr, "dc-tool: ESP32 control port %u unreachable; "
                            "assuming the bridge is fixed at %u baud\n",
                    tcp_ctl_port, initial_speed);
            ctl_warned = 1;
        }
        ctl_available = 0;
        return -1;
    }

    va_start(ap, fmt);
    len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[len++] = '\n';

    if(write(fd, line, len) != len) {
        close(fd);
        return -1;
    }

    for(;;) {
        int r;

        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        tv.tv_sec = 3;
        tv.tv_usec = 0;

        if(select(fd + 1, &rf, NULL, NULL, &tv) <= 0)
            break;

        r = read(fd, reply + got, n - 1 - got);
        if(r <= 0)
            break;

        got += r;
        reply[got] = '\0';

        if(strchr(reply, '\n') || got >= (int)n - 1)
            break;
    }

    close(fd);

    reply[strcspn(reply, "\r\n")] = '\0';

    if(!strncmp(reply, "ok", 2))
        return 0;

    if(!got)
        snprintf(reply, n, "no reply");

    return -2;
}

static void transport_close(void) {
    if(tcp_fd >= 0) {
        close(tcp_fd);
        tcp_fd = -1;
    }
}
#endif /* !_WIN32 */

void cleanup(void) {
    if(!gdb_socket_started)
        return;

    gdb_socket_started = 0;

    // Send SIGTERM to the GDB Client, telling remote DC program has ended
    char gdb_buf[16];
    strcpy(gdb_buf, "+$X0f#ee\0");

#ifdef __MINGW32__
    send(socket_fd, gdb_buf, strlen(gdb_buf), 0);
    sleep(1);
    closesocket(socket_fd);
    closesocket(gdb_server_socket);
    WSACleanup();
#else
    write(socket_fd, gdb_buf, strlen(gdb_buf));
    sleep(1);
    close(socket_fd);
    close(gdb_server_socket);
#endif
}

#ifdef _WIN32
int serial_read(void *buffer, int count) {
    BOOL fSuccess;

    fSuccess = ReadFile(hCommPort, buffer, count, (DWORD *)&count, NULL);
    if(!fSuccess)
        return -1;

    return count;
}

int serial_write(void *buffer, int count) {
    BOOL fSuccess;

    fSuccess = WriteFile(hCommPort, buffer, count, (DWORD *)&count, NULL);
    if(!fSuccess)
        return -1;

    return count;
}

int serial_putc(char ch) {
    BOOL fSuccess;
    int count = 1;

    fSuccess = WriteFile(hCommPort, &ch, count, (DWORD *)&count, NULL);
    if(!fSuccess)
        return -1;

    return count;
}
#else
static int io_fd(void) {
    return xport == XP_TCP ? tcp_fd : dcfd;
}

int serial_read(void *buffer, int count) {
    return read(io_fd(), buffer, count);
}

int serial_write(void *buffer, int count) {
    unsigned char *p = buffer;
    int left = count;

    /* A tty write is normally complete; a socket write may not be. */
    while(left > 0) {
        int w = write(io_fd(), p, left);

        if(w < 0) {
            if(errno == EINTR)
                continue;
            return -1;
        }

        p += w;
        left -= w;
    }

    return count;
}

int serial_putc(char ch) {
    return serial_write(&ch, 1);
}
#endif /* _WIN32 */

/* serial_read() with a deadline. Returns -2 on timeout; 0 = unbounded. */
static int serial_read_timeout(void *buffer, int count, int ms) {
#ifndef _WIN32
    if(ms > 0) {
        fd_set rf;
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        int fd = io_fd();

        FD_ZERO(&rf);
        FD_SET(fd, &rf);

        if(select(fd + 1, &rf, NULL, NULL, &tv) <= 0)
            return -2;
    }
#else
    (void)ms;
#endif
    return serial_read(buffer, count);
}

/* Something went wrong on the link. There is nothing to recover -- dcload has
   no resync -- so say what happened and leave the port tidy. */
static void link_failed(const char *why) {
    fprintf(stderr, "\ndc-tool: %s\n", why);
    finish_serial();
    exit(2);
}

/* read count bytes from dc into buf */
void blread(void *buf, int count) {
    int retval, want = count;
    unsigned char *tmp = buf;

    while(count) {
        retval = serial_read_timeout(tmp, count, io_timeout_ms);

        if(retval == -2) {
            char msg[160];

            snprintf(msg, sizeof(msg), "timed out after %d ms waiting for %d "
                     "byte(s) from dcload (%d received). Is the Dreamcast at "
                     "dcload's prompt, and are both ends at the same baud?",
                     io_timeout_ms, want, want - count);
            link_failed(msg);
        }

        if(retval == 0)
            link_failed("connection to the Dreamcast closed");

        if(retval < 0) {
            if(errno == EINTR)
                continue;
            link_failed(strerror(errno));
        }

        tmp += retval;
        count -= retval;
    }
}

char serial_getc(void) {
    unsigned char tmp;

    blread(&tmp, 1);

    return (char)tmp;
}

/* dcload echoes every command byte before acting on it. Insist on it: a
   mismatch here is the earliest possible sign of a baud or sync problem, and
   ignoring it (as the original tool did) turns it into a silent hang. */
void expect_echo(unsigned char c) {
    unsigned char got;

    blread(&got, 1);

    if(got != c) {
        char msg[160];

        snprintf(msg, sizeof(msg), "expected dcload to echo '%c' (0x%02x), got "
                 "0x%02x -- is the Dreamcast at dcload's prompt and at %u baud?",
                 c >= 32 && c < 127 ? c : '?', c, got, initial_speed);
        link_failed(msg);
    }
}

/* send 4 bytes */
int send_uint(unsigned int value) {
    unsigned char b[4];
    unsigned int tmp;

    /* send little-endian, in one write */
    b[0] = (unsigned char)(value & 0xFF);
    b[1] = (unsigned char)((value >> 0x08) & 0xFF);
    b[2] = (unsigned char)((value >> 0x10) & 0xFF);
    b[3] = (unsigned char)((value >> 0x18) & 0xFF);
    serial_write(b, 4);

    /* get little-endian */
    tmp =  ((unsigned int) (serial_getc() & 0xFF));
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x08);
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x10);
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x18);

    if(tmp != value) {
        char msg[120];

        snprintf(msg, sizeof(msg), "dcload echoed 0x%08x for 0x%08x: protocol "
                 "desync (baud mismatch?)", tmp, value);
        link_failed(msg);
    }

    return 1;
}

/* Read the 4-byte echo of a value written earlier as part of a burst. */
static void expect_uint(unsigned int value) {
    unsigned int tmp;

    tmp =  ((unsigned int) (serial_getc() & 0xFF));
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x08);
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x10);
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x18);

    if(tmp != value) {
        char msg[120];

        snprintf(msg, sizeof(msg), "dcload echoed 0x%08x for 0x%08x: protocol "
                 "desync (baud mismatch?)", tmp, value);
        link_failed(msg);
    }
}

static void put_le32(unsigned char *b, unsigned int v) {
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 0x08) & 0xFF);
    b[2] = (unsigned char)((v >> 0x10) & 0xFF);
    b[3] = (unsigned char)((v >> 0x18) & 0xFF);
}

/* Ask dcload what it is. Returns the version string ("1.0.8") or NULL. */
static const char *probe_version(void) {
    static char line[80];
    unsigned char c;
    size_t n = 0;

    c = 'V';
    serial_write(&c, 1);
    expect_echo('V');

    /* "dcload-serial 1.0.8\n\r" -- read to the '\r'. */
    for(;;) {
        blread(&c, 1);
        if(c == '\r')
            break;
        if(n < sizeof(line) - 1 && c != '\n')
            line[n++] = (char)c;
    }
    line[n] = '\0';

    if(strncmp(line, "dcload-serial ", 14))
        return NULL;

    return line + 14;
}

/* 1 if "a.b.c" is at least major.minor.patch. */
static int version_at_least(const char *v, int major, int minor, int patch) {
    int a = 0, b = 0, c = 0;

    if(sscanf(v, "%d.%d.%d", &a, &b, &c) < 2)
        return 0;

    if(a != major) return a > major;
    if(b != minor) return b > minor;
    return c >= patch;
}

/* receive 4 bytes */
unsigned int recv_uint(void) {
    unsigned int tmp;

    /* get little-endian */
    tmp =  ((unsigned int) (serial_getc() & 0xFF));
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x08);
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x10);
    tmp |= ((unsigned int) (serial_getc() & 0xFF) << 0x18);

    return (tmp);
}

/* receive total bytes from dc and store in data */
void recv_data(void *data, unsigned int total, unsigned int verbose) {
    unsigned char type, sum, ok;
    lzo_uint size, newsize;
    unsigned char *tmp;

    if(verbose) {
        printf("recv_data: ");
        fflush(stdout);
    }

    while(total) {
        blread(&type, 1);

        size = recv_uint();

        switch(type) {
            case 'U':       // uncompressed
                if(verbose) {
                    printf("U");
                    fflush(stdout);
                }
                blread(data, size);
                blread(&sum, 1);
                ok = 'G';
                serial_write(&ok, 1);
                total -= size;
                data += size;
                break;
            case 'C':       // compressed
                if(verbose) {
                    printf("C");
                    fflush(stdout);
                }
                tmp = malloc(size);
                blread(tmp, size);
                blread(&sum, 1);
                if(lzo1x_decompress(tmp, size, data, &newsize, 0) == LZO_E_OK) {
                    ok = 'G';
                    serial_write(&ok, 1);
                    total -= newsize;
                    data += newsize;
                } else {
                    ok = 'B';
                    serial_write(&ok, 1);
                    printf("\nrecv_data: decompression failed!\n");
                }
                free(tmp);
                break;
            default:
                break;
        }
    }

    if(verbose) {
        printf("\n");
        fflush(stdout);
    }
}

/* send size bytes to dc from addr */
void send_data(unsigned char *addr, unsigned int size, unsigned int verbose) {
    unsigned int i;
    unsigned char sum = 0;
    unsigned char data;
    lzo_uint csize;
    unsigned int sendsize;
    unsigned char c;
    unsigned char *buffer;

    buffer = malloc(DCLOADBUFFER + DCLOADBUFFER / 64 + 16 + 3);

    if(verbose) {
        printf("send_data: ");
        fflush(stdout);
    }

    while(size) {
        if(size > DCLOADBUFFER)
            sendsize = DCLOADBUFFER;
        else
            sendsize = size;

        lzo1x_1_compress((unsigned char *)addr, sendsize, buffer, &csize, wrkmem);

        if(csize < sendsize) {
            // send compressed
            if(verbose) {
                printf("C");
                fflush(stdout);
            }
            sum = 0;
            for(i = 0; i < csize; i++)
                sum ^= buffer[i];

            if(pipelined) {
                /* header + size + payload + checksum in one go; the size echo
                   comes back while the payload is still in flight. */
                unsigned char hdr[5];

                hdr[0] = 'C';
                put_le32(hdr + 1, csize);
                serial_write(hdr, 5);
                serial_write(buffer, csize);
                serial_write(&sum, 1);
                expect_uint(csize);
                blread(&data, 1);
            }
            else {
                c = 'C';
                serial_write(&c, 1);
                send_uint(csize);
                serial_write(buffer, csize);
                serial_write(&sum, 1);
                blread(&data, 1);
            }

            /* A bad decompress: dcload asks for the same payload again. */
            while(data != 'G') {
                serial_write(buffer, csize);
                serial_write(&sum, 1);
                blread(&data, 1);
            }
        } else {
            // send uncompressed
            if(verbose) {
                printf("U");
                fflush(stdout);
            }
            sum = 0;
            for (i = 0; i < sendsize; i++) {
                sum ^= ((unsigned char *)addr)[i];
            }

            if(pipelined) {
                unsigned char hdr[5];

                hdr[0] = 'U';
                put_le32(hdr + 1, sendsize);
                serial_write(hdr, 5);
                serial_write((unsigned char *)addr, sendsize);
                serial_write(&sum, 1);
                expect_uint(sendsize);
                blread(&data, 1);
            }
            else {
                c = 'U';
                serial_write(&c, 1);
                send_uint(sendsize);
                serial_write((unsigned char *)addr, sendsize);
                serial_write(&sum, 1);
                blread(&data, 1);
            }
        }

        size -= sendsize;
        addr += sendsize;
    }

    if(verbose) {
        printf("\n");
        fflush(stdout);
    }
}

#ifdef _WIN32
void output_error(void) {
    char *lpMsgBuf;

    FormatMessage(
    FORMAT_MESSAGE_ALLOCATE_BUFFER |
    FORMAT_MESSAGE_FROM_SYSTEM |
    FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    GetLastError(),
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
    (LPTSTR) &lpMsgBuf, 0, NULL // Process any inserts in lpMsgBuf.
    );
    printf("%s\n",(char *)lpMsgBuf);
    LocalFree((LPVOID) lpMsgBuf);
}
#endif

#ifndef _WIN32
void get_supported_speed (unsigned int *speed, speed_t *baudconst) {
    speed_t tmpbaud;
    unsigned int tmpspeed = *speed;

#ifdef BOTHER
    /*
       Use the BOTHER constant to enable custom baud rates.
       This allows specifying any arbitrary speed via the `c_ispeed` and `c_ospeed` fields
       in the extended termios2 structure (Linux-specific).
    */
    *baudconst = BOTHER;
#elif __APPLE__
    /*
       On macOS, set an initial baud rate (e.g., B115200) as a placeholder.
       The actual custom speed will be configured later using `ioctl(IOSSIOSPEED)`.
       This is required because macOS handles custom baud rates differently.
     */
    *baudconst = B115200;
#else
    for(tmpbaud = B0; tmpbaud == B0;) {
        switch(tmpspeed) {
#ifdef B1500000
        case 1500000:
            tmpbaud = B1500000;
            break;
#endif
#ifdef B500000
        case 500000:
            tmpbaud = B500000;
            break;
#endif
#ifdef B230400
        case 230400:
            tmpbaud = B230400;
            break;
#endif
#ifdef B115200
        case 115200:
            tmpbaud = B115200;
            break;
#endif
#ifdef B57600
        case 57600:
            tmpbaud = B57600;
            break;
#endif
        case 38400:
            tmpbaud = B38400;
            break;
        case 19200:
            tmpbaud = B19200;
            break;
        case 9600:
            tmpbaud = B9600;
            break;
        default:
            tmpspeed = (tmpspeed == INITIAL_SPEED ? 38400 : INITIAL_SPEED);
            break;
        }
    }
    *baudconst = tmpbaud;
    *speed = tmpspeed;
#endif /* BOTHER */
}

void tc_close (int fd) {
#ifndef BOTHER
    tcflush(fd, TCIOFLUSH);
#endif
    close(fd);
}

int tc_get_attr(int fd, termiosx_t *tio) {
#ifdef BOTHER
    return ioctl(fd, TCGETS2, tio);
#else
    return tcgetattr(fd, tio);
#endif
}

int tc_set_attr (int fd, termiosx_t *tio) {
#ifdef BOTHER
    return ioctl(fd, TCSETS2, tio);
#else
    tcflush(fd, TCIOFLUSH);
    return tcsetattr(fd, TCSANOW, tio);
#endif
}

void set_io_speed (unsigned int speed, speed_t baudconst) {
    termiosx_t newtio;

    tc_get_attr(dcfd, &oldtio); // save current serial port settings

    memset(&newtio, 0, sizeof(newtio)); // clear struct for new port settings
    newtio.c_cflag = CRTSCTS | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0;     // inter-character timer unused
    newtio.c_cc[VMIN] = 1;      // blocking read until 1 character arrives

#ifdef BOTHER
    newtio.c_cflag |= BOTHER;
    newtio.c_ospeed = speed;
    newtio.c_cflag |= BOTHER << IBSHIFT;
    newtio.c_ispeed = speed;
#else
    cfsetispeed(&newtio, baudconst);
    cfsetospeed(&newtio, baudconst);
#endif

    if(tc_set_attr(dcfd, &newtio) < 0) {
        perror("tc_set_attr");
        printf("warning: your baud rate is likely set incorrectly\n");
    }

#ifdef __APPLE__
    if(ioctl(dcfd, IOSSIOSPEED, &speed) < 0) {
        perror("IOSSIOSPEED");
        printf("warning: your baud rate is likely set incorrectly\n");
    }
#endif
}
#endif /* #ifndef _WIN32 */

/* setup serial port */
int open_serial(const char *devicename, unsigned int speed, unsigned int *speedtest) {
    *speedtest = speed;
#ifndef _WIN32
    speed_t baudconst;
    unsigned int oldspeed;

    cur_speed = speed;

    if(xport == XP_TCP) {
        char reply[64];

        if(tcp_fd < 0) {
            tcp_fd = tcp_connect(tcp_host, tcp_port, 5000);
            if(tcp_fd < 0)
                exit(-1);
        }

        /* Put the bridge at the rate dcload is at. Unreachable is tolerated
           (fixed-rate bridge); a refusal is not. */
        if(esp_ctl(reply, sizeof(reply), "baud %u", speed) == -2) {
            fprintf(stderr, "dc-tool: ESP32 refused baud %u: %s\n", speed, reply);
            exit(-1);
        }

        return 0;
    }

    dcfd = open(devicename, O_RDWR | O_NOCTTY);
    if(dcfd < 0) {
        perror(devicename);
        exit(-1);
    }

    oldspeed = speed;
    get_supported_speed (&speed, &baudconst);
    if(speed != oldspeed) {
        printf("Unsupported baudrate (%d) - falling back to baudrate (%d)\n", oldspeed, speed);
        *speedtest = speed;
    }

    set_io_speed (speed, baudconst);

#else /* _WIN32 */
    BOOL fSuccess;
    COMMTIMEOUTS ctmoCommPort;
    DCB dcbCommPort;

    /* Setup the com port */
    hCommPort = CreateFile(devicename, GENERIC_READ | GENERIC_WRITE, 0,
               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);

    if(hCommPort == INVALID_HANDLE_VALUE) {
        printf("*Error opening com port\n");
        output_error();
        return -1;
    }

    ctmoCommPort.ReadIntervalTimeout = MAXDWORD;
    ctmoCommPort.ReadTotalTimeoutMultiplier = MAXDWORD;
    ctmoCommPort.ReadTotalTimeoutConstant = MAXDWORD;
    ctmoCommPort.WriteTotalTimeoutMultiplier = 0;
    ctmoCommPort.WriteTotalTimeoutConstant = 0;
    SetCommTimeouts(hCommPort, &ctmoCommPort);
    dcbCommPort.DCBlength = sizeof(DCB);

    fSuccess = GetCommState(hCommPort, &dcbCommPort);
    if(!fSuccess) {
        printf("*Error getting com port state\n");
        output_error();
        return -1;
    }

    dcbCommPort.BaudRate = speed;
    dcbCommPort.ByteSize = DATA_BITS;
    dcbCommPort.Parity = PARITY_SET;
    dcbCommPort.StopBits = STOP_BITS;

    fSuccess = SetCommState(hCommPort, &dcbCommPort);
    if(!fSuccess) {
        printf("*Error setting com port state\n");
        output_error();
        return -1;
    }
#endif /* !_WIN32 */
    return 0;
}

/* prepare for program exit */
void finish_serial(void) {
#ifdef _WIN32
    FlushFileBuffers(hCommPort);
#else
    if(xport == XP_TCP)
        transport_close();      /* the bridge restores its baud on close */
    else
        tc_set_attr(dcfd, &oldtio);
#endif
    cleanup();
}

/* close the host serial port */
void close_serial(void) {
#ifdef _WIN32
    FlushFileBuffers(hCommPort);
    CloseHandle(hCommPort);
#else
    if(xport == XP_TCP)
        transport_close();
    else
        tc_close(dcfd);
#endif
}

int speedhack = 0;
/* speedhack controls whether dcload will use
 * - N=13 (alternate, -3.0% error) instead of N=12 (normal, 4.3% error) for 115200
 * - N=6  (alternate, -2.8% error) instead of N=5 (normal, 11.5% error) for 230400
 */

/* use_extclk controls whether the DC's serial port will use an external clock */
int use_extclk = 0;

int change_speed(char *device_name, unsigned int speed) {
    unsigned char c;
    unsigned int dummy, rv = 0xdeadbeef;

    c = 'S';
    serial_write(&c, 1);
    expect_echo('S');

    if(speedhack && (speed == 115200))
        send_uint(111607); /* get dcload to pick N=13 rather than N=12 */
    else if(speedhack && (speed == 230400))
        send_uint(223214); /* get dcload to pick N=6 rather than N=5 */
    else if(use_extclk)
        send_uint(0);
    else
        send_uint(speed);

    printf("Changing speed to %d bps... ", speed);
    fflush(stdout);

#ifndef _WIN32
    if(xport == XP_TCP) {
        char reply[64];

        /* dcload has echoed the new rate and is re-initialising its SCI. The
           echo has already reached us, so nothing is left in flight on the
           wire in either direction; switch the bridge now. */
        if(esp_ctl(reply, sizeof(reply), "baud %u", speed) == -2) {
            printf("failed: ESP32 said \"%s\"\n", reply);
            return 1;
        }

        usleep(20000);      /* dcload's scif_init() settling time, with margin */
        cur_speed = speed;
    }
    else
#endif
    {
        close_serial();

        if(open_serial(device_name, speed, &dummy)<0)
            return 1;
    }

    /* Both sides now confirm the new rate with a known word. send_uint()
       already checks dcload's echo; dcload then sends it back a second time. */
    send_uint(rv);

    if(recv_uint() != rv)
        link_failed("speed change handshake failed: the two ends are not at the same rate");

    printf("done\n");

    return 0;
}

int open_gdb_socket(int port) {
    struct sockaddr_in server_addr;

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    gdb_server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef __MINGW32__
    if(gdb_server_socket == INVALID_SOCKET) {
#else
    if(gdb_server_socket < 0) {
#endif
        perror("error creating gdb server socket");
        return -1;
    }

    const int enable_reuse_addr = 1;

#if _WIN32
    /* For Windows this cast is necessary on modern GCC... */
    int checkopt = setsockopt(gdb_server_socket, SOL_SOCKET, SO_REUSEADDR,
                              (const char *) &enable_reuse_addr, sizeof(enable_reuse_addr));
#else
    /* ... but maybe it's necessary for other OS as well? */
    int checkopt = setsockopt(gdb_server_socket, SOL_SOCKET, SO_REUSEADDR,
                              &enable_reuse_addr, sizeof(enable_reuse_addr));
#endif

#ifdef __MINGW32__
    if(checkopt == SOCKET_ERROR) {
#else
    if(checkopt < 0) {
#endif
        perror( "warning: failed to set gdb socket options");
    }

    int checkbind = bind(gdb_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
#ifdef __MINGW32__
    if(checkbind == SOCKET_ERROR) {
#else
    if(checkbind < 0) {
#endif
        perror("error binding gdb server socket");
        return -1;
    }

    int checklisten = listen(gdb_server_socket, 0);
#ifdef __MINGW32__
    if(checklisten == SOCKET_ERROR) {
#else
    if(checklisten < 0) {
#endif
        perror("error listening to gdb server socket");
        return -1;
    }

    return 0;
}

void usage(void) {
    printf("\n%s %s by Andrew \"ADK\" Kieschnick\n\n", PACKAGE, VERSION);
    printf("-x <filename> Upload and execute <filename>\n");
    printf("-u <filename> Upload <filename>\n");
    printf("-d <filename> Download to <filename>\n");
    printf("-a <address>  Set address to <address> (default: 0x8c010000)\n");
    printf("-s <size>     Set size to <size>\n");
    printf("-t <device>   Use <device> to communicate with dc (default: %s)\n", SERIALDEVICE);
    printf("              tcp:<host>[:<port>] talks to an esp32-dc bridge directly\n");
    printf("              (default port 2323; baud control on port+1)\n");
    printf("-b <baudrate> Use <baudrate> (default: %d)\n", DEFAULT_SPEED);
    printf("-B <baudrate> Rate dcload boots at (default: %d)\n", INITIAL_SPEED);
    printf("-T <ms>       Give up if dcload is silent for <ms> (default: 5000, 0 = never)\n");
    printf("-P            Use the original protocol: no pipelined or streamed uploads,\n");
    printf("              and console writes wait for the host as before\n");
    printf("-e            Try alternate 115200/230400 (must also use -b 115200 or -b 230400)\n");
    printf("-E            Use an external clock for the DC's serial port\n");
    printf("-n            Do not attach console and fileserver\n");
    printf("-p            Use dumb terminal rather than console/fileserver\n");
    printf("-q            Do not clear screen before download\n");
#ifndef __MINGW32__
    printf("-c <path>     Chroot to <path> (must be super-user)\n");
#endif
    printf("-i <isofile>  Enable cdfs redirection using iso image <isofile>\n");
    printf("-g            Start a GDB server\n");
    printf("-v <groups>   Log fileserver requests to stderr, <groups> is a comma-separated\n");
    printf("              list such as open,delete (-v help lists all groups)\n");
    printf("-h            Usage information (you\'re looking at it)\n\n");
    cleanup();
}

/* Got to make sure WinSock is initalized */
#ifdef __MINGW32__
int start_ws(void) {
    WSADATA wsaData;
    int failed = 0;
    failed = WSAStartup(MAKEWORD(2,2), &wsaData);
    if(failed != NO_ERROR) {
        perror("WSAStartup");
        return 1;
    }

    return 0;
}
#endif

static void put_le32(unsigned char *b, unsigned int v);
static void expect_uint(unsigned int value);

/* One section through the 'b' command. Returns 0, or -1 if dcload refused a
   batch three times in a row. */
static int send_section_stream(unsigned int addr, unsigned char *data,
                               unsigned int size) {
    unsigned char *cbuf = malloc(DCLOADBUFFER + DCLOADBUFFER / 64 + 16 + 3);
    unsigned char *batch = malloc(STREAM_SCRATCH_LEN * 2 + STREAM_MAX_BLOCKS * 16 + 16);
    unsigned char hdr[8], c;
    unsigned int off = 0;

    c = 'b';
    serial_write(&c, 1);
    expect_echo('b');

    put_le32(hdr, addr);
    put_le32(hdr + 4, size);
    serial_write(hdr, 8);
    expect_uint(addr);
    expect_uint(size);
    send_uint(stream_scratch);
    send_uint(STREAM_SCRATCH_LEN);

    printf("send_stream: ");
    fflush(stdout);

    while(off < size) {
        unsigned int blen = 0, scratch_used = 0, nblk = 0, sum = 0, start = off;
        unsigned int i, tries;

        /* Build the batch: blocks until the scratch area, the block table or
           the section runs out. Uncompressed blocks cost no scratch. */
        while(off < size && nblk < STREAM_MAX_BLOCKS &&
              blen + 9 + DCLOADBUFFER <= STREAM_BATCH_WIRE) {
            unsigned int raw = size - off > DCLOADBUFFER ? DCLOADBUFFER : size - off;
            lzo_uint clen;
            unsigned char *p = batch + blen;

            lzo1x_1_compress(data + off, raw, cbuf, &clen, wrkmem);

            if(clen < raw) {
                if(scratch_used + clen > STREAM_SCRATCH_LEN)
                    break;
                p[0] = 'C';
                put_le32(p + 1, (unsigned int)clen);
                put_le32(p + 5, raw);
                memcpy(p + 9, cbuf, clen);
                for(i = 0; i < clen; i++)
                    sum += cbuf[i];
                blen += 9 + (unsigned int)clen;
                scratch_used += (unsigned int)clen;
            }
            else {
                p[0] = 'U';
                put_le32(p + 1, raw);
                put_le32(p + 5, raw);
                memcpy(p + 9, data + off, raw);
                for(i = 0; i < raw; i++)
                    sum += data[off + i];
                blen += 9 + raw;
            }

            nblk++;
            off += raw;
        }

        batch[blen] = 'E';
        put_le32(batch + blen + 1, sum);
        blen += 5;

        for(tries = 0; ; tries++) {
            /* The ack comes after the whole batch has crossed the wire, so
               the wait has to include its transmission time. */
            int saved = io_timeout_ms;

            serial_write(batch, blen);
            if(io_timeout_ms > 0 && cur_speed > 0)
                io_timeout_ms += (int)((unsigned long long)blen * 10 * 1000 * 3 /
                                       (2ULL * cur_speed));
            blread(&c, 1);
            io_timeout_ms = saved;

            if(c == 'G')
                break;

            if(c != 'B' || tries >= 2) {
                printf("\n");
                if(c == 'B')
                    link_failed("dcload rejected the same batch three times");
                {
                    char msg[80];
                    snprintf(msg, sizeof(msg), "unexpected reply 0x%02x to a stream batch", c);
                    link_failed(msg);
                }
            }

            printf("!");        /* batch resent */
            fflush(stdout);
        }

        printf("#");
        fflush(stdout);
    }

    printf("\n");
    free(cbuf);
    free(batch);
    return 0;
}

/* After the 'B' echo: address and size. dcload echoes the address at once
   and the size only after it has drawn the progress bar, so the two can go
   out together but the size echo must be waited for before any data. */
static void send_section_header(unsigned int addr, unsigned int size) {
    if(pipelined) {
        unsigned char hdr[8];

        put_le32(hdr, addr);
        put_le32(hdr + 4, size);
        serial_write(hdr, 8);
        expect_uint(addr);
        expect_uint(size);
    }
    else {
        send_uint(addr);
        send_uint(size);
    }
}

/* Send one section, streamed when dcload supports it and a scratch area
   exists, one acknowledged block at a time otherwise. */
static void send_section(unsigned int addr, unsigned char *data, unsigned int size) {
    unsigned char c;

    if(streaming && stream_scratch) {
        send_section_stream(addr, data, size);
        return;
    }

    c = 'B';
    serial_write(&c, 1);
    expect_echo('B');
    send_section_header(addr, size);
    send_data(data, size, 1);
}

unsigned int upload(unsigned char *filename, unsigned int address) {
    int inputfd;
    int size = 0;
    unsigned char *inbuf;
    struct timeval starttime, endtime;
    unsigned char c;
    double stime, etime;

#ifdef WITH_BFD
    bfd *somebfd;
    int sectsize;
#else
    Elf *elf;
    Elf32_Ehdr *ehdr;
    Elf32_Shdr *shdr;
    Elf_Scn *section = NULL;
    Elf_Data *data;
    char *section_name;
    size_t index;
#endif

    lzo_init();

#ifdef WITH_BFD
    if((somebfd = bfd_openr(filename, 0))) { /* try bfd first */
        if(bfd_check_format(somebfd, bfd_object)) {
            asection *section;

            printf("File format is %s, ", somebfd->xvec->name);
            address = somebfd->start_address;
            size = 0;
            printf("start address is 0x%x\n", address);

            nranges = 0;
            for(section = somebfd->sections; section != NULL; section = section->next)
                if((section->flags & SEC_LOAD) && bfd_section_size(section))
                    note_range(section->lma, bfd_section_size(section));
            choose_scratch();

            gettimeofday(&starttime, 0);

            for(section = somebfd->sections; section != NULL; section = section->next) {
                if((section->flags & SEC_HAS_CONTENTS) && (section->flags & SEC_LOAD)) {
                    sectsize = bfd_section_size(section);
                    printf("Section %s, ", section->name);
                    printf("lma 0x%x, ", section->lma);
                    printf("size %d\n", sectsize);
                    if(sectsize) {
                        size += sectsize;
                        inbuf = malloc(sectsize);
                        bfd_get_section_contents(somebfd, section, inbuf, 0, sectsize);

                        send_section(section->lma, inbuf, sectsize);

                        free(inbuf);
                    }
                }
            }

            bfd_close(somebfd);
            goto done_transfer;
        }

        bfd_close(somebfd);
    }
#else /* !WITH_BFD -- use libelf */
    if(elf_version(EV_CURRENT) == EV_NONE) {
        fprintf(stderr, "ELF library initialization error: %s\n", elf_errmsg(-1));
        exit(-1);
    }

    if((inputfd = open((char *)filename, O_RDONLY | O_BINARY)) < 0) {
        perror((char *)filename);
        exit(-1);
    }

    if((elf = elf_begin(inputfd, ELF_C_READ, NULL)) == NULL) {
        fprintf(stderr, "Unable to open ELF file: %s\n", elf_errmsg(-1));
        exit(-1);
    }

    if(elf_kind(elf) == ELF_K_ELF) {
        if(!(ehdr = elf32_getehdr(elf))) {
            fprintf(stderr, "Unable to read ELF header: %s\n", elf_errmsg(-1));
            exit(-1);
        }

        address = ehdr->e_entry;
        printf("File format is ELF, start address is 0x%x\n", address);

        /* Retrieve the index of the ELF section containing the string table of
           section names */
        if(elf_getshdrstrndx(elf, &index)) {
            fprintf(stderr, "Unable to read section index: %s\n", elf_errmsg(-1));
            exit(-1);
        }

        /* Every section's address range first, so the streaming scratch
           area can be placed clear of all of them. */
        nranges = 0;
        while((section = elf_nextscn(elf, section))) {
            if((shdr = elf32_getshdr(section)) && shdr->sh_addr && shdr->sh_size)
                note_range(shdr->sh_addr, shdr->sh_size);
        }
        choose_scratch();
        section = NULL;

        gettimeofday(&starttime, 0);
        while((section = elf_nextscn(elf, section))) {
            if(!(shdr = elf32_getshdr(section))) {
                fprintf(stderr, "Unable to read section header: %s\n", elf_errmsg(-1));
                exit(-1);
            }

            if(!(section_name = elf_strptr(elf, index, shdr->sh_name))) {
                fprintf(stderr, "Unable to read section nam: %s\n", elf_errmsg(-1));
                exit(-1);
            }

            if(!shdr->sh_addr)
                continue;

            /* Check if there's some data to upload. */
            data = elf_getdata(section, NULL);
            if(!data->d_buf || !data->d_size)
                continue;

            printf("Section %s, lma 0x%x, size %d\n", section_name,
                   shdr->sh_addr, shdr->sh_size);
            size += shdr->sh_size;

            {
                /* A section may come in several data pieces; the protocol
                   wants one contiguous buffer. */
                unsigned char *sbuf = malloc(shdr->sh_size);
                unsigned int got = 0;

                do {
                    if(data->d_buf && got + data->d_size <= shdr->sh_size) {
                        memcpy(sbuf + got, data->d_buf, data->d_size);
                        got += data->d_size;
                    }
                } while((data = elf_getdata(section, data)));

                send_section(shdr->sh_addr, sbuf, got);
                free(sbuf);
            }
        }

        elf_end(elf);
        close(inputfd);
        goto done_transfer;
     } else {
        elf_end(elf);
        close(inputfd);
    }
#endif /* WITH_BFD */
    /* if all else fails, send raw bin */
    printf("File format is raw binary, start address is 0x%x\n", address);
    inputfd = open((char *)filename, O_RDONLY | O_BINARY);

    if(inputfd < 0) {
        perror((char *)filename);
        exit(-1);
    }

    size = lseek(inputfd, 0, SEEK_END);
    lseek(inputfd, 0, SEEK_SET);

    inbuf = malloc(size);

    read(inputfd, inbuf, size);

    close(inputfd);

    gettimeofday(&starttime, 0);

    nranges = 0;
    note_range(address, size);
    choose_scratch();

    send_section(address, inbuf, size);

done_transfer:
    gettimeofday(&endtime, 0);

    stime = starttime.tv_sec + starttime.tv_usec / 1000000.0;
    etime = endtime.tv_sec + endtime.tv_usec / 1000000.0;

    printf("effective: %.2f bytes / sec\n", (double) size / (etime - stime));
    printf("%.2f seconds to transfer %d bytes\n", (etime - stime), size);
    fflush(stdout);

    return address;
}

void download(unsigned char *filename, unsigned int address,
          unsigned int size, unsigned int quiet) {
    int outputfd;

    unsigned char c;
    unsigned int wrkmem = 0x8cff0000;
    unsigned char *data;
    struct timeval starttime, endtime;
    double stime, etime;

    outputfd = open((char *)filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);

    if(outputfd < 0) {
        perror((char *)filename);
        exit(-1);
    }

    data = malloc(size);

    c = quiet ? 'G' : 'F';
    serial_write(&c, 1);
    expect_echo(c);
    send_uint(address);
    send_uint(size);
    send_uint(wrkmem);
    gettimeofday(&starttime, 0);
    recv_data(data, size, 1);
    gettimeofday(&endtime, 0);

    printf("Received %d bytes\n", size);

    stime = starttime.tv_sec + starttime.tv_usec / 1000000.0;
    etime = endtime.tv_sec + endtime.tv_usec / 1000000.0;

    printf("effective: %.2f bytes / sec\n", (double) size / (etime - stime));
    printf("%.2f seconds to transfer %d bytes\n", (etime - stime), size);
    fflush(stdout);

    write(outputfd, data, size);

    close(outputfd);
}

void execute(unsigned int address, unsigned int console) {
    unsigned char c;

    printf("Sending execute command (0x%x, console=%d)...", address, console);

    c = 'A';
    serial_write(&c, 1);
    expect_echo('A');

    send_uint(address);
    send_uint(console);

    printf("executing\n");
}

void do_console(unsigned char *path, unsigned char *isofile) {
    unsigned char command;
    int isofd;

    if(isofile) {
        isofd = open((char *)isofile, O_RDONLY | O_BINARY);
        if(isofd < 0)
            perror((char *)isofile);
    }

#ifndef __MINGW32__
    if(path)
        if(chroot((char *)path))
            perror((char *)path);
#endif

    while(1) {
        int r;

        fflush(stdout);

        /* Unbounded on purpose: a program may be silent for hours. */
        r = serial_read(&command, 1);
        if(r == 0 || (r < 0 && errno != EINTR))
            link_failed("connection to the Dreamcast closed");
        if(r < 0)
            continue;

        switch(command) {
            case 0:
                finish_serial();
                exit(0);
                break;
            case 1:
                dc_fstat();
                break;
            case 2:
                dc_write();
                break;
            case 3:
                dc_read();
                break;
            case 4:
                dc_open();
                break;
            case 5:
                dc_close();
                break;
            case 6:
                dc_creat();
                break;
            case 7:
                dc_link();
                break;
            case 8:
                dc_unlink();
                break;
            case 9:
                dc_chdir();
                break;
            case 10:
                dc_chmod();
                break;
            case 11:
                dc_lseek();
                break;
            case 12:
                dc_time();
                break;
            case 13:
                dc_stat();
                break;
            case 14:
                dc_utime();
                break;
            case 15:
                printf("command 15 should not happen... (but it did)\n");
                break;
            case 16:
                dc_opendir();
                break;
            case 17:
                dc_closedir();
                break;
            case 18:
                dc_readdir();
                break;
            case 19:
                dc_cdfs_redir_read_sectors(isofd);
                break;
            case 20:
                dc_gdbpacket();
                break;
            case 21:
                dc_rewinddir();
                break;
            case 22:
                dc_exit();
                break;
            case 23:
                dc_write_nowait();
                break;
            default:
                printf("Unimplemented command (%d) \n", command);
                printf("Assuming program has exited, or something...\n");
                finish_serial();
                exit(-1);
                break;
        }
    }

    if(isofd)
        close(isofd);
}

/* dumb terminal mode
 * for programs that don't use dcload I/O functions
 * FIXME: should allow setting a different baud rate from what dcload uses
 * FIXME: should allow input
 */

void do_dumbterm(void) {
    unsigned char c;

    printf("\nDumb terminal mode isn't implemented, so you get this half-assed one.\n\n");

    fflush(stdout);

    while(1) {
        blread(&c, 1);
        printf("%c", c);
        fflush(stdout);
    }
}

#ifdef __MINGW32__
#define AVAILABLE_OPTIONS       "x:u:d:a:s:t:b:B:T:i:v:npqheEgP"
#else
#define AVAILABLE_OPTIONS       "x:u:d:a:s:t:b:B:T:c:i:v:npqheEgP"
#endif

int main(int argc, char *argv[]) {
    unsigned int address = 0x8c010000;
    unsigned int size = 0;
    unsigned char *filename = 0;
    unsigned char *path = 0;
    unsigned int console = 1;
    unsigned int dumbterm = 0;
    unsigned int quiet = 0;
    unsigned char command = 0;
    unsigned int dummy, speed = DEFAULT_SPEED;
    char *device_name = SERIALDEVICE;
    unsigned int cdfs_redir = 0;
    unsigned char *isofile = 0;
    int someopt;

    if (argc < 2) {
        usage();
        exit(-1);
    }
    someopt = getopt(argc, argv, AVAILABLE_OPTIONS);
    while(someopt > 0) {
        switch(someopt) {
            case 'x':
                if(command) {
                    printf("You can only specify one of -x, -u, and -d\n");
                    exit(-1);
                }
                command = 'x';
                filename = malloc(strlen(optarg) + 1);
                strcpy((char *)filename, optarg);
                break;
            case 'u':
                if(command) {
                    printf("You can only specify one of -x, -u, and -d\n");
                    exit(-1);
                }
                command = 'u';
                filename = malloc(strlen(optarg) + 1);
                strcpy((char *)filename, optarg);
                break;
            case 'd':
                if(command) {
                    printf("You can only specify one of -x, -u, and -d\n");
                    exit(-1);
                }
                command = 'd';
                filename = malloc(strlen(optarg) + 1);
                strcpy((char *)filename, optarg);
                break;
#ifndef __MINGW32__
            case 'c':
                path = malloc(strlen(optarg) + 1);
                strcpy((char *)path, optarg);
                break;
#endif
            case 'i':
                cdfs_redir = 1;
                isofile = malloc(strlen(optarg) + 1);
                strcpy((char *)isofile, optarg);
                break;
            case 'a':
                address = strtoul(optarg, NULL, 0);
                break;
            case 's':
                size = strtoul(optarg, NULL, 0);
                break;
            case 't':
                device_name = malloc(strlen(optarg) + 1);
                strcpy(device_name, optarg);
                break;
            case 'b':
                speed = strtoul(optarg, NULL, 0);
                break;
            case 'B':
                initial_speed = strtoul(optarg, NULL, 0);
                break;
            case 'T':
                io_timeout_ms = (int)strtol(optarg, NULL, 0);
                break;
            case 'P':
                no_pipeline = 1;
                break;
            case 'n':
                console = 0;
                break;
            case 'p':
                console = 0;
                dumbterm = 1;
                break;
            case 'q':
                quiet = 1;
                break;
            case 'h':
                usage();
                exit(0);
                break;
            case 'e':
                speedhack = 1;
                break;
            case 'E':
                use_extclk = 1;
                break;
            case 'g':
                printf("Starting a GDB server on port 2159\n");
#ifdef __MINGW32__
                if(start_ws())
                    return -1;
#endif
                open_gdb_socket(2159);
                gdb_socket_started = 1;
                break;
            case 'v':
                if(!strcmp(optarg, "help")) {
                    help_log_groups();
                    cleanup();
                    exit(0);
                }
                if(parse_log_groups(optarg)) {
                    help_log_groups();
                    cleanup();
                    exit(-1);
                }
                break;
            default:
                break;
        }

        someopt = getopt(argc, argv, AVAILABLE_OPTIONS);
    }

    if((command == 'x') || (command == 'u')) {
        struct stat statbuf;
        if(stat((char *)filename, &statbuf)) {
            perror((char *)filename);
            exit(-1);
        }
    }

    if(console)
        printf("Console enabled\n");

    if(dumbterm)
        printf("Dumb terminal enabled\n");

    if(quiet)
        printf("Quiet download\n");

#ifndef __MINGW32__
    if(path)
        printf("Chroot enabled\n");
#endif

    if(cdfs_redir)
        printf("Cdfs redirection enabled\n");

    if(speedhack && (speed == 115200 || speed == 230400))
        printf("Alternate 115200/230400 enabled\n");

    if(use_extclk)
        printf("External clock usage enabled\n");

    if(enabled_log_groups)
        printf("Fileserver request logging enabled\n");

#ifndef _WIN32
    if(parse_transport(device_name))
        printf("Transport: tcp %s:%u (baud control on port %u)\n",
               tcp_host, tcp_port, tcp_ctl_port);
#endif

#ifndef __APPLE__
    /* test for reasonable baud - this is for POSIX systems */
    if(xport == XP_SERIAL && speed != initial_speed) {
        if(open_serial(device_name, speed, &speed)<0)
            return 1;
        close_serial();
    }
#endif

    if(open_serial(device_name, initial_speed, &dummy)<0)
        return 1;

    /* Who is on the other end? Also the first thing that fails if the rate
       is wrong, with a message that says so. */
    {
        const char *ver = probe_version();

        if(ver && version_at_least(ver, 1, 0, 8) && !no_pipeline)
            pipelined = 1;
        if(ver && version_at_least(ver, 1, 0, 9) && !no_pipeline)
            streaming = 1;
        if(ver && version_at_least(ver, 1, 0, 10) && !no_pipeline)
            console_nowait = 1;

        printf("dcload-serial %s%s\n", ver ? ver : "(unknown version)",
               streaming ? ", streaming uploads" :
               pipelined ? ", pipelined uploads" : "");
    }

#ifndef _WIN32
    /* A bridge that cannot be told about a speed change must not be asked
       for one: dcload would switch, the bridge would not, and every byte
       after that would be lost. Stay at the boot rate instead. */
    if(xport == XP_TCP && !ctl_available && speed != initial_speed) {
        printf("No control port: staying at %u baud (use -B to declare a "
               "fixed-rate bridge)\n", initial_speed);
        speed = initial_speed;
    }
#endif

    if(speed != initial_speed)
        if(change_speed(device_name, speed))
            return 1;

    switch(command) {
        case 'x':
            if(cdfs_redir) {
                unsigned char c;
                c = 'H';
                serial_write(&c, 1);
                expect_echo('H');
            }
            /* Let dcload send console output without waiting for replies.
               It forgets this whenever it restarts, so ask every run. */
            if(console_nowait && console) {
                unsigned char c = 'W';

                serial_write(&c, 1);
                expect_echo('W');
                printf("Console output: no-reply writes\n");
            }
            printf("Upload <%s>\n", filename);
            address = upload(filename, address);
            printf("Executing at <0x%x>\n", address);
            execute(address, console);
            if(console)
                do_console(path, isofile);
            else if(dumbterm)
                do_dumbterm();
            break;
        case 'u':
            printf("Upload <%s> at <0x%x>\n", filename, address);
            upload(filename, address);
            if(speed != initial_speed)
                change_speed(device_name, initial_speed);
            break;
        case 'd':
            if(!size) {
                printf("You must specify a size (-s <size>) with download (-d <filename>)\n");
                cleanup();
                exit(-1);
            }
            printf("Download %d bytes at <0x%x> to <%s>\n", size, address,
                filename);
            download(filename, address, size, quiet);
            if(speed != initial_speed)
                change_speed(device_name, initial_speed);
            break;
        default:
            if(dumbterm)
                do_dumbterm();
            else
                usage();
	    	exit(-1);
            break;
    }

    close_serial();
    cleanup();
    exit(0);
}
