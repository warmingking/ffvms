FROM debian

# prepare dev env
RUN apt-get update && \
    apt-get install -y apt-utils \
    git \
    autoconf \
    automake \
    build-essential \
    g++ \
    cmake \
    libevent-dev \
    libgflags-dev \
    libgoogle-glog-dev \
    libgtest-dev \
    libavformat-dev \
    libssl-dev \
    zlib1g-dev && \
    apt-get -y autoremove

RUN git config --global http.proxy http://icefe-XPS-15-9560:7890
RUN git config --global https.proxy http://icefe-XPS-15-9560:7890
RUN git config --global url."https://github.com/".insteadOf git@github.com:

ARG GRPC_RELEASE_TAG=v1.35.0
ARG GRPC_DIR=/var/local/git/grpc
RUN git clone -b ${GRPC_RELEASE_TAG} https://github.com/grpc/grpc ${GRPC_DIR} && \
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
    make -j${nproc} && make install

ARG HTTP_PARSER_TAG=master
ARG HTTP_PARSER_DIR=/var/local/git/http-parser
RUN git clone -b ${HTTP_PARSER_TAG} https://github.com/ploxiln/http-parser ${HTTP_PARSER_DIR} && \
    cd ${HTTP_PARSER_DIR} && \
    make && make install

ARG CTPL_TAG=master
ARG CTPL_DIR=/var/local/git/ctpl
RUN git clone -b ${CTPL_TAG} https://github.com/vit-vit/CTPL ${CTPL_DIR} && \
    mkdir /usr/include/ctpl && \
    mv ${CTPL_DIR}/*.h /usr/include/ctpl

# for convenience
RUN apt-get install -y sudo zsh zsh-antigen && apt-get -y autoremove

ARG USER=icefe
ARG UID=1000
ARG GID=1000
ARG PW=12345678

RUN useradd -m -G sudo -s /usr/bin/zsh ${USER} && echo "${USER}:${PW}" | chpasswd
USER ${UID}:${GID}
