#!/bin/bash

ulimit -c unlimited

cur_dir=$(dirname "$(readlink -f "$0")")

pushd $cur_dir/../build/core
    ./ffvms --config_file $cur_dir/config.json --alsologtostderr 1 $@
popd
