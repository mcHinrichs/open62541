#!/bin/bash
#
# Copyright (c) 2018, Intel Corporation
#
# SPDX-License-Identifier: BSD-3-Clause
#

#
# DISCLAIMER
#
# This script is meant for testing purposes only.
# It provides an oversimplified approach for having a simple PTP
# network up and running, with each local node having its CLOCK_TAI
# offset adjusted.
#
# TODO:
#	- 	find a way to fetch the TAI offset from ptp4l directly. Ivan
#		suggested using pmc for that.
#

set -e

INTERFACE=none
TAI_OFFSET=37
PTP4L_VERBOSE=''
PHC2SYS_VERBOSE=''
if [ -z $PTP4L ]; then
	PTP4L=$(which ptp4l)
fi
if [ -z $PHC2SYS ]; then
	PHC2SYS=$(which phc2sys)
fi


# On the PTP master, if started with -M parameter, synchronize the
# system clock to PHC first, then propagate that to network using ptp4l.
# We trust that the system clock was initially setup correctly or adjusted
# to some other source (i.e. NTP, GPS, etc).
#
# For this -M mode, clocks are kept synchronized by phc2sys.
# This is provided for the scenarios in which the PTP master on this network
# is also running one end of the TSN application (either the listener or the
# talker), which requires the local clocks to be synchronized.
#
# When that isn't the case (i.e. the etf experiment, in which all we care
# about is the network clock sync), then just start this script with -m
# instead so phc2sys is not used and the jitter of the network clock sync is
# not affected.
#
setup_ptp_master() {
	ptp4l -2 -i $INTERFACE $PTP4L_VERBOSE &
}

setup_ptp_master_and_sync() {
	phc2sys -c $INTERFACE -s CLOCK_REALTIME -w $PHC2SYS_VERBOSE &
	setup_ptp_master
}


# On PTP slaves, first synchronize the PHC to the PTP master,
# then synchronize the system clock to the PHC.
setup_ptp_slave() {
	phc2sys -a -r $PHC2SYS_VERBOSE &
	ptp4l -2 -s -i $INTERFACE $PTP4L_VERBOSE &
}


# Use adjtimex to set the TAI offset to CLOCK_TAI.
adjust_clock_tai_offset() {
	tmp_src=$(mktemp /tmp/XXXXXX.c)
	tmp_bin=$(mktemp)
	cat <<EOF > $tmp_src
#include <stdio.h>
#include <stdlib.h>
#include <sys/timex.h>

int main(void)
{
	struct timex timex = {
		.modes		= ADJ_TAI,
		.constant	= $TAI_OFFSET
	};

	if (adjtimex(&timex) == -1) {
		perror("adjtimex failed to set CLOCK_TAI offset");
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
EOF

	gcc -o $tmp_bin $tmp_src
	$tmp_bin
	rm -f $tmp_bin $tmp_src
}


test_dependencies() {
	if [ ! -x $PTP4L ]; then
		echo "ptp4l must be available from your \$PATH or set \$PTP4L."
		exit -1
	fi
	if [ ! -x $PHC2SYS ]; then
		echo "phc2sys must be available from your \$PATH or set \$PHC2SYS."
		exit -1
	fi
}


test_dependencies

ptp_master_mode=f
while getopts "Mmsvi:" opt; do
	case ${opt} in
	i)	INTERFACE=$OPTARG ;;
	m)	ptp_master_mode=y ;;
	s)	ptp_master_mode=n ;;
	M)	ptp_master_mode=M ;;
	v)	PTP4L_VERBOSE='-m --summary_interval=5' ;
		PHC2SYS_VERBOSE='-m -u 20' ;;
	*)	exit -1 ;;
	esac
done

if [ ${INTERFACE} = none ]; then
	echo "You must set the network interface using '-i'."
	exit -1
fi

if [ ${ptp_master_mode} = y ]; then
	setup_ptp_master
	adjust_clock_tai_offset
elif [ ${ptp_master_mode} = M ]; then
	setup_ptp_master_and_sync
	adjust_clock_tai_offset
elif [ ${ptp_master_mode} = n ]; then
	setup_ptp_slave
	adjust_clock_tai_offset
else
	echo "You must select PTP master (-m) OR PTP slave (-s) mode."
	exit -1
fi

