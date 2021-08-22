#!/bin/bash
end=100
# for i in $(seq 1 $end)
# do
#     cp ../tbut.mp4 tbut$i.mp4
# done

for i in $(seq 1 $end)
do
    rm -rf tbut$i.mp4
done