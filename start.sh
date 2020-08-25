#!/bin/bash

base_path=$(readlink -f "$0")
root_dir=$(dirname "$base_path")

pushd $root_dir > /dev/null 2>&1
./ffvms -logtostderr 1
popd > /dev/null 2>&1
