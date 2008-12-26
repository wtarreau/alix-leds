/*
 * alix-switch version 1.0 - (C) 2008 - Willy Tarreau <w@1wt.eu>
 * Return 0 if ALIX switch is found and pressed, otherwise 1.
 * Redistribute under GPLv2.
 *
 * To build optimally (add -DQUIET to remove messages) :
 *  $ diet gcc -fomit-frame-pointer -mpreferred-stack-boundary=2 -Wall -Os \
 *         -Wl,--gc-sections -o alix-switch alix-switch.c 
 *  $ sstrip alix-switch
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/resource.h>


const char usage[] =
#ifndef QUIET
  "alix-switch version 1.0 - (C) 2008 - Willy Tarreau <w@1wt.eu>\n"
  "  Check ALIX switch and return 0 if pressed.\n"
  "\n"
  "Usage:\n"
  "  # alix-switch [-h] [-l]\n"
  "\n"
  "If '-l' is passed, the leds will blink until the switch is released.\n"
#endif
  "";

/* if ret < 0, report msg with perror and return -ret.
 * if ret > 0, return msg on stderr and return ret
 * if ret == 0, return msg on stdout and return 0.
 * if msg is NULL, nothing is reported.
 */
#ifndef QUIET
#define die(r, m) _die((r), (m))
#else
#define die(r, m) exit(r)
#endif

#define SWITCH_PORT 0x61B0
#define SWITCH_MASK 0x0100

#define LED1_PORT 0x6100
#define LED2_PORT 0x6180
#define LED3_PORT 0x6180
#define LED1_MASK 0x00400040
#define LED2_MASK 0x02000200
#define LED3_MASK 0x08000800
#define LED_ON    0xFFFF0000


/* common error messages */
static const struct {
	unsigned char err;
	const char str[7];
} errstr[] = {
	{ .err = EACCES         , .str = "EACCES" }, // open, socket
	{ .err = EFAULT         , .str = "EFAULT" }, // open
	{ .err = EFBIG          , .str = "EFBIG" },  // open
	{ .err = EINVAL         , .str = "EINVAL" }, // iopl, socket
	{ .err = EISDIR         , .str = "EISDIR" }, // open
	{ .err = ELOOP          , .str = "ELOOP" },  // open
	{ .err = EMFILE         , .str = "EMFILE" }, // open, socket
	{ .err = ENAMETOOLONG   , .str = "E2LONG" }, // open
	{ .err = ENFILE         , .str = "ENFILE" }, // open, socket
	{ .err = ENOBUFS        , .str = "ENOBUF" },// socket,
	{ .err = ENODEV         , .str = "ENODEV" }, // open
	{ .err = ENOENT         , .str = "ENOENT" }, // open
	{ .err = ENOMEM         , .str = "ENOMEM" }, // open, socket
	{ .err = ENOSYS         , .str = "ENOSYS" }, // iopl
	{ .err = ENXIO          , .str = "ENXIO" }, // open
	{ .err = EPERM          , .str = "EPERM" }, // iopl
};

/* return an error message for errno <err> */
static const char *errmsg(int err)
{
	int i;
	for (i = 0; i < sizeof(errstr)/sizeof(errstr[0]); i++)
		if (errstr[i].err == (unsigned char)err)
			return errstr[i].str;
	return "Unknown error";
}

static void fdprint(int fd, const char *msg)
{
	const char *p = msg - 1;
	while (*(++p));
	write(fd, msg, p - msg);
}

/* prints message <msg> + one LF to fd <fd> without buffering.
 * <msg> cannot be NULL.
 */
static void fdperror(int fd, const char *msg)
{
	int err = errno;
	fdprint(fd, msg);
	fdprint(fd, ": ");
	msg = errmsg(err);
	fdprint(fd, msg);
	fdprint(fd, "\n");
}

/* prints message <msg> + one LF to fd <fd> without buffering.
 * <msg> cannot be NULL.
 */
static void fdputs(int fd, const char *msg)
{
	fdprint(fd, msg);
	fdprint(fd, "\n");
}

__attribute__((noreturn))
static void _die(int ret, const char *msg)
{
	if (ret < 0) {
		ret = -ret;
		if (msg)
			fdperror(2, msg);
	}
	else if (msg) {
		fdputs((ret == 0) ? 1 : 2, msg);
	}
	exit(ret);
}

static inline int switch_pressed()
{
	return !(inl(SWITCH_PORT) & SWITCH_MASK);
}

static inline void setled(unsigned leds, unsigned mask, unsigned port)
{
#ifndef DEBUG
	outl(leds & mask, port);
#endif
}

int main(int argc, char **argv)
{
	int leds = 0;
	int light;

	argc--; argv++;
	while (argc > 0) {
		if (**argv != '-')
			die(1, usage);

		/* options with one arg first */
		if (argv[0][1] == 'h')
			die(0, usage);
		else if (argv[0][1] == 'l')
			leds = 1;
		else
			die(1, usage);
		argc--; argv++;
	}

	if (iopl(3) == -1)
		die(-1, "Cannot get I/O port");

	if (!switch_pressed())
		return 1;

	light = LED_ON;
	while (leds && switch_pressed()) {
		setled(LED1_MASK, light, LED1_PORT);
		setled(LED2_MASK, light, LED2_PORT);
		setled(LED3_MASK, light, LED3_PORT);
		usleep(150000);
		light = ~light;
	}

	/* restore LED1 ON, others OFF */
	setled(LED1_MASK, LED_ON, LED1_PORT);
	setled(LED2_MASK, ~LED_ON, LED2_PORT);
	setled(LED3_MASK, ~LED_ON, LED3_PORT);
	return 0;
}

