# base image
FROM ubuntu:focal as base

# set noninteractive
ARG DEBIAN_FRONTEND=noninteractive

# prepare dev env
RUN apt-get update && \
    apt-get install -y apt-utils \
    git \
    autoconf \
    automake \
    build-essential \
    cmake \
    libboost-all-dev \
    libavformat-dev \
    libgflags-dev \
    libgoogle-glog-dev \
    libgtest-dev

RUN git config --global url."https://github.com/".insteadOf git@github.com:

ARG GRPC_TAG=v1.35.0
ARG GRPC_DIR=/var/local/git/grpc
RUN apt-get install -y libssl-dev zlib1g-dev && \
    git clone -b ${GRPC_TAG} https://github.com/grpc/grpc ${GRPC_DIR} && \
    cd ${GRPC_DIR} && \
    git submodule update --init && \
    mkdir -p cmake/build && \
    cd cmake/build && \
    cmake ../.. -DgRPC_INSTALL=ON \
                -DCMAKE_BUILD_TYPE=Release \
                -DgRPC_INSTALL=ON \
                -DgRPC_BUILD_TESTS=OFF \
                -DgRPC_CARES_PROVIDER=module \
                -DgRPC_ABSL_PROVIDER=module \
                -DgRPC_PROTOBUF_PROVIDER=module \
                -DgRPC_RE2_PROVIDER=module \
                -DgRPC_SSL_PROVIDER=package \
                -DgRPC_ZLIB_PROVIDER=package && \
    make -j4 && make install

ARG HTTP_PARSER_TAG=master
ARG HTTP_PARSER_DIR=/var/local/git/http-parser
RUN git clone -b ${HTTP_PARSER_TAG} https://github.com/ploxiln/http-parser ${HTTP_PARSER_DIR} && \
    cd ${HTTP_PARSER_DIR} && \
    make && make install

ARG FMT_TAG=master
ARG FMT_DIR=/var/local/git/fmt
RUN git clone -b ${FMT_TAG} https://github.com/fmtlib/fmt.git ${FMT_DIR} && \
    cd ${FMT_DIR} && \
    mkdir build && cd build && cmake .. -DFMT_DOC=OFF -DFMT_TEST=OFF && \
    make -j4 && make install

ARG JSON_TAG=v3.9.1
ARG JSON_DIR=/var/local/git/json
RUN git clone -b ${JSON_TAG} https://github.com/nlohmann/json.git ${JSON_DIR} && \
    cd ${JSON_DIR} && \
    mkdir build && cd build && \
    cmake .. && make -j4 && make install

ARG GPERFTOOLS_TAG=gperftools-2.9.1
ARG GPERFTOOLS_DIR=/var/local/git/gperftools
RUN git clone -b ${GPERFTOOLS_TAG} https://github.com/gperftools/gperftools.git ${GPERFTOOLS_DIR} && \
    cd ${GPERFTOOLS_DIR} && \
    ./autogen.sh && ./configure --prefix=/usr/local --disable-cpu-profiler --disable-heap-profiler --disable-heap-checker --disable-debugalloc && \
    make -j4 && make install

ARG PROMETHEUS_TAG=v0.12.3
ARG PROMETHEUS_DIR=/var/local/git/prometheus
RUN git clone -b ${PROMETHEUS_TAG} https://github.com/jupp0r/prometheus-cpp.git ${PROMETHEUS_DIR} && \
    cd ${PROMETHEUS_DIR} && \
    git submodule update --init && \
    mkdir build && cd build && \
    # without push-gateway integration
    cmake .. -DENABLE_PUSH=OFF && make -j4 && make install

RUN apt-get autoremove -y

# dev image
FROM base AS dev

ARG TIMEZONE
ENV TZ=$TIMEZONE
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN apt-get update && \
    apt-get install -y openssh-server sudo gdb lsof net-tools netcat zsh zsh-antigen ffmpeg linux-tools-5.4.0-70-generic wget unzip tmux && \
    apt-get autoremove -y

RUN wget -q https://github.com/brendangregg/FlameGraph/archive/master.zip && \
    unzip master.zip && \
    mv FlameGraph-master/ /opt/FlameGraph

RUN mkdir /run/sshd

ENV PATH="/opt/FlameGraph:${PATH}"

ARG USER
ARG UID
ARG GID
ARG PW

RUN useradd -m -G sudo -s /usr/bin/zsh ${USER} && echo "${USER}:${PW}" | chpasswd

# release image
FROM base AS release
RUN rm -rf /var/local/git/
