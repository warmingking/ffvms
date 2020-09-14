#ifndef RTSP_CONNECTION_H
#define RTSP_CONNECTION_H

#include <netinet/in.h>

class RTSPConnection {
public:
    struct sockaddr_in sin;
    size_t currentCseq;
};

#endif
