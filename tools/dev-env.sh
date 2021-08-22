#!/bin/bash

set -e

cur_dir=$(dirname "$(readlink -f "$0")")

pushd $cur_dir
    echo "### build base image..."
    docker build --target base -t ffvms:base .
    echo "### build dev image..."
    docker build --target dev --build-arg TIMEZONE=Asia/Shanghai --build-arg USER --build-arg UID --build-arg GID --build-arg PW=12345678 -t ffvms:dev .
    pushd ..
        echo "### start dev environment..."
        docker run --privileged --cap-add=SYS_ADMIN -p 2222:22 -p 8554:8554 -v $HOME:/home/icefe -v $PWD:/ffvms -w /ffvms -d ffvms:dev /usr/sbin/sshd -D
    popd
popd
