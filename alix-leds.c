/*
 * alix-leds version 1.0 - (C) 2008 - Willy Tarreau <w@1wt.eu>
 * Blink LEDs on ALIX motherboards depending on network status.
 * Redistribute under GPLv2.
 *
 * Usage:
 *   alix-leds { [-l 1|2|3] [-p|-f] [-i intf] [-s slave] [-t tun] }*
 *
 * For more info, check the "usage" help string below.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <errno.h>
#include <sched.h>
#include <net/if.h>
#include <sys/io.h>
#include <sys/ioctl.h>
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
};

struct if_status {
	const char *name;
	int present;
	int status;
};

struct led {
	int type;  /* led type (LED_*). 0 = unused */
	int state; /* internal state. 0 at init. 1 for first state. */
	int sleep; /* sleep time in ms */
	unsigned int port; /* I/O port */
	unsigned int mask; /* on/off mask */
	char *disk_name;
	struct if_status intf, slave, tun; /* checked interfaces */
	int count, limit, flash;           /* used for interface status */
};

static struct led leds[3];

/* network socket */
static int net_sock = -2; /* -2 = unneeded, -1 = needed, >=0 = initialized */
static int fast_mode = 0; /* start blink fast for running led */

const char usage[] =
  "alix-leds version 1.0 - (C) 2008 - Willy Tarreau <w@1wt.eu>\n"
  "  Blink LEDs on ALIX motherboards depending on system and network status.\n"
  "\n"
  "Usage:\n"
  "  # alix-leds [-p pidfile] {[-l 1|2|3] [-r|-R] [-i intf] [-s slave] [-t tun]}*\n"
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
  "Use -p to store the daemon's pid into file <pidfile>.\n"
  "";

/* if ret < 0, report msg with perror and return -ret.
 * if ret > 0, return msg on stderr and return ret
 * if ret == 0, return msg on stdout and return 0.
 * if msg is NULL, nothing is reported.
 */
void die(int ret, const char *msg)
{
	if (ret < 0) {
		ret = -ret;
		if (msg)
			perror(msg);
	}
	else if (ret > 0) {
		if (msg)
			fprintf(stderr, "%s\n", msg);
	}
	else if (ret == 0) {
		if (msg)
			printf("%s\n", msg);
	}
	exit(ret);
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
int if_exist(struct if_status *if1, struct if_status *if2, struct if_status *if3)
{
	char buffer[256];
	struct if_status *ifs[3] = {if1, if2, if3};
	FILE *f;
	int ret = 0;

	if1->present = 0;
	if2->present = 0;
	if3->present = 0;
	f = fopen("/proc/net/dev", "r");
	if (!f)
		return 0;

	while (fgets(buffer, sizeof(buffer), f) != NULL) {
		char *name, *colon;
		int ifnum;

		name = buffer;
		while (isspace(*name))
			name++;
		colon = name;
		while (*colon && !isspace(*colon) && *colon != ':')
			colon++;
		/* if colon points to ':', we have a name before it */
		if (*colon != ':')
			continue;
		*(colon++) = 0;

		for (ifnum = 0; ifnum < 3; ifnum++) {
			if (ifs[ifnum]->name && strcmp(name, ifs[ifnum]->name) == 0) {
				ifs[ifnum]->present = 1;
				ret++;
			}
		}
	}
	fclose(f);
	return ret;
}

static inline void setled(unsigned leds, unsigned mask, unsigned port)
{
#ifndef DEBUG
	outl(leds & mask, port);
#endif
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
#ifdef DEBUG
	printf("manage_net: led=%p, state=%d count=%d limit=%d flash=%d intf=%d slave=%d tun=%d\n",
	       led, led->state, led->count, led->limit, led->flash,
	       led->intf.status, led->slave.status, led->tun.status);
#endif
	
	switch (led->state) {
	case 0: led->state = 1;
		/* fall through */
	case 1:
		if_exist(&led->intf, &led->slave, &led->tun);
		led->intf.status = !led->intf.name ||
			(led->intf.present &&
			 (if_up(net_sock, led->intf.name) == 1) &&
			 (glink(net_sock, led->intf.name) == 1));
	
		led->slave.status = !led->slave.name ||
			(led->slave.present &&
			 (if_up(net_sock, led->slave.name) == 1));
	
		led->tun.status = !led->tun.name ||
			(led->tun.present &&
			 (if_up(net_sock, led->tun.name) == 1));

		if (led->intf.status && led->slave.status && led->tun.status) {
			led->limit = MAXSTEPS; // always on if eth & slave & tun UP
			led->flash = 0;
		}
		else if (led->intf.status && led->slave.status) {
			led->limit = MAXSTEPS; // flashes if eth & slave UP
			led->flash = 1;
		}
		else if (led->intf.status) {
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
	FILE *pidfile = NULL;
	int pid;

	/* cheaper than pre-initializing the array in the .data section */
	init_leds(leds);

	argc--; argv++;
	while (argc > 0) {
		/* options with one arg first */
		if (strcmp(*argv, "-h") == 0)
			die(0, usage);
		else if (strcmp(*argv, "-r") == 0) {
			if (!led)
				die(1, "Must specify led before running mode");
			if (led->type != LED_UNUSED && led->type != LED_RUNNING)
				die(1, "LED already assigned to non-running polling");
			led->type = LED_RUNNING;
		}
		else if (strcmp(*argv, "-R") == 0) {
			if (!led)
				die(1, "Must specify led before fast running mode");
			if (led->type != LED_UNUSED && led->type != LED_RUNNING)
				die(1, "LED already assigned to non-running polling");
			led->type = LED_RUNNING;
			fast_mode = 1;
		}

		/* options with two args below */
		else if (argc < 2)
				die(1, usage);

		else if (strcmp(*argv, "-i") == 0) {
			if (!led)
				die(1, "Must specify led before interface");
			if (led->type != LED_UNUSED && led->type != LED_NET)
				die(1, "LED already assigned to non-net polling");
			led->type = LED_NET;
			led->intf.name = argv[1];
			last_interf = argv[1];
			net_sock = -1;
			argc--; argv++;
		}
		else if (strcmp(*argv, "-s") == 0) {
			if (!led)
				die(1, "Must specify led before slave");
			if (led->type != LED_UNUSED && led->type != LED_NET)
				die(1, "LED already assigned to non-net polling");
			led->type = LED_NET;
			led->slave.name = argv[1];
			net_sock = -1;
			argc--; argv++;
		}
		else if (strcmp(*argv, "-t") == 0) {
			if (!led)
				die(1, "Must specify led before tunnel");
			if (led->type != LED_UNUSED && led->type != LED_NET)
				die(1, "LED already assigned to non-net polling");
			led->type = LED_NET;
			led->tun.name = argv[1];
			net_sock = -1;
			argc--; argv++;
		}
		else if (strcmp(*argv, "-l") == 0) {
			int l = atoi(argv[1]);
			if (l >= 1 && l <= 3)
				led = &leds[l - 1];
			else
				die(1, usage);
			argc--; argv++;
		}
		else if (strcmp(*argv, "-p") == 0) {
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

	sch.sched_priority = 0;
	if (sched_setscheduler(0, SCHED_IDLEPRIO, &sch) == -1) {
		sched_setscheduler(0, SCHED_OTHER, &sch);
		setpriority(PRIO_PROCESS, 0, 20);
	}

	signal(SIGUSR1, sig_handler);
	signal(SIGUSR2, sig_handler);

#ifndef DEBUG
	if (pidname) {
		pidfile = fopen(pidname, "w");
		if (!pidfile)
			die(-4, "Failed to open pidfile");
	}

	chdir("/");
	close(0); close(1); close(2);
	pid = fork();
	if (pid > 0)
		if (pidfile)
			fprintf(pidfile, "%d\n", pid);
	if (pidfile)
		fclose(pidfile);

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
