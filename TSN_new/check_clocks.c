/*
 * Copyright (c) 2018, Intel Corporation
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>

#define ONE_SEC 1000000000ULL
#define PTP_MAX_DEV_PATH 16

/* fd to clockid helpers. Copied from posix-timers.h. */
#define CLOCKFD 3
static inline clockid_t make_process_cpuclock(const unsigned int pid,
					      const clockid_t clock)
{
	return ((~pid) << 3) | clock;
}

static inline clockid_t fd_to_clockid(const int fd)
{
	return make_process_cpuclock((unsigned int) fd, CLOCKFD);
}

static inline void open_phc_fd(int* fd_ptp, char* ifname)
{
	struct ethtool_ts_info interface_info = {0};
	char ptp_path[PTP_MAX_DEV_PATH];
	struct ifreq req = {0};
	int fd_ioctl;

	/* Get PHC index */
	interface_info.cmd = ETHTOOL_GET_TS_INFO;
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
	req.ifr_data = (char *) &interface_info;

	fd_ioctl = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd_ioctl < 0) {
		perror("Couldn't open socket");
		exit(EXIT_FAILURE);
	}

	if (ioctl(fd_ioctl, SIOCETHTOOL, &req) < 0) {
		perror("Couldn't issue SIOCETHTOOL ioctl");
		exit(EXIT_FAILURE);
	}

	snprintf(ptp_path, sizeof(ptp_path), "%s%d", "/dev/ptp",
		 interface_info.phc_index);

	*fd_ptp = open(ptp_path, O_RDONLY);
	if (*fd_ptp < 0) {
		perror("Couldn't open the PTP fd. Did you forget to run with sudo again?");
		exit(EXIT_FAILURE);
	}

	close(fd_ioctl);
}

int main(int argc, char** argv)
{
	struct timespec ts_rt1, ts_rt2, ts_ptp1, ts_ptp2, ts_tai1, ts_tai2;
	uint64_t rt, tai, ptp, lat_rt, lat_tai, lat_ptp;
	char ifname[IFNAMSIZ];
	int fd_ptp, err;

	if (argc < 2) {
		printf("You must run this as %s NET_IFACE (e.g. enp2s0)\n", argv[0]);
		return EXIT_FAILURE;
	}

	strncpy(ifname, argv[1], sizeof(ifname) - 1);
	open_phc_fd(&fd_ptp, ifname);

	/* Fetch timestamps for each clock. */
	clock_gettime(CLOCK_REALTIME, &ts_rt1);
	clock_gettime(CLOCK_TAI, &ts_tai1);
	clock_gettime(fd_to_clockid(fd_ptp), &ts_ptp1);
	rt = (ts_rt1.tv_sec * ONE_SEC) + ts_rt1.tv_nsec;
	tai = (ts_tai1.tv_sec * ONE_SEC) + ts_tai1.tv_nsec;
	ptp = (ts_ptp1.tv_sec * ONE_SEC) + ts_ptp1.tv_nsec;

	/* Compute clocks read latency. */
	clock_gettime(CLOCK_REALTIME, &ts_rt1);
	clock_gettime(CLOCK_REALTIME, &ts_rt2);
	lat_rt = ((ts_rt2.tv_sec * ONE_SEC) + ts_rt2.tv_nsec)
		   - ((ts_rt1.tv_sec * ONE_SEC) + ts_rt1.tv_nsec);

	clock_gettime(CLOCK_TAI, &ts_tai1);
	clock_gettime(CLOCK_TAI, &ts_tai2);
	lat_tai = ((ts_tai2.tv_sec * ONE_SEC) + ts_tai2.tv_nsec)
		   - ((ts_tai1.tv_sec * ONE_SEC) + ts_tai1.tv_nsec);

	clock_gettime(fd_to_clockid(fd_ptp), &ts_ptp1);
	clock_gettime(fd_to_clockid(fd_ptp), &ts_ptp2);
	lat_ptp = ((ts_ptp2.tv_sec * ONE_SEC) + ts_ptp2.tv_nsec)
		   - ((ts_ptp1.tv_sec * ONE_SEC) + ts_ptp1.tv_nsec);

	printf("rt tstamp:\t%llu\n", rt);
	printf("tai tstamp:\t%llu\n", tai);
	printf("phc tstamp:\t%llu\n", ptp);
	printf("rt latency:\t%llu\n", lat_rt);
	printf("tai latency:\t%llu\n", lat_tai);
	printf("phc latency:\t%llu\n", lat_ptp);
	printf("phc-rt delta:\t%lld\n", ptp - rt);
	printf("phc-tai delta:\t%lld\n", ptp - tai);

	close(fd_ptp);

	return EXIT_SUCCESS;
}
