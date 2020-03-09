#!/bin/sh
#
# Copyright (c) 2018, Intel Corporation
#
# SPDX-License-Identifier: BSD-3-Clause
#

IFACE=$1

if [ -z $IFACE ]; then
    echo "You must provide the network interface as first argument"
    exit -1
fi

# Now plus 1 minute
PLUS_1MIN=$((`date +%s%N` + 37000000000 + (60 * 1000000000)))

# Base will the next "round" timestamp ~1 min from now, plus 250us
BASE=$(($PLUS_1MIN - ( $PLUS_1MIN % 1000000000 ) + 250000))

sudo ./udp_tai -i $IFACE -b $BASE -P 1000000 -t 3 -p 90 -d 600000 -u 7788
