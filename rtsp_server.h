#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <list>
#include <set>
#include <glog/logging.h>
#include <event2/bufferevent.h>
#include "CTPL/ctpl_stl.h"
#include "rtsp_parser.h"
#include "rtsp_connection.h"
#include "video_manager_service.h"

#define COMMAND_LENGTH 10

enum RTSPMethod {
    UNKNOWN = 0,
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

struct VideoRequest {
    bool _use_file;
    std::string filename;
    bool repeatedly;
    bool _use_gb;
    std::string gbid;
    StreamingMode gbStreamingMode;

    inline bool operator==(const VideoRequest& other) const {
        return (_use_file == other._use_file)
               && (filename == other.filename)
               && (_use_gb == other._use_gb)
               && (gbid == other.gbid);
    }

    inline bool valueAndParamsEqual(const VideoRequest& other) const {
        return (_use_file == other._use_file)
               && (filename == other.filename)
               && (repeatedly == other.repeatedly)
               && (_use_gb == other._use_gb)
               && (gbid == other.gbid)
               && (gbStreamingMode == other.gbStreamingMode);
    }
};

namespace std
{
    template <>
    struct hash<VideoRequest>
    {
        size_t operator()(const VideoRequest& r) const
        {
            // Compute individual hash values for first, second and third
            // http://stackoverflow.com/a/1646913/126995
            size_t res = 17;
            res = res * 31 + hash<bool>()(r._use_file);
            res = res * 31 + hash<string>()(r.filename);
            res = res * 31 + hash<bool>()(r._use_gb);
            res = res * 31 + hash<string>()(r.gbid);
            return res;
        }
    };
}

struct RTSPRequest {
    size_t cseq;
    RTSPMethod method;
    std::string url;
    std::string session;
    size_t streamid;
    StreamingMode streamingMode;
    uint16_t rtpPort;
    std::string lastHeader;

    std::string toString() const;
};

class RTSPServer {
public:
    RTSPServer(int p);
    virtual ~RTSPServer();

    void start();

private:
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

    void writeRtpData(std::string& url, uint8_t* data, size_t len);

    static void sendFrame(const std::set<struct bufferevent*>& bevs, const uint8_t* data, const size_t len);

private:
    std::map<struct bufferevent*, RTSPConnection> mBev2ConnectionMap;
    std::mutex mVideoSdpMutex;
    std::map<std::string, std::string> mUrl2SdpMap;
    std::map<std::string, std::list<struct bufferevent*>> mWaitingMetaMap; // 用list组织，因为只有顺序访问
    std::map<std::string, std::set<struct bufferevent*>> mPlayingVideoMap; // 用set组织，因为可能有查找删除等操作
    std::map<struct event*, std::string> mEvent2UrlMap;
    static std::map<struct event*, RTSPServer*> event2RTSPServerMap; // 析构时候要置为nullptr

    friend void accept_conn_cb(struct evconnlistener *listener,
                               evutil_socket_t sock,
                               struct sockaddr *addr,
                               int len,
                               void *ptr);
    friend void errorcb(struct bufferevent *bev, short error, void *ctx);
    friend void addVideoSourceCallback(const std::pair<const std::string*, const std::string*>& url2sdp, const void* client);
    friend void writeFrameInEventLoop(evutil_socket_t fd, short what, void *arg);
};

#endif
