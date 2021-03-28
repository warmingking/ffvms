#ifndef __RTSP_SERVEICE_H__
#define __RTSP_SERVEICE_H__

#include "common.h"
#include "rtp_producer.h"
#include <event2/bufferevent.h>
#include <functional>
#include <map>
#include <media_server/librtsp/rtsp-server.h>
#include <memory>
#include <set>
#include <shared_mutex>
#include <system_error>
#include <thread>

void accept_conn_cb(struct evconnlistener *listener, evutil_socket_t sock,
                    struct sockaddr *addr, int len, void *ptr);
void readcb(struct bufferevent *bev, void *ctx);
void errorcb(struct bufferevent *bev, short error, void *ctx);

int Close(void *ptr2);
int OnDescribe(void *ptr, rtsp_server_t *rtsp, const char *uri);
int OnOptions(void *ptr, rtsp_server_t *rtsp, const char *uri);
int OnPlay(void *ptr, rtsp_server_t *rtsp, const char *uri, const char *session,
           const int64_t *npt, const double *scale);

namespace ffvms
{
namespace core
{

/** RtspService
 * 暂时只支持视频流, 每个 client 只有一个 session
 */
class RtspService
{
public:
    struct Config
    {
        int rtsp_port;
        int event_thread_num;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Config, rtsp_port, event_thread_num)
    };

public:
    RtspService();
    virtual ~RtspService();

    std::error_code Init(Config config, std::shared_ptr<RtpProducer> rtpProducer);
    void Dispatch();

    struct EventRtspServer
    {
        EventRtspServer() = default;
        virtual ~EventRtspServer()
        {
            bufferevent_free(bev);
            rtsp_server_destroy(server);
        }

        rtsp_server_t *server;
        struct bufferevent *bev;
        RtspService *owner;
    };

    struct ProxyInfo
    {
        std::shared_mutex mClientMutex;
        std::set<EventRtspServer *> beforeDescribeClients;
        std::set<EventRtspServer *> waitingSdpClients;
        std::set<EventRtspServer *> beforePlayClients;
        std::set<EventRtspServer *> playingClients;
        std::atomic_bool registered; // default false
        std::string sourceSdp;       // default empty string
        std::unique_ptr<char, std::function<void(char *)>>
            sendBuffer; // default size: mtu
    };

private:
    // 取锁顺序: mRtspServerMutex > mProxyInfoMutex > ProxyInfo::mClientMutex

    std::vector<std::unique_ptr<std::thread>> mEventLoopThreads;
    std::vector<std::unique_ptr<struct event_base,
                                std::function<void(struct event_base *)>>>
        mEventBases;
    std::vector<std::unique_ptr<struct evconnlistener,
                                std::function<void(struct evconnlistener *)>>>
        mEventListeners;

    std::unique_ptr<rtsp_handler_t> mpRtspHandler;
    std::shared_ptr<RtpProducer> mpRtpProducer;

    // 每个 bev 对应一个 rtsp server
    std::shared_mutex mRtspServerMutex;
    std::map<struct bufferevent *, std::unique_ptr<EventRtspServer>>
        mBev2RtspServer;

    // 每个 video request 可能对应多个 bev ( client )
    std::mutex mProxyInfoMutex;
    std::map<VideoRequest, std::unique_ptr<ProxyInfo>> mVideoRequest2ProxyInfo;

    friend void ::accept_conn_cb(struct evconnlistener *listener,
                                 evutil_socket_t sock, struct sockaddr *addr,
                                 int len, void *ptr);
    friend void ::readcb(struct bufferevent *bev, void *ctx);
    friend void ::errorcb(struct bufferevent *bev, short error, void *ctx);

    friend int ::Close(void *ptr2);
    friend int ::OnDescribe(void *ptr, rtsp_server_t *rtsp, const char *uri);
    friend int ::OnOptions(void *ptr, rtsp_server_t *rtsp, const char *uri);
    friend int ::OnPlay(void *ptr, rtsp_server_t *rtsp, const char *uri,
                        const char *session, const int64_t *npt,
                        const double *scale);
};
} // namespace core
} // namespace ffvms

#endif // __RTSP_SERVEICE_H__