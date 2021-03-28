#!/usr/bin/bash

video_file=/workspaces/ffvms/tbut.mp4
# video_file=/workspaces/ffvms/sample.mp4
local_port=$1

trap recycle exit

function recycle() {
    if [[ $(ps -ef | grep ffmpeg | grep -c "localport=${local_port}") -gt 0 ]]; then
        ps -ef | grep ffmpeg \
            | grep "localport=${local_port}" \
            | awk '{print $2}' \
            | xargs kill
    fi
}

ffmpeg \
    -re \
    -stream_loop -1 \
    -i ${video_file} \
    -c copy -an \
    -f rtp_mpegts \
    -sdp_file /workspaces/ffvms/mp2p.sdp \
    "rtp://localhost:8086?localport=${local_port}" \
    > /dev/null 2>&1
