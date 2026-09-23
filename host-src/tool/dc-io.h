#ifndef __DC_IO_H__
#define __DC_IO_H__

int send_uint(unsigned int value);
unsigned int recv_uint(void);
void recv_data(void *data, unsigned int total, unsigned int verbose);
void send_data(unsigned char *addr, unsigned int size, unsigned int verbose);
void finish_serial(void);
void expect_echo(unsigned char c);
extern unsigned int initial_speed;
extern int io_timeout_ms;

#endif /* __DC_IO_H__ */
