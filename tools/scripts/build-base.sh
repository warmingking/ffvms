#!/bin/bash

set -e

cur_dir=$(dirname $(readlink -f "$0"))

pushd $cur_dir/../docker > /dev/null
    docker build --target base -t qunxyz/ffvms:base .
popd > /dev/null