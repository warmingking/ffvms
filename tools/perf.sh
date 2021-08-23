#!/bin/bash

set -e

perf_dir=$(mktemp -d -t ffvms.perf.XXXX)

pushd $perf_dir
    echo "default perf record 20 seconds data"
    pid=$(ps -ef | grep ffvms | grep config | awk '{print $2}')
    /usr/lib/linux-tools/5.4.0-70-generic/perf record -F 99 -p $pid --call-graph dwarf sleep 20
    /usr/lib/linux-tools/5.4.0-70-generic/perf script -i perf.data | /opt/FlameGraph/stackcollapse-perf.pl | /opt/FlameGraph/flamegraph.pl > perf.svg
    echo "please open $perf_dir/perf.svg in your browser"
popd
