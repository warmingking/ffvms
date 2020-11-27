#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <list>
#include <set>
#include <unordered_map>
#include <glog/logging.h>
#include <event2/bufferevent.h>
#include "CTPL/ctpl_stl.h"
#include "rtsp_parser.h"
#include "rtsp_connection.h"
#include "video_manager_service.h"

#define COMMAND_LENGTH 10

struct RTSPRequest {
    size_t cseq;
    RTSPCommand method;
    std::string url;
    std::string session;
    size_t streamid;
    StreamingMode streamingMode;
    uint16_t rtpPort;
    std::string lastHeader;

    std::string toString() const;
};

class RTSPServer : public VideoSink {
public:
    static RTSPServer& getInstance();

    virtual ~RTSPServer();

    void start(int port);

private:
    RTSPServer();
    RTSPServer(const RTSPServer&);
    RTSPServer& operator=(const RTSPServer&);

    int mPort;
    struct event_base* mpBase;
    VideoManagerService* mpVideoManagerService;
    ctpl::thread_pool* mpProbeVideoThreadPool;
    ctpl::thread_pool* mpGetFrameThreadPool;

public:
    void processOptionCommand(struct bufferevent* bev, const BaseCommand&);
    void processDescribeCommand(struct bufferevent* bev, const BaseCommand&);
    void processSetupCommand(struct bufferevent* bev, const BaseCommand&);
    void processPlayCommand(struct bufferevent* bev, const BaseCommand&);
    void sendDescribeSdp(struct bufferevent* bev, const std::string& sdp);

    void writeRtpData(const VideoRequest& url, uint8_t* data, size_t len);

    static void sendFrame(const std::set<struct bufferevent*>& bevs, const uint8_t* data, const size_t len);

private:
    std::atomic<unsigned int> mSessionId;
    std::string getSessionId();
    std::map<struct bufferevent*, RTSPConnection> mBev2ConnectionMap;
    std::mutex mVideoSdpMutex;
    std::unordered_map<VideoRequest, std::string> mVideoRequest2SdpMap;
    std::unordered_map<VideoRequest, std::list<bufferevent*>> mWaitingMetaMap; // 用list组织，因为只有顺序访问
    std::unordered_map<VideoRequest, std::pair<uint8_t*, std::set<bufferevent*>>> mPlayingVideoMap; // value是一个pair，key是buffer，value是bevs，bev用set组织，因为可能有查找删除等操作,
    std::unordered_map<VideoRequest, struct event*> mUrl2EventMap;

    friend void accept_conn_cb(struct evconnlistener *listener,
                               evutil_socket_t sock,
                               struct sockaddr *addr,
                               int len,
                               void *ptr);
    friend void errorcb(struct bufferevent *bev, short error, void *ctx);
    friend void writeFrameInEventLoop(evutil_socket_t fd, short what, void *arg);
};

#endif
