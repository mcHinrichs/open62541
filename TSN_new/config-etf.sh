#!/bin/bash
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

BATCH_FILE=etf.batch

cat > $BATCH_FILE <<EOF
qdisc replace dev $IFACE parent root handle 100 mqprio \\
      num_tc 3 \\
      map 2 2 1 0 2 2 2 2 2 2 2 2 2 2 2 2 \\
      queues 1@0 1@1 2@2 \\
      hw 0

qdisc replace dev enp3s0 parent 100:1 etf \\
      offload delta 300000 clockid CLOCK_TAI

qdisc replace dev enp3s0 parent 100:2 etf clockid CLOCK_TAI \\
      delta 300000 offload deadline_mode
EOF

tc -batch $BATCH_FILE
