#!/bin/bash
end=100
for i in $(seq 1 $end)
do
    nohup ffmpeg -rtsp_transport tcp -i "rtsp://127.0.0.1:8554/file/scripts/tbut$i.mp4?repeatedly=true" -c copy -f null - > /dev/null 2>&1 &
done
