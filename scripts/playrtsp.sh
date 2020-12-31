#!/bin/bash
end=100
for i in $(seq 1 $end)
do
    nohup ffmpeg -rtsp_transport tcp -i "rtsp://172.29.237.196:8554/file/tbut$i.mp4?repeatedly=true" -c copy -f null - > /dev/null 2>&1 &
done
