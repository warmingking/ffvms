#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <list>
#include <set>
#include <shared_mutex>
#include <glog/logging.h>
#include <event2/bufferevent.h>
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
    class VideoObject {
    public:
        std::shared_mutex mMutex;
        std::string sdp;
        bool sdpReady;
        std::map<struct bufferevent*, RTSPConnection> mBev2ConnectionMap;
        uint8_t rtpHeader[4];
        struct event* pPlayingEvent;

        VideoObject();
        // virtual ~VideoObject();
    };

public:
    static RTSPServer& getInstance();
    virtual ~RTSPServer();
    void start(int port);

private:
    RTSPServer();
    RTSPServer(const RTSPServer&);
    RTSPServer& operator=(const RTSPServer&);

    int mPort;
    VideoManagerService* mpVideoManagerService;

public:
    void processOptionCommand(struct bufferevent* bev, const BaseCommand&);
    void processDescribeCommand(struct bufferevent* bev, const BaseCommand&);
    void processSetupCommand(struct bufferevent* bev, const BaseCommand&);
    void processPlayCommand(struct bufferevent* bev, const BaseCommand&);

    void writeRtpData(const VideoRequest& url, uint8_t* data, size_t len);
    void streamComplete(const VideoRequest& request);

    static void sendFrame(const std::set<struct bufferevent*>& bevs, const uint8_t* data, const size_t len);

private:
    ctpl::thread_pool* mpIOThreadPool;
    std::atomic<unsigned int> mSessionId;

    void startIOLoop(struct sockaddr_in& sin);
    std::string getSessionId();
    void sendRtspError(struct bufferevent* bev, const int currentCseq, const int error);
    void sendDescribeSdp(struct bufferevent* bev, const int currentCseq, const std::string& sdp);
    void teardown(const VideoRequest& url);

    std::shared_mutex mMutex;
    std::map<VideoRequest, VideoObject*> mProcessingVideoMap;

    friend void accept_conn_cb(struct evconnlistener *listener,
                               evutil_socket_t sock,
                               struct sockaddr *addr,
                               int len,
                               void *ptr);
    friend void errorcb(struct bufferevent *bev, short error, void *ctx);
    friend void writeFrameInEventLoop(evutil_socket_t fd, short what, void *arg);
};

#endif
