FROM ubuntu as base

# set
ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Shanghai
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

# prepare dev env
RUN apt-get update && \
    apt-get install -y apt-utils \
    git \
    autoconf \
    automake \
    build-essential \
    g++ \
    cmake \
    libboost-all-dev \
    libevent-dev \
    libavformat-dev \
    libgflags-dev \
    libgoogle-glog-dev \
    libgtest-dev

RUN git config --global http.proxy http://172.17.0.1:7890
RUN git config --global https.proxy http://172.17.0.1:7890
RUN git config --global url."https://github.com/".insteadOf git@github.com:

SHELL ["/bin/bash", "-c"]

ARG GRPC_RELEASE_TAG=v1.35.0
ARG GRPC_DIR=/var/local/git/grpc
RUN apt-get install -y libssl-dev zlib1g-dev && \
    git clone -b ${GRPC_RELEASE_TAG} https://github.com/grpc/grpc ${GRPC_DIR} && \
    cd ${GRPC_DIR} && \
    git submodule update --init --recursive && \
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

ARG CTPL_TAG=master
ARG CTPL_DIR=/var/local/git/ctpl
RUN git clone -b ${CTPL_TAG} https://github.com/vit-vit/CTPL ${CTPL_DIR} && \
    mkdir /usr/include/ctpl && \
    mkdir /usr/local/include/ctpl && \
    cp ${CTPL_DIR}/*.h /usr/local/include/ctpl

ARG MEDIA_SERVER_TAG=master
ARG MEDIA_SERVER_DIR=/var/local/git/media-server
ARG MEDIA_SERVER_INSTALL_DIR=/usr/local/include/media_server
RUN apt-get install -y bc && \
    git clone -b ${MEDIA_SERVER_TAG} https://github.com/ireader/sdk ${MEDIA_SERVER_DIR}/sdk && \
    git clone -b ${MEDIA_SERVER_TAG} https://github.com/ireader/avcodec ${MEDIA_SERVER_DIR}/avcodec && \
    git clone -b ${MEDIA_SERVER_TAG} https://github.com/ireader/media-server ${MEDIA_SERVER_DIR}/media-server && \
    mkdir ${MEDIA_SERVER_INSTALL_DIR} && \
    cd ${MEDIA_SERVER_DIR}/avcodec && make clean && make -j4 RELEASE=1 && \
    for LIB in $(ls -d */ | awk -F '/' '{print $1}'); do cp -r $LIB/include ${MEDIA_SERVER_INSTALL_DIR}/$LIB ; cp $LIB/release.linux/$LIB.a /usr/local/lib | true ; done && \
    cd ${MEDIA_SERVER_DIR}/media-server && \
    make clean && make -j4 RELEASE=1 && \
    for LIB in $(ls -d lib*); do cp -r $LIB/include ${MEDIA_SERVER_INSTALL_DIR}/$LIB ; cp $LIB/release.linux/$LIB.a /usr/local/lib ; done && \
    cd ../sdk && make clean && make -j4 RELEASE=1 && cp -r include/* ${MEDIA_SERVER_INSTALL_DIR} && \
    for LIB in $(ls -d lib*); do cp -r $LIB/include ${MEDIA_SERVER_INSTALL_DIR}/$LIB ; cp $LIB/release.linux/$LIB.a /usr/local/lib ; done

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

ARG LIBPCAP_TAG=libpcap-1.10
ARG LIBPCAP_DIR=/var/local/git/libpcap
RUN apt-get install -y flex bison libnl-genl-3-dev && \
    git clone -b ${LIBPCAP_TAG} https://github.com/the-tcpdump-group/libpcap ${LIBPCAP_DIR} && \
    cd ${LIBPCAP_DIR} && \
    mkdir build && cd build && \
    cmake .. && make -j4 && make install

RUN apt-get autoremove -y

# for convenience
FROM base AS dev
ENV HTTP_PROXY="http://127.0.0.1:7890"
ENV HTTPS_PROXY="http://127.0.0.1:7890"

RUN apt-get update && \
    apt-get install -y sudo gdb lsof net-tools netcat zsh zsh-antigen ffmpeg linux-tools-5.4.0-70-generic wget unzip && \
    apt-get autoremove -y

RUN wget -q https://github.com/brendangregg/FlameGraph/archive/master.zip && \
    unzip master.zip && \
    mv FlameGraph-master/ /opt/FlameGraph

ENV PATH="/opt/FlameGraph:${PATH}"

ARG USER=icefe
ARG UID=1000
ARG GID=1000
ARG PW=12345678

RUN useradd -m -G sudo -s /usr/bin/zsh ${USER} && echo "${USER}:${PW}" | chpasswd
USER ${UID}:${GID}
