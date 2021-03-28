#!/bin/bash
end=20
for i in $(seq 1 $end)
do
    # nohup ffmpeg -rtsp_transport tcp -i "rtsp://127.0.0.1:8554/file//workspaces/ffvms/tools/tbut$i.mp4?repeatedly=true" -c copy -f null - > /dev/null 2>&1 &
    nohup ffmpeg -rtsp_transport tcp -i "rtsp://127.0.0.1:8554/gb/demo$i" -c copy -f null - > /dev/null 2>&1 &
done
