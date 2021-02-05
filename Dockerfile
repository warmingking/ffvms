FROM ubuntu
RUN apt-get update && \
    apt-get install apt-utils -y && \
    apt-get install libgflags-dev -y && \
    apt-get install libgoogle-glog-dev -y && \
    apt-get install libavformat-dev -y && \
    apt-get install cmake -y && \
    apt-get install git -y
