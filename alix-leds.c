/*
 * alix-leds : blink leds on ALIX motherboards depending on network status.
 * (C) 2008 - Willy Tarreau <w@1wt.eu> - Redistribute under GPLv2
 *
 * Usage:
 *   alix-leds [eth] [ppp] [tun]   (defaults to eth2, ppp0 and tun0)
 *
 * Led3 will be off when eth is down, will slowly blink when eth is up and ppp
 * down, and will emit two quick flashes when eth&ppp are up but tun is down.
 * Otherwise, if all are up, it will remain lit.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <errno.h>
#include <sched.h>
#include <net/if.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/time.h>
//#include <sys/types.h>
#include <sys/resource.h>

#include <linux/types.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>


#undef SCHED_IDLEPRIO
#define SCHED_IDLEPRIO  5

#ifndef SIOCETHTOOL
#define SIOCETHTOOL     0x8946
#endif

#define SLEEPTIME 500000
#define MAXSTEPS  2

/* ALIX leds */
#define LED1_PORT 0x6100
#define LED2_PORT 0x6180
#define LED3_PORT 0x6180
#define LED1_MASK 0x00400040
#define LED2_MASK 0x02000200
#define LED3_MASK 0x08000800
#define LED_ON    0xFFFF0000

struct if_status {
	const char *name;
	int present;
	int status;
};

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

/* return 1 if interface <dev> exists, otherwise 0. The problem
 * is that the system will do a modprobe every time, which eats
 * a lot of CPU.
 */
int if_exist_ioctl(int sock, const char *dev)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name)-1);

	if (ioctl(sock, SIOCGIFINDEX, &ifr) != 0)
		return 0;
	return 1;
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

/* Returns the number of existing devices found in /proc/net/dev.
 * Their corresponding entry gets ->present set to 1 if the device
 * exists, or zero if it was not found. Note that it is permitted
 * to have several interfaces with the same name.
 */
int if_exist(struct if_status *if1, struct if_status *if2, struct if_status *if3)
{
	char buffer[256];
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

		if (strcmp(name, if1->name) == 0) {
			if1->present = 1;
			ret++;
		}
		if (strcmp(name, if2->name) == 0) {
			if2->present = 1;
			ret++;
		}
		if (strcmp(name, if3->name) == 0) {
			if3->present = 1;
			ret++;
		}
	}
	fclose(f);
	return ret;
}

void setled(unsigned leds, unsigned mask, unsigned port)
{
	outl(leds & mask, port);
}

int main(int argc, char **argv)
{
	int sock;
	int count, limit, flash;
	struct sched_param sch;
	struct if_status eth, ppp, tun;

	unsigned int port   = LED3_PORT;
	unsigned int leds   = LED3_MASK;

	if (argc > 1)
		eth.name = argv[1];
	else
		eth.name = "eth2";

	if (argc > 2)
		ppp.name = argv[2];
	else
		ppp.name = "ppp0";

	if (argc > 3)
		tun.name = argv[3];
	else
		tun.name = "tun0";

	if (iopl(3) == -1)
		die(-1, "Cannot get I/O port");

	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		die(-2, "Failed to get socket");

	if (glink(sock, eth.name) == -1 && errno == EPERM)
		die(-3, "Failed to get link status");

	sch.sched_priority = 0;
	if (sched_setscheduler(0, SCHED_IDLEPRIO, &sch) == -1) {
		sched_setscheduler(0, SCHED_OTHER, &sch);
		setpriority(PRIO_PROCESS, 0, 20);
	}

#ifndef DEBUG
	chdir("/");
	close(0); close(1); close(2);
	if (fork() != 0)
		exit(0);
#endif

	/* This part runs in background */
	count = limit = flash = 0;
	while (1) {
		eth.status = 0;
		ppp.status = 0;
		tun.status = 0;

		if_exist(&eth, &ppp, &tun);
		eth.status = eth.present && (glink(sock, eth.name) == 1);
		ppp.status = ppp.present && (if_up(sock, ppp.name) == 1);
		tun.status = tun.present && (if_up(sock, tun.name) == 1);

		if (eth.status && ppp.status && tun.status) {
			limit = MAXSTEPS; // always on if eth & ppp & tun UP
			flash = 0;
		}
		else if (eth.status && ppp.status) {
			limit = MAXSTEPS; // flashes if eth & ppp UP
			flash = 1;
		}
		else if (eth.status) {
			limit = MAXSTEPS/2;  // 50% ON/OFF if only eth UP
			flash = 0;
		}
		else {
			limit = 0;  // always off if eth DOWN
			flash = 0;
		}

#ifdef DEBUG
		printf("eth0: %d eth1: %d eth2: %d ppp0: %d\n",
		       glink(sock, "eth0"),
		       glink(sock, "eth1"),
		       glink(sock, "eth2"),
		       glink(sock, "ppp0"));
		printf("eth=%d ppp=%d tun=%d => flash=%d count=%d, limit=%d, status=%d\n",
		       eth.status, ppp.status, tun.status,
		       flash, count, limit, count<limit);
		usleep(SLEEPTIME);
#else
		if (count == limit-1 && flash) {
			setled(leds, LED_ON, port);
			usleep(SLEEPTIME * 45/100);
			setled(leds, ~LED_ON, port);
			usleep(SLEEPTIME * 15/100);
			setled(leds, LED_ON, port);
			usleep(SLEEPTIME * 25/100);
			setled(leds, ~LED_ON, port);
			usleep(SLEEPTIME * 15/100);
			setled(leds, LED_ON, port);
		}
		else if (count < limit) {
			setled(leds, LED_ON, port);
			usleep(SLEEPTIME);
		}
		else {
			setled(leds, ~LED_ON, port);
			usleep(SLEEPTIME);
		}
#endif

		count++;
		if (count >= MAXSTEPS)
			count = 0;
	}
}
