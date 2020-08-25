#ifndef RTSPSERVER_H
#define RTSPSERVER_H

#include <glog/logging.h>
#include <event2/bufferevent.h>

#define COMMAND_LENGTH 10

enum RTSPCommand {
    UNKNOWN = -1,
    OPTIONS,
    DESCRIBE,
    SETUP,
    PLAY,
    PAUSE,
    RECORD,
    ANNOUNCE,
    TEAEWORN,
    GET_PARAMETER,
    SET_PARAMETER,
    REDIRECT
};

class RTSPServer {
public:
    RTSPServer(int p);
    void start();

private:
    int mPort;

public:
    int parseCommandLine(const char* const line, RTSPCommand& command);
    void handleCmdOptions(struct evbuffer *);

private:

};

#endif
