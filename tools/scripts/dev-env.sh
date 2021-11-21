#!/bin/bash

set -ex

cur_dir=$(dirname "$(readlink -f "$0")")

pushd $cur_dir > /dev/null
    echo "### update base image"
    docker pull qunxyz/ffvms:base
    docker tag qunxyz/ffvms:base ffvms:base
    echo "### build dev image"
    docker build --target dev --build-arg TIMEZONE=Asia/Shanghai --build-arg USER=$USER --build-arg UID=$UID --build-arg GID=$GID --build-arg PW=12345678 -t ffvms:dev -f ../docker/Dockerfile .
    echo "### start dev env"
    docker run --privileged --cap-add=SYS_ADMIN -p 2222:22 -p 8554:8554 -v $HOME:/home/$USER -v $PWD/../../:/ffvms -w /ffvms -d ffvms:dev /usr/sbin/sshd -D
popd > /dev/null