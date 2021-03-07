FROM ubuntu

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

ARG CPPCORO_TAG=master
ARG CPPCORO_DIR=/var/local/git/cppcoro
RUN apt-get install -y python2.7 clang lld libc++-dev libc++abi-dev && \
    git clone -b ${CPPCORO_TAG} https://github.com/andreasbuhr/cppcoro.git ${CPPCORO_DIR} && \
    cd ${CPPCORO_DIR} && \
    git submodule update --init --recursive && \
    mkdir build && cd build && \
    export CXX=clang++ && \
    export CXXFLAGS="-stdlib=libc++ -march=native" && \
    export LDFLAGS="-stdlib=libc++ -fuse-ld=lld -Wl,--gdb-index" && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make && make install

ARG MEDIA_SERVER_TAG=master
ARG MEDIA_SERVER_DIR=/var/local/git/media-server
RUN git clone -b ${MEDIA_SERVER_TAG} https://github.com/ireader/sdk ${MEDIA_SERVER_DIR}/sdk && \
    git clone -b ${MEDIA_SERVER_TAG} https://github.com/ireader/media-server ${MEDIA_SERVER_DIR}/media-server && \
    cd ${MEDIA_SERVER_DIR}/media-server && \
    make clean && make RELEASE=1 && \
    mkdir /usr/local/include/meida_server && \
    for LIB in $(ls -d lib*); do cp -r $LIB/include /usr/local/include/meida_server/$LIB ; cp $LIB/release.linux/$LIB.a /usr/local/lib ; done && \
    cd ../sdk && make clean && make RELEASE=1 && \
    for LIB in $(ls -d lib*); do cp -r $LIB/include /usr/local/include/meida_server/$LIB ; cp $LIB/release.linux/$LIB.a /usr/local/lib ; done

RUN apt-get autoremove -y

# for convenience
RUN apt-get install -y sudo zsh zsh-antigen && apt-get autoremove -y

ARG USER=icefe
ARG UID=1000
ARG GID=1000
ARG PW=12345678

RUN useradd -m -G sudo -s /usr/bin/zsh ${USER} && echo "${USER}:${PW}" | chpasswd
USER ${UID}:${GID}
