/*
 * alix-leds version 2.0 - (C) 2008 - Willy Tarreau <w@1wt.eu>
 * Blink LEDs on ALIX motherboards depending on network status.
 * Redistribute under GPLv2.
 *
 * To build optimally (add -DQUIET to remove messages) :
 *  $ diet gcc -fomit-frame-pointer -Wall -Os -Wl,--sort-section=alignment \
 *         -o alix-leds alix-leds.c 
 *  $ sstrip alix-leds
 *
 * For more info about usage, check the "usage" help string below.
 */

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <errno.h>
#include <sched.h>
#include <net/if.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>

#include <linux/types.h>
#include <linux/sockios.h>

/* for passing single values */
struct ethtool_value {
        __u32     cmd;
        __u32     data;
};

#define ETHTOOL_GLINK 0xa

#ifndef SIOCETHTOOL
#define SIOCETHTOOL     0x8946
#endif

#undef SCHED_IDLEPRIO
#define SCHED_IDLEPRIO  5

/* sleep 1 second max */
#define MAXSLEEP   1000000
#define SLEEP_1SEC 1000000
#define SLEEP_500M  500000

/* ALIX leds */
#define LED1_PORT 0x6100
#define LED2_PORT 0x6180
#define LED3_PORT 0x6180
#define LED1_MASK 0x00400040
#define LED2_MASK 0x02000200
#define LED3_MASK 0x08000800
#define LED_ON    0xFFFF0000

/* used by network leds */
#define MAXSTEPS  2

enum {
	LED_UNUSED = 0,
	LED_NET = 1,
	LED_RUNNING = 2,
	LED_CPU = 3,
	LED_DISK = 4,
};

enum {
	IF_TYPE_NONE = 0,
	IF_TYPE_PHYSICAL,
	IF_TYPE_LOGICAL,
};

struct if_status {
	const char *name;
	int type; /* IF_TYPE_* */
	int present;
	int status;
};

struct cpu_status {
	unsigned int cpu_total[2], cpu_idle[2];
	unsigned int cpu_usage;
};

struct ide_status {
	unsigned int count[2];
	unsigned int disk_usage;
};

struct led {
	int type;  /* led type (LED_*). 0 = unused */
	int state; /* internal state. 0 at init. 1 for first state. */
	int sleep; /* sleep time in ms */
	unsigned int port; /* I/O port */
	unsigned int mask; /* on/off mask */
	char *disk_name;
	struct if_status *intf, *slave, *tun; /* checked interfaces */
	struct cpu_status cpu;
	struct ide_status ide;
	int count, limit, flash;           /* used for interface status */
};

#define MAXIFS 16
static struct led leds[3];
static struct if_status ifs[MAXIFS];
static int nbifs = 0;

/* network socket */
static int net_sock  = 0; /* -2 = unneeded, -1 = needed, >=0 = initialized */
static int fast_mode = 0; /* start blink fast for running led */

const char usage[] =
#ifndef QUIET
  "alix-leds version 2.0 - (C) 2008 - Willy Tarreau <w@1wt.eu>\n"
  "  Blink LEDs on ALIX motherboards depending on system and network status.\n"
  "\n"
  "Usage:\n"
  "  # alix-leds [-p pidfile] {[-l 1|2|3] [-durR] [-i intf] [-s slave] [-t tun]}*\n"
  "              [-I]\n"
  "\n"
  "LEDs 1,2,3 are independently managed. Specify one led, followed by the checks\n"
  "to associate to that LED. Repeat for other leds. Network interface status can\n"
  "report up to 3 interfaces per LED : a physical interface with link checking, a\n"
  "slave interface (eg: ppp), and a tunnel interface, in this priority order. Any\n"
  "unspecified interface is considered up. Network status is reported as follows :\n"
  "  - when all interfaces are up, the LED remains lit.\n"
  "  - when <intf> link is down, the LED remains off.\n"
  "  - when <slave> is down or absent, the LED blinks slowly (once per second).\n"
  "  - when <tun> is down or absent, the LED flashes twice a second.\n"
  "The 'running' more (-r) will slowly blink the led at 1 Hz. Using -R will blink\n"
  "it at 10 Hz. SIGUSR1 switches running leds to -r, SIGUSR2 switches them to -R.\n"
  "Use -p to store the daemon's pid into file <pidfile>. The 'usage' mode (-u)\n"
  "reports CPU usage by blinking slower or faster depending on the load. -I sets\n"
  "scheduling to idle priority (less precise). -d enables monitoring of hard disk.\n"
#endif
  "";

/* common error messages */
static const struct {
	int err;
	const char *str;
} errstr[] = {
	{ .err = EACCES         , .str = "EACCES" },
	{ .err = EAFNOSUPPORT   , .str = "EAFNOSUPPORT" },
	{ .err = EEXIST         , .str = "EEXIST" },
	{ .err = EFAULT         , .str = "EFAULT" },
	{ .err = EFBIG          , .str = "EFBIG" },
	{ .err = EINVAL         , .str = "EINVAL" },
	{ .err = EISDIR         , .str = "EISDIR" },
	{ .err = ELOOP          , .str = "ELOOP" },
	{ .err = EMFILE         , .str = "EMFILE" },
	{ .err = ENAMETOOLONG   , .str = "ENAMETOOLONG" },
	{ .err = ENFILE         , .str = "ENFILE" },
	{ .err = ENOBUFS        , .str = "ENOBUFS" },
	{ .err = ENODEV         , .str = "ENODEV" },
	{ .err = ENOENT         , .str = "ENOENT" },
	{ .err = ENOMEM         , .str = "ENOMEM" },
	{ .err = ENOSPC         , .str = "ENOSPC" },
	{ .err = ENOSYS         , .str = "ENOSYS" },
	{ .err = ENOTDIR        , .str = "ENOTDIR" },
	{ .err = ENXIO          , .str = "ENXIO" },
	{ .err = EPERM          , .str = "EPERM" },
	{ .err = EPROTONOSUPPORT, .str = "EPROTONOSUPPORT" },
	{ .err = EROFS          , .str = "EROFS" },
	{ .err = ETXTBSY        , .str = "ETXTBSY" },
	{ .err = EWOULDBLOCK    , .str = "EWOULDBLOCK" },
};

/* return an error message for errno <err> */
static const char *errmsg(int err)
{
	int i;
	for (i = 0; i < sizeof(errstr)/sizeof(errstr[0]); i++)
		if (errstr[i].err == err)
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

/* open file <name> for read, read the maximum of it into <buffer>, but not
 * more than <size> bytes. A terminating zero is always added after a read
 * succeeds. The zero lies within <size> but is not counted in the return
 * value. The number of bytes read is returned. Zero is returned if the file
 * was empty, <0 is returned in case of any error.
 */
static int readfile(const char *name, char *buffer, int size)
{
	int fd, ret;
	char *orig;

	fd = ret = open(name, O_RDONLY);
	if (ret < 0)
		goto out;

	orig = buffer;
	do {
		ret = read(fd, buffer, size);
		if (ret < 0)
			goto out_close;
		if (ret == 0)
			break;
		size -= ret;
		buffer += ret;
	} while (size > 0);

	/* we always want to stuff the terminating zero, even if that implies
	 * to truncate the result.
	 */
	if (!size)
		buffer--;
	ret = buffer - orig;
	*buffer = 0;

 out_close:
	close(fd);
 out:
	return ret;
}

/* if ret < 0, report msg with perror and return -ret.
 * if ret > 0, return msg on stderr and return ret
 * if ret == 0, return msg on stdout and return 0.
 * if msg is NULL, nothing is reported.
 */
static inline void die(int ret, const char *msg)
{
#ifndef QUIET
	if (ret < 0) {
		ret = -ret;
		if (msg)
			fdperror(2, msg);
	}
	else if (msg) {
		fdputs((ret == 0) ? 1 : 2, msg);
	}
#endif
	exit(ret);
}

/*
 * This function simply returns a locally allocated string containing
 * the ascii representation for number 'n' in decimal.
 */
static inline const char *ultoa_r(unsigned long n, char *buffer, int size)
{
	char *pos;

	pos = buffer + size - 1;
	*pos-- = '\0';

	do {
		*pos-- = '0' + n % 10;
		n /= 10;
	} while (n && pos >= buffer);
	return pos + 1;
}

/* return next line of buffer <buffer> after <start>, which may hold last
 * return value. On first call, <start> must be NULL so that the beginning of
 * <buffer> is returned first. When end of buffer is reached (\0), NULL is
 * returned. The caller must be careful about setting <start> after any \0 if
 * it truncates strings.
 */
static char *nextline(char *buffer, char *start)
{
	if (start)
		while (*start && *(start++) != '\n');
	else
		start = buffer;

	if (!*start)
		return NULL;
	return start;
}

/* return a pointer to a struct if_status already existing or just created
 * matching this interface name and type. NULL is returned if the interface
 * does not exist and cannot be created. The name pointer is just copied, so
 * the caller must allocate it if required.
 */
struct if_status *getif(const char *name, int type)
{
	int if_num;

	for (if_num = 0; if_num < nbifs; if_num++)
		if (ifs[if_num].type == type &&
		    strcmp(name, ifs[if_num].name) == 0)
			return &ifs[if_num];

	if (nbifs >= MAXIFS)
		return NULL;

	ifs[nbifs].name = name;
	ifs[nbifs].type = type;
	nbifs++;

	return &ifs[nbifs-1];
}

/* return link status for interface <dev> using socket <sock>.
 * return 0 if down, 1 if up, -1 if any error (check errno).
 */
int glink(int sock, const char *dev)
{
	struct ifreq ifr;
	struct ethtool_value edata;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name)-1);

	edata.cmd = ETHTOOL_GLINK;
	ifr.ifr_data = (void *)&edata;
	if (ioctl(sock, SIOCETHTOOL, &ifr) != 0)
		return -1;

	return edata.data ? 1 : 0;
}

/* return 1 if interface <dev> is up, otherwise 0. */
int if_up(int sock, const char *dev)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name)-1);

	if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0)
		return 0;
	return (ifr.ifr_flags & IFF_UP) ? 1 : 0;
}

/* Returns the number of existing devices found in /proc/net/dev. Their
 * corresponding entry gets ->present set to 1 if the device exists, or zero
 * if it was not found. Note that it is permitted to have several interfaces
 * with the same name. It is important to check for device existence before
 * querying it, because it avoids the automatic modprobe the system may do
 * for absent devices.
 */
int if_exist()
{
	char buffer[4096];  // should be enough for about 25 interfaces
	int ret = 0;
	int if_num;
	char *line;

	for (if_num = 0; if_num < nbifs; if_num++)
		ifs[if_num].present = 0;

	if (readfile("/proc/net/dev", buffer, sizeof(buffer)) <= 0)
		return 0;

	line = NULL;
	while ((line = nextline(buffer, line)) != NULL) {
		char *name;

		while (isspace(*line))
			line++;
		name = line;

		while (*line && !isspace(*line) && *line != ':')
			line++;

		/* if line points to ':', we have a name before it */
		if (*line != ':')
			continue;
		*(line++) = 0;

		for (if_num = 0; if_num < nbifs; if_num++) {
			if (strcmp(name, ifs[if_num].name) == 0) {
				ifs[if_num].present = 1;
				ret++;
			}
		}
	}
	return ret;
}

/* retrieve CPU usage from /proc/uptime, and update cpu_total[] and cpu_idle[].
 * Return 0 if any error, or 1 if values were updated.
 */
int update_cpu(struct led *led)
{
	char buffer[256];
	char *ptr;
	unsigned int total, idle;

	if (readfile("/proc/uptime", buffer, sizeof(buffer)) <= 0)
		return 0;

	/* format : 
	 * cpu_total_sec.centisec cpu_idle_sec.centisec
	 */

	total = 0;
	ptr = buffer;
	while (*ptr && *ptr != ' ') {
		if (isdigit(*ptr)) /* ignore dot */
			total = total*10 + *ptr - '0';
		ptr++;
	}

	while (isspace(*ptr))
		ptr++;

	idle = 0;
	while (*ptr && *ptr != '\n') {
		if (isdigit(*ptr)) /* ignore dot */
			idle = idle*10 + *ptr - '0';
		ptr++;
	}

	led->cpu.cpu_total[0] = led->cpu.cpu_total[1];
	led->cpu.cpu_total[1] = total;
	led->cpu.cpu_idle[0] = led->cpu.cpu_idle[1];
	led->cpu.cpu_idle[1] = idle;

	total = led->cpu.cpu_total[1] - led->cpu.cpu_total[0];
	idle = led->cpu.cpu_idle[1] - led->cpu.cpu_idle[0];
	if (idle > total) // kernel 2.6 workaround
		idle = total;
	/* CPU usage between 0 and 100 */
	if (led->cpu.cpu_total[0] && total)
		led->cpu.cpu_usage = ((total - idle)*100) / total;
	if (led->cpu.cpu_usage < 0)
		led->cpu.cpu_usage = 0;
	else if (led->cpu.cpu_usage > 100)
		led->cpu.cpu_usage = 100;

	return 1;
}

/* retrieve IDE interrupt counts from /proc/interrupts, and update ide_count[].
 * Lines with device names beginning with 'ide' and 'pata' are cumulated.
 * Return 0 if any error, or 1 if values were updated.
 */
int update_disk(struct led *led)
{
	char buffer[256];
	FILE *f;
	char *ptr;
	unsigned int total, count;

	f = fopen("/proc/interrupts", "r");
	if (!f)
		return 0;

	total = 0;
	while (fgets(buffer, sizeof(buffer), f) != NULL) {
		/* format : 
		 * [ 0-9]*:    count   pic   device[, device]
		 */

		ptr = buffer;
		while (*ptr != ':') {
			if (!*ptr || (*ptr != ' ' && !isdigit(*ptr)))
				goto next_line;
			ptr++;
		}

		/* skip the colon and the spaces */
		while (isspace(*++ptr));

		/* read counter */
		count = 0;
		while (isdigit(*ptr)) {
			count = count*10 + *ptr - '0';
			ptr++;
		}
		if (!*ptr)
			goto next_line;

		/* skip the spaces */
		while (isspace(*++ptr));
		if (!*ptr)
			goto next_line;

		/* skip the PIT names */
		while (*ptr && !isspace(*++ptr));

		/* skip the spaces again */
		while (isspace(*++ptr));
		if (!*ptr)
			goto next_line;

		/* OK, we have the device(s) name here. Iterate over all names */
		while (1) {
			const char *dev;

			dev = ptr;
			while (*ptr && *ptr != ',')
				ptr++;

			if (*ptr)
				*(ptr++) = 0;

			if (strncmp(dev, "ide", 3) == 0 || strncmp(dev, "pata", 4) == 0)
				/* got it ! */
				break;

			if (!*ptr)
				goto next_line;

			/* skip the comma and spaces again */
			while (isspace(*++ptr));
			if (!*ptr)
				goto next_line;
		}

		/* if we get here, we found the right line */
		total += count;
	next_line:
		;
	}
	fclose(f);

	led->ide.count[0] = led->ide.count[1];
	led->ide.count[1] = total;
	led->ide.disk_usage = led->ide.count[1] - led->ide.count[0];

	return 1;
}


static inline void setled(unsigned leds, unsigned mask, unsigned port)
{
#ifndef DEBUG
	outl(leds & mask, port);
#endif
}

void manage_disk(struct led *led)
{
	if (led->state == 0) {
		setled(led->mask, ~LED_ON, led->port);
		if (update_disk(led))
			led->state = 1;
		led->sleep = SLEEP_1SEC * 250/1000;
		/* we need two measures */
		return;
	}

	/* just check stats at the beginning of a period */
	if (led->state <= 2)
		update_disk(led);

	/* do not switch led status during intermediate states */
	if (led->state == 1 || led->state == 3)
		led->state = (led->ide.disk_usage) ? 2 : 1;
	else
		led->state++;

	/* We want 100ms ON/25ms OFF every time we see disk activity */
	switch (led->state) {
	case 1: /* led is off for at least 250 ms */
		setled(led->mask, ~LED_ON, led->port);
		led->sleep = (SLEEP_1SEC * 250/1000);
		break;
	case 2: /* led is ON */
		setled(led->mask, LED_ON, led->port);
		led->sleep = (SLEEP_1SEC * 100/1000);
		break;
	case 3: /* led flashes OFF */
		setled(led->mask, ~LED_ON, led->port);
		led->sleep = (SLEEP_1SEC * 25/1000);
		break;
	}
}

void manage_cpu(struct led *led)
{
	if (led->state == 0) {
		if (update_cpu(led))
			led->state = 1;
		led->count = 0;
		led->limit = 1;
		/* we need two measures */
		led->sleep = SLEEP_1SEC / 2;
		return;
	}

	led->count++;
	if (led->count >= led->limit) {
		int last_usage = led->cpu.cpu_usage;
		int diff;

		update_cpu(led);
		/* We want 500ms ON/500ms OFF at 0% CPU, and 40ms ON/60 ms OFF at 100%,
		 * which means that we come here 10 times faster at 100%. If we detect
		 * a fast variation, we will plan to quickly recheck.
		 */

		diff = (led->cpu.cpu_usage - last_usage);
		if (diff < 0)
			diff = -diff;

		if (diff < 10)
			led->limit = led->cpu.cpu_usage / 10;
		else
			led->limit = led->cpu.cpu_usage / 50;
		led->count = 0;
	}

	switch (led->state) {
	case 1:
		led->sleep = (SLEEP_1SEC * 40/1000) + (SLEEP_1SEC * 46/10000) * (100 - led->cpu.cpu_usage);
		setled(led->mask, LED_ON, led->port);
		led->state = 2;
		break;
	case 2:
		led->sleep = (SLEEP_1SEC * 60/1000) + (SLEEP_1SEC * 44/10000) * (100 - led->cpu.cpu_usage);
		setled(led->mask, ~LED_ON, led->port);
		led->state = 1;
		break;
	}
}

void manage_running(struct led *led)
{
	switch (led->state) {
	case 0: led->state = 1;
		/* fall through */
	case 1:
		setled(led->mask, LED_ON, led->port);
		led->sleep = fast_mode ? SLEEP_1SEC * 5 / 100 : SLEEP_1SEC * 40/100;
		led->state = 2;
		break;
	case 2:
		setled(led->mask, ~LED_ON, led->port);
		led->sleep = fast_mode ? SLEEP_1SEC * 5 / 100 : SLEEP_1SEC * 60/100;
		led->state = 1;
		break;
	}
}

void manage_net(struct led *led)
{
	enum { ETH_UP = 1, SLAVE_UP = 2, TUN_UP = 4 };
	unsigned char status = ETH_UP | SLAVE_UP | TUN_UP;

	switch (led->state) {
	case 0: led->state = 1;
		/* fall through */
	case 1:
		if_exist();
		if (led->intf) {
			led->intf->status =
				led->intf->present &&
				(if_up(net_sock, led->intf->name) == 1) &&
				(glink(net_sock, led->intf->name) == 1);
			if (!led->intf->status)
				status &= ~ETH_UP;
		}

		if (led->slave) {
			led->slave->status =
				led->slave->present &&
				(if_up(net_sock, led->slave->name) == 1);
			if (!led->slave->status)
				status &= ~SLAVE_UP;
		}

		if (led->tun) {
			led->tun->status =
				led->tun->present &&
				(if_up(net_sock, led->tun->name) == 1);
			if (!led->tun->status)
				status &= ~TUN_UP;
		}

		if (status == (ETH_UP | SLAVE_UP | TUN_UP)) {
			led->limit = MAXSTEPS; // always on if eth & slave & tun UP
			led->flash = 0;
		}
		else if (status == (ETH_UP | SLAVE_UP)) {
			led->limit = MAXSTEPS; // flashes if eth & slave UP
			led->flash = 1;
		}
		else if (status & ETH_UP) {
			led->limit = MAXSTEPS/2;  // 50% ON/OFF if only eth UP
			led->flash = 0;
		}
		else {
			led->limit = 0;  // always off if eth DOWN
			led->flash = 0;
		}

		if (led->count == MAXSTEPS-1 && led->flash) {
			setled(led->mask, LED_ON, led->port);
			led->sleep = SLEEP_500M * 45/100;
			led->state = 2;
		}
		else if (led->count < led->limit) {
			setled(led->mask, LED_ON, led->port);
			led->sleep = SLEEP_500M;
		}
		else {
			setled(led->mask, ~LED_ON, led->port);
			led->sleep = SLEEP_500M;
		}

#ifdef DEBUG
		printf("manage_net: led=%p, state=%d count=%d limit=%d flash=%d intf=%d slave=%d tun=%d\n",
		       led, led->state, led->count, led->limit, led->flash,
		       !!(status & ETH_UP), !!(status & SLAVE_UP), !!(status & TUN_UP));
#endif
		break;
	case 2:
		setled(led->mask, ~LED_ON, led->port);
		led->sleep = SLEEP_500M * 15/100;
		led->state = 3;
		break;
	case 3:
		setled(led->mask, LED_ON, led->port);
		led->sleep = SLEEP_500M * 25/100;
		led->state = 4;
		break;
	case 4:
		setled(led->mask, ~LED_ON, led->port);
		led->sleep = SLEEP_500M * 15/100;
		led->state = 1;
		break;
	}

	if (led->state == 1) {
		led->count++;
		if (led->count >= MAXSTEPS)
			led->count = 0;
	}
}

void sig_handler(int sig)
{
	switch (sig) {
	case SIGUSR1:
		fast_mode = 0;
		break;
	case SIGUSR2:
		fast_mode = 1;
		break;
	}
	signal(sig, sig_handler);
}

static inline void init_leds(struct led *led)
{
	led[0].port = LED1_PORT;
	led[0].mask = LED1_MASK;

	led[1].port = LED2_PORT;
	led[1].mask = LED2_MASK;

	led[2].port = LED3_PORT;
	led[2].mask = LED3_MASK;
}

int main(int argc, char **argv)
{
	struct sched_param sch;
	struct led *led = NULL;
	const char *last_interf = NULL;
	const char *pidname = NULL;
	int pidfd = 0;
	int pid, fd;
	int sched;
	int prio = 0;

	/* cheaper than pre-initializing the array in the .data section */
	init_leds(leds);
	net_sock = -2; /* uninitialized */

	argc--; argv++;
	while (argc > 0) {
		if (**argv != '-')
			die(1, usage);

		/* options with one arg first */
		if (argv[0][1] == 'h')
			die(0, usage);
		else if (argv[0][1] == 'd') {
			if (!led)
				die(1, "Must specify led before disk mode");
			if (led->type != LED_UNUSED && led->type != LED_DISK)
				die(1, "LED already assigned to non-disk polling");
			led->type = LED_DISK;
		}
		else if (argv[0][1] == 'u') {
			if (!led)
				die(1, "Must specify led before cpu mode");
			if (led->type != LED_UNUSED && led->type != LED_CPU)
				die(1, "LED already assigned to non-cpu polling");
			led->type = LED_CPU;
		}
		else if (argv[0][1] == 'r') {
			if (!led)
				die(1, "Must specify led before running mode");
			if (led->type != LED_UNUSED && led->type != LED_RUNNING)
				die(1, "LED already assigned to non-running polling");
			led->type = LED_RUNNING;
		}
		else if (argv[0][1] == 'R') {
			if (!led)
				die(1, "Must specify led before fast running mode");
			if (led->type != LED_UNUSED && led->type != LED_RUNNING)
				die(1, "LED already assigned to non-running polling");
			led->type = LED_RUNNING;
			fast_mode = 1;
		}
		else if (argv[0][1] == 'I')
			prio = 1;

		/* options with two args below */
		else if (argc < 2)
				die(1, usage);

		else if (argv[0][1] == 'i') {
			if (!led)
				die(1, "Must specify led before interface");
			if (led->type != LED_UNUSED && led->type != LED_NET)
				die(1, "LED already assigned to non-net polling");
			led->type = LED_NET;
			led->intf = getif(argv[1], IF_TYPE_PHYSICAL);
			if (!led->intf)
				die(1, "Too many interfaces");
			last_interf = argv[1];
			net_sock = -1;
			argc--; argv++;
		}
		else if (argv[0][1] == 's') {
			if (!led)
				die(1, "Must specify led before slave");
			if (led->type != LED_UNUSED && led->type != LED_NET)
				die(1, "LED already assigned to non-net polling");
			led->type = LED_NET;
			led->slave = getif(argv[1], IF_TYPE_LOGICAL);
			if (!led->slave)
				die(1, "Too many interfaces");
			net_sock = -1;
			argc--; argv++;
		}
		else if (argv[0][1] == 't') {
			if (!led)
				die(1, "Must specify led before tunnel");
			if (led->type != LED_UNUSED && led->type != LED_NET)
				die(1, "LED already assigned to non-net polling");
			led->type = LED_NET;
			led->tun = getif(argv[1], IF_TYPE_LOGICAL);
			if (!led->tun)
				die(1, "Too many interfaces");
			net_sock = -1;
			argc--; argv++;
		}
		else if (argv[0][1] == 'l') {
			int l = atoi(argv[1]);
			if (l >= 1 && l <= 3)
				led = &leds[l - 1];
			else
				die(1, usage);
			argc--; argv++;
		}
		else if (argv[0][1] == 'p') {
			pidname = argv[1];
			argc--; argv++;
		}
		else
			die(1, usage);
		argc--; argv++;
	}

	if (iopl(3) == -1)
#ifndef DEBUG
		die(-1, "Cannot get I/O port");
#else
	;
#endif
	if (net_sock == -1) {
		/* at least one interface requires network status */
		net_sock = socket(PF_INET, SOCK_DGRAM, 0);
		if (net_sock < 0)
			die(-2, "Failed to get socket");
		if (last_interf) {
			/* if we want to monitor netlink, we may need some privileges */
			if (glink(net_sock, last_interf) == -1 && errno == EPERM)
				die(-3, "Failed to get link status");
		}
	}

	if (prio > 0) {
		/* set idle priority */
		prio = 20; // nice value in case of failure
		sched = SCHED_IDLEPRIO;
		sch.sched_priority = 0;
	} else {
		/* defaults to realtime priority */
		prio = -19; // nice value in case of failure
		sched = SCHED_RR;
		sch.sched_priority = 1;
	}
	if (sched_setscheduler(0, sched, &sch) == -1) {
		sched_setscheduler(0, SCHED_OTHER, &sch);
		setpriority(PRIO_PROCESS, 0, prio);
	}

	signal(SIGUSR1, sig_handler);
	signal(SIGUSR2, sig_handler);

#ifndef DEBUG
	if (pidname) {
		pidfd = open(pidname, O_WRONLY|O_CREAT|O_TRUNC);
		if (pidfd < 0)
			die(-4, "Failed to open pidfile");
	}

	chdir("/");

	/* close only stdin/stdout/stderr (not dgram socket or pidfile) */
	for (fd = 0; fd < 1024; fd++)
		if (net_sock != fd && (!pidname || fd != pidfd))
			close(fd);

	pid = fork();
	if (pid > 0 && pidname) {
		char buffer[21];
		char *ret;

		ret = (char *)ultoa_r(pid, buffer, sizeof(buffer));
		fdputs(pidfd, ret);
	}
	if (pidname)
		close(pidfd);

	if (pid != 0)
		exit(0);
#endif

	/* mini-scheduler
	 * This one is only based on delays and not deadlines. This makes it
	 * simpler and more robust against time changes. However it is not
	 * scalable and should never process hundreds of tasks.
	 */
	while (1) {
		int led_num;
		int sleep_time = MAXSLEEP;

		for (led_num = 0; led_num < 3; led_num++) {
			led = &leds[led_num];

			if (led->type == LED_UNUSED)
				continue;

			if (led->sleep > 0)
				continue;

			/* led timer expired */
			switch (led->type) {
			case LED_NET:
				manage_net(led);
				break;
			case LED_RUNNING:
				manage_running(led);
				break;
			case LED_CPU:
				manage_cpu(led);
				break;
			case LED_DISK:
				manage_disk(led);
				break;
			}
		}

		for (led_num = 0; led_num < 3; led_num++) {
			led = &leds[led_num];
			if (led->type != LED_UNUSED && led->sleep < sleep_time)
				sleep_time = led->sleep;
		}

		if (sleep_time > 1000000)
			sleep_time = 1000000;

		/* Sleep and ignore signals. We will drift but its not dramatic */
		while (usleep(sleep_time) != 0 && errno == EINTR);

		/* update all leds' sleep time */
		for (led_num = 0; led_num < 3; led_num++) {
			led = &leds[led_num];
			if (led->type != LED_UNUSED)
				led->sleep -= sleep_time;
		}		
	}
}
