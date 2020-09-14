#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <list>
#include <glog/logging.h>
#include <event2/bufferevent.h>
#include "CTPL/ctpl_stl.h"
#include "rtsp_parser.h"
#include "rtsp_connection.h"
#include "video_manager_service.h"

#define COMMAND_LENGTH 10

class RTSPServer {
public:
    RTSPServer(int p);
    virtual ~RTSPServer();

    void start();

private:
    int mPort;
    VideoManagerService* mpVideoManagerService;
    ctpl::thread_pool* mpProbeVideoThreadPool;

public:
    std::map<struct bufferevent*, RTSPConnection> mBev2ConnectionMap;

    void processOptionCommand(struct bufferevent*, const BaseCommand&);
    void processDescribeCommand(struct bufferevent*, const BaseCommand&);
    void processSetupCommand(struct bufferevent*, const BaseCommand&);
    void processPlayCommand(struct bufferevent*, const BaseCommand&);
    void sendDescribeSdp(struct bufferevent*, const VideoMeta& meta);

private:
    static std::mutex videoMetaMutex;
    static std::map<std::string, VideoMeta> videoMetaMap;
    static std::map<std::string, std::list<struct bufferevent*>> waitingMetaMap;

    static void addVideoSourceCallback(const VideoMeta& meta, const void* client);
};

#endif
