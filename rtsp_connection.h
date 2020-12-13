#ifndef RTSP_CONNECTION_H
#define RTSP_CONNECTION_H

#include <netinet/in.h>
#include <event2/bufferevent.h>

class RTSPConnection {
public:
    size_t currentCseq;
};

#endif
