/*
 * This program demonstrates transmission of L2 frames using the
 * system TAI timer.
 *
 * Copyright (c) 2018, Intel Corporation
 *
 * Copyright (C) 2017 linutronix GmbH
 *
 * Large portions taken from the linuxptp stack.
 * Copyright (C) 2011, 2012 Richard Cochran <richardcochran@gmail.com>
 *
 * Some portions taken from the sgd test program.
 * Copyright (C) 2015 linutronix GmbH
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#define _GNU_SOURCE /*for CPU_SET*/
#include <errno.h>
#include <ifaddrs.h>
#include <linux/errqueue.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ONE_SEC			1000000000ULL
#define DEFAULT_PERIOD		1000000
#define DEFAULT_DELAY		500000
#define DEFAULT_PRIORITY	3
#define MARKER			'a'

#ifndef SO_TXTIME
#define SO_TXTIME		61
#define SCM_TXTIME		SO_TXTIME
#endif

#ifndef SO_EE_ORIGIN_TXTIME
#define SO_EE_ORIGIN_TXTIME		6
#define SO_EE_CODE_TXTIME_INVALID_PARAM	1
#define SO_EE_CODE_TXTIME_MISSED	2
#endif

#define pr_err(s)	fprintf(stderr, s "\n")
#define pr_info(s)	fprintf(stdout, s "\n")

/* The API for SO_TXTIME is the below struct and enum, which will be
 * provided by uapi/linux/net_tstamp.h in the near future.
 */
struct sock_txtime {
	clockid_t clockid;
	uint16_t flags;
};

enum txtime_flags {
	SOF_TXTIME_DEADLINE_MODE = (1 << 0),
	SOF_TXTIME_REPORT_ERRORS = (1 << 1),

	SOF_TXTIME_FLAGS_LAST = SOF_TXTIME_REPORT_ERRORS,
	SOF_TXTIME_FLAGS_MASK = (SOF_TXTIME_FLAGS_LAST - 1) |
				 SOF_TXTIME_FLAGS_LAST
};


static int running = 1, use_so_txtime = 1;
static int period_nsec = DEFAULT_PERIOD;
static int waketx_delay = DEFAULT_DELAY;
static int so_priority = DEFAULT_PRIORITY;
static int use_deadline_mode = 0;
static int receive_errors = 0;
static uint64_t base_time = 0;
static uint8_t mac_addr[ETH_ALEN] = {0};
static struct sock_txtime sk_txtime;
static struct sockaddr_ll addr = {0};

static void normalize(struct timespec *ts)
{
	while (ts->tv_nsec > 999999999) {
		ts->tv_sec += 1;
		ts->tv_nsec -= ONE_SEC;
	}

	while (ts->tv_nsec < 0) {
		ts->tv_sec -= 1;
		ts->tv_nsec += ONE_SEC;
	}
}

static int sk_interface_index(int fd, const char *name)
{
	struct ifreq ifreq;
	int err;

	memset(&ifreq, 0, sizeof(ifreq));
	strncpy(ifreq.ifr_name, name, sizeof(ifreq.ifr_name) - 1);
	err = ioctl(fd, SIOCGIFINDEX, &ifreq);
	if (err < 0) {
		pr_err("ioctl SIOCGIFINDEX failed: %m");
		return err;
	}
	return ifreq.ifr_ifindex;
}

static int l2_open_socket(const char *name, clockid_t clkid)
{
	int fd, index, on = 1;
	addr.sll_family = AF_PACKET,
	addr.sll_protocol = htons(ETH_P_TSN),
	addr.sll_halen = ETH_ALEN,

	fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_TSN));
	if (fd < 0) {
		pr_err("socket failed: %m");
		goto no_socket;
	}

	index = sk_interface_index(fd, name);
	if (index < 0)
		goto no_option;

	addr.sll_ifindex = index;

	if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &so_priority, sizeof(so_priority))) {
		pr_err("Couldn't set priority");
		goto no_option;
	}

	memcpy(&addr.sll_addr, mac_addr, ETH_ALEN);

	sk_txtime.clockid = clkid;
	sk_txtime.flags = (use_deadline_mode | receive_errors);
	if (use_so_txtime && setsockopt(fd, SOL_SOCKET, SO_TXTIME, &sk_txtime, sizeof(sk_txtime))) {
		pr_err("setsockopt SO_TXTIME failed: %m");
		goto no_option;
	}

	return fd;
no_option:
	close(fd);
no_socket:
	return -1;
}

static int l2_send(int fd, void *buf, int len, __u64 txtime)
{
	char control[CMSG_SPACE(sizeof(txtime))] = {};
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec iov;
	ssize_t cnt;

	iov.iov_base = buf;
	iov.iov_len = len;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &addr;
	msg.msg_namelen = sizeof(addr);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	/*
	 * We specify the transmission time in the CMSG.
	 */
	if (use_so_txtime) {
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);

		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_TXTIME;
		cmsg->cmsg_len = CMSG_LEN(sizeof(__u64));
		*((__u64 *) CMSG_DATA(cmsg)) = txtime;
	}

	cnt = sendmsg(fd, &msg, 0);
	if (cnt < 1) {
		pr_err("sendmsg failed: %m");
		return cnt;
	}

	return cnt;
}

static unsigned char tx_buffer[256];

static int process_socket_error_queue(int fd)
{
	uint8_t msg_control[CMSG_SPACE(sizeof(struct sock_extended_err))];
	unsigned char err_buffer[sizeof(tx_buffer)];
	struct sock_extended_err *serr;
	struct cmsghdr *cmsg;
	__u64 tstamp = 0;

	struct iovec iov = {
	        .iov_base = err_buffer,
	        .iov_len = sizeof(err_buffer)
	};
	struct msghdr msg = {
	        .msg_iov = &iov,
	        .msg_iovlen = 1,
	        .msg_control = msg_control,
	        .msg_controllen = sizeof(msg_control)
	};

	if (recvmsg(fd, &msg, MSG_ERRQUEUE) == -1) {
		pr_err("recvmsg failed");
	        return -1;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	while (cmsg != NULL) {
		serr = (void *) CMSG_DATA(cmsg);
		if (serr->ee_origin == SO_EE_ORIGIN_TXTIME) {
			tstamp = ((__u64) serr->ee_data << 32) + serr->ee_info;

			switch(serr->ee_code) {
			case SO_EE_CODE_TXTIME_INVALID_PARAM:
				fprintf(stderr, "packet with tstamp %llu dropped due to invalid params\n", tstamp);
				return 0;
			case SO_EE_CODE_TXTIME_MISSED:
				fprintf(stderr, "packet with tstamp %llu dropped due to missed deadline\n", tstamp);
				return 0;
				default:
					return -1;
			}
		}

		cmsg = CMSG_NXTHDR(&msg, cmsg);
	}

	return 0;
}

static int run_nanosleep(clockid_t clkid, int fd)
{
	struct timespec ts;
	int cnt, err;
	__u64 txtime;
	struct pollfd p_fd = {
		.fd = fd,
	};

	memset(tx_buffer, MARKER, sizeof(tx_buffer));

	/* If no base-time was specified, start one to two seconds in the
	 * future.
	 */
	if (base_time == 0) {
		clock_gettime(clkid, &ts);
		ts.tv_sec += 1;
		ts.tv_nsec = ONE_SEC - waketx_delay;
	} else {
		ts.tv_sec = base_time / ONE_SEC;
		ts.tv_nsec = (base_time % ONE_SEC) - waketx_delay;
	}

	normalize(&ts);

	txtime = ts.tv_sec * ONE_SEC + ts.tv_nsec;
	txtime += waketx_delay;

	fprintf(stderr, "\ntxtime of 1st packet is: %llu", txtime);

	while (running) {
		memcpy(tx_buffer, &txtime, sizeof(__u64));
		err = clock_nanosleep(clkid, TIMER_ABSTIME, &ts, NULL);
		switch (err) {
		case 0:
			cnt = l2_send(fd, tx_buffer, sizeof(tx_buffer), txtime);
			if (cnt != sizeof(tx_buffer)) {
				pr_err("send failed");
			}
			ts.tv_nsec += period_nsec;
			normalize(&ts);
			txtime += period_nsec;

			/* Check if errors are pending on the error queue. */
			err = poll(&p_fd, 1, 0);
			if (err == 1 && p_fd.revents & POLLERR) {
				if (!process_socket_error_queue(fd))
					return -ECANCELED;
			}

			break;
		case EINTR:
			continue;
		default:
			fprintf(stderr, "clock_nanosleep returned %d: %s",
				err, strerror(err));
			return err;
		}
	}

	return 0;
}

static int set_realtime(pthread_t thread, int priority, int cpu)
{
	cpu_set_t cpuset;
	struct sched_param sp;
	int err, policy;

	int min = sched_get_priority_min(SCHED_FIFO);
	int max = sched_get_priority_max(SCHED_FIFO);

	fprintf(stderr, "min %d max %d\n", min, max);

	if (priority < 0) {
		return 0;
	}

	err = pthread_getschedparam(thread, &policy, &sp);
	if (err) {
		fprintf(stderr, "pthread_getschedparam: %s\n", strerror(err));
		return -1;
	}

	sp.sched_priority = priority;

	err = pthread_setschedparam(thread, SCHED_FIFO, &sp);
	if (err) {
		fprintf(stderr, "pthread_setschedparam: %s\n", strerror(err));
		return -1;
	}

	if (cpu < 0) {
		return 0;
	}
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	err = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
	if (err) {
		fprintf(stderr, "pthread_setaffinity_np: %s\n", strerror(err));
		return -1;
	}

	return 0;
}

static void usage(char *progname)
{
	fprintf(stderr,
		"\n"
		"usage: %s [options]\n"
		"\n"
		" -c [num]      run on CPU 'num'\n"
		" -d [num]      delta from wake up to txtime in nanoseconds (default %d)\n"
		" -h            prints this message and exits\n"
		" -i [name]     use network interface 'name'\n"
		" -p [num]      run with RT priorty 'num'\n"
		" -P [num]      period in nanoseconds (default %d)\n"
		" -s            do not use SO_TXTIME\n"
		" -t [num]      set SO_PRIORITY to 'num' (default %d)\n"
		" -D            set deadline mode for SO_TXTIME\n"
		" -E            enable error reporting on the socket error queue for SO_TXTIME\n"
		" -b [tstamp]   txtime of 1st packet as a 64bit [tstamp]. Default: now + ~2seconds\n"
		" -m [mac_addr] dst MAC address\n"
		"\n",
		progname, DEFAULT_DELAY, DEFAULT_PERIOD, DEFAULT_PRIORITY);
}

int main(int argc, char *argv[])
{
	int c, cpu = -1, err, fd, priority = -1;
	clockid_t clkid = CLOCK_TAI;
	char *iface = NULL, *progname;

	/* Process the command line arguments. */
	progname = strrchr(argv[0], '/');
	progname = progname ? 1 + progname : argv[0];
	while (EOF != (c = getopt(argc, argv, "c:d:hi:p:P:st:DEb:m:"))) {
		switch (c) {
		case 'c':
			cpu = atoi(optarg);
			break;
		case 'd':
			waketx_delay = atoi(optarg);
			break;
		case 'h':
			usage(progname);
			return 0;
		case 'i':
			iface = optarg;
			break;
		case 'p':
			priority = atoi(optarg);
			break;
		case 'P':
			period_nsec = atoi(optarg);
			break;
		case 's':
			use_so_txtime = 0;
			break;
		case 't':
			so_priority = atoi(optarg);
			break;
		case 'D':
			use_deadline_mode = SOF_TXTIME_DEADLINE_MODE;
			break;
		case 'E':
			receive_errors = SOF_TXTIME_REPORT_ERRORS;
			break;
		case 'b':
			base_time = atoll(optarg);
			break;
		case 'm':
			err = sscanf(optarg, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
				&mac_addr[0], &mac_addr[1], &mac_addr[2],
				&mac_addr[3], &mac_addr[4], &mac_addr[5]);
			if (err != 6) {
				printf("Invalid MAC address\n");
				return -1;
			}

			break;
		case '?':
			usage(progname);
			return -1;
		}
	}

	if (mac_addr[0] == 0 && mac_addr[1] == 0 && mac_addr[2] == 0) {
		pr_err("Destination MAC Address must be specified.");
		usage(progname);
		return -1;
	}

	if (waketx_delay > 999999999 || waketx_delay < 0) {
		pr_err("Bad wake up to transmission delay.");
		usage(progname);
		return -1;
	}

	if (period_nsec < 1000) {
		pr_err("Bad period.");
		usage(progname);
		return -1;
	}

	if (!iface) {
		pr_err("Need a network interface.");
		usage(progname);
		return -1;
	}

	if (set_realtime(pthread_self(), priority, cpu)) {
		return -1;
	}

	fd = l2_open_socket(iface, clkid);
	if (fd < 0) {
		return -1;
	}

	err = run_nanosleep(clkid, fd);

	close(fd);
	return err;
}
