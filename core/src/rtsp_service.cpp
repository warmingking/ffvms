#include "rtsp_service.h"
#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/thread.h>
#include <fcntl.h>
#include <fmt/core.h>
#include <media_server/path.h>
#include <media_server/uri-parse.h>
#include <media_server/urlcodec.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <sys/socket.h>

using namespace ffvms::core;
using namespace std::placeholders;

// using event_base = struct event_base;
// using evconnlistener = struct evconnlistener;
// using sockaddr = struct sockaddr;
// using sockaddr_in = struct sockaddr_in;
// using bufferevent = struct bufferevent;
#define MAX_LINE 16384

void readcb(bufferevent *bev, void *ctx)
{
    RtspService *pService = (RtspService *)ctx;
    std::shared_lock _(pService->mRtspServerMutex);
    auto it = pService->mBev2RtspServer.find(bev);
    if (it == pService->mBev2RtspServer.end())
    {
        LOG(ERROR) << "got data from unkown bev " << bev;
        return;
    }
    evbuffer *input = bufferevent_get_input(bev);
    char line[MAX_LINE];

    size_t read = evbuffer_remove(bufferevent_get_input(bev), &line, MAX_LINE);
    if (read == -1)
    {
        LOG(ERROR) << "drain the message from bev " << bev;
        return;
    }

    int ret = rtsp_server_input(it->second->server, line, &read);
}

void errorcb(bufferevent *bev, short error, void *ctx)
{
    RtspService *pService = (RtspService *)ctx;
    std::unique_lock _(pService->mRtspServerMutex);
    auto it = pService->mBev2RtspServer.find(bev);
    if (it == pService->mBev2RtspServer.end())
    {
        LOG(ERROR) << "got error from unkown bev " << bev;
        return;
    }

    LOG(INFO) << "unestablish connection with bev " << bev;
    pService->mBev2RtspServer.erase(it);
}

void accept_conn_cb(evconnlistener *listener, evutil_socket_t sock,
                    sockaddr *addr, int len, void *ptr)
{
    // 设置 TCP_NODELAY
    int tcpNodelay = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &tcpNodelay,
                   sizeof(tcpNodelay)) != 0)
    {
        close(sock);
        LOG(FATAL) << "set TCP_NODELAY for failed, error: " << strerror(errno);
    }

    RtspService *pService = (RtspService *)ptr;
    sockaddr_in *sin = (sockaddr_in *)addr;
    char *ip = inet_ntoa(sin->sin_addr);
    int port = htons(sin->sin_port);
    event_base *base = evconnlistener_get_base(listener);
    auto *bev = bufferevent_socket_new(
        base, sock,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS |
            BEV_OPT_UNLOCK_CALLBACKS);
    if (bev == NULL)
    {
        LOG(ERROR) << "failed to create socket for " << ip << ":" << port;
        return;
    }
    LOG(INFO) << "establish connection with host " << ip << ":" << port
              << ", bev: " << bev;

    std::unique_lock _(pService->mRtspServerMutex);
    if (pService->mBev2RtspServer.count(bev) != 0)
    {
        LOG(ERROR) << "bev " << bev << " already established";
        return;
    }
    auto pServer = std::make_unique<RtspService::EventRtspServer>();
    pServer->server = rtsp_server_create(
        ip, port, pService->mpRtspHandler.get(), pServer.get(), pServer.get());
    pServer->bev = bev;
    pServer->owner = pService;
    pService->mBev2RtspServer[bev] = std::move(pServer);
    bufferevent_set_max_single_write(bev,
                                     1400); // 不大于mtu, 确保数据可以即时发送走
    bufferevent_setcb(bev, readcb, NULL, errorcb, pService);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

auto processSdpFunction = [](RtspService::ProxyInfo *proxy, std::string &&sdp) {
    std::unique_lock _(proxy->mClientMutex);
    proxy->registered = true;
    for (const auto &es : proxy->waitingSdpClients)
    {
        rtsp_server_reply_describe(es->server, 200, sdp.c_str());
        proxy->beforePlayClients.insert(es);
    }
    proxy->waitingSdpClients.clear();
    proxy->sourceSdp = std::move(sdp);
};

auto consumePktFunction = [](RtspService::ProxyInfo *proxy, const char *data,
                             const size_t len) {
    std::shared_lock _(proxy->mClientMutex);
    // 默认以 TCP 的方式
    // rfc2326[10.12] <https://tools.ietf.org/html/rfc2326#section-10.12>
    int interleaved(0);
    int payload = data[1] & 0x7f;
    if (payload >= 72 && payload <= 76)
    { // RTCP
        interleaved = 1;
    }

    *(proxy->sendBuffer.get()) = '$';
    *(proxy->sendBuffer.get() + 1) = interleaved;
    *(proxy->sendBuffer.get() + 2) = len >> 8;
    *(proxy->sendBuffer.get() + 3) = len & 0xff;
    std::copy(data, &data[len], proxy->sendBuffer.get() + 4);
    for (const auto &es : proxy->playingClients)
    {
        rtsp_server_send_interleaved_data(es->server, proxy->sendBuffer.get(),
                                          len + 4);
    }
};

auto processErrorFunction = [](RtspService::ProxyInfo *proxy,
                               std::error_code &&ec) {
    // 如果出错, 只需要将这一路上所有的 socket 都关闭即可, 关闭 socket
    // 最终会调用 rtsp_server_t 的 close, 有完整的回收清理
    LOG(INFO) << "proxy error " << ec << " will be closed";
    for (const auto &client : proxy->beforeDescribeClients)
    {
        evutil_closesocket(bufferevent_getfd(client->bev));
    }
    for (const auto &client : proxy->waitingSdpClients)
    {
        evutil_closesocket(bufferevent_getfd(client->bev));
    }
    for (const auto &client : proxy->beforePlayClients)
    {
        evutil_closesocket(bufferevent_getfd(client->bev));
    }
    for (const auto &client : proxy->playingClients)
    {
        evutil_closesocket(bufferevent_getfd(client->bev));
    }
};

// 这些回调函数都不需要加 mRtspServerMutex 锁, 因为外层已经拿锁了
int Close(void *ptr2)
{
    VLOG(1) << "close " << ptr2;
    RtspService::EventRtspServer *pServer =
        (RtspService::EventRtspServer *)ptr2;
    RtspService *pService = pServer->owner;
    bool found(false), needUnregistered(false);
    // 先找到这个 client
    std::unique_lock _(pService->mProxyInfoMutex);
    for (const auto &video2ProxyInfo : pService->mVideoRequest2ProxyInfo)
    {
        if (video2ProxyInfo.second->beforeDescribeClients.count(pServer) != 0)
        {
            found = true;
            video2ProxyInfo.second->beforeDescribeClients.erase(pServer);
        }
        else if (video2ProxyInfo.second->waitingSdpClients.count(pServer) != 0)
        {
            found = true;
            video2ProxyInfo.second->waitingSdpClients.erase(pServer);
        }
        else if (video2ProxyInfo.second->beforePlayClients.count(pServer) != 0)
        {
            found = true;
            video2ProxyInfo.second->beforePlayClients.erase(pServer);
        }
        else if (video2ProxyInfo.second->playingClients.count(pServer) != 0)
        {
            found = true;
            video2ProxyInfo.second->playingClients.erase(pServer);
        }
        if (found)
        {
            // 只有这个 video 上的所有 client 都结束, 才需要停止这一路
            needUnregistered =
                (video2ProxyInfo.second->waitingSdpClients.empty() &&
                 video2ProxyInfo.second->beforePlayClients.empty() &&
                 video2ProxyInfo.second->playingClients.empty());
            if (needUnregistered)
            {
                // FIXME: delay unregister
                if (video2ProxyInfo.second->registered)
                {
                    pService->mpRtpProducer->UnregisterVideo(
                        video2ProxyInfo.first);
                }
                pService->mVideoRequest2ProxyInfo.erase(video2ProxyInfo.first);
            }
            break;
        }
    }
    return 0;
}

int Send(void *ptr2, const void *data, size_t bytes)
{
    evbuffer_add(
        bufferevent_get_output(((RtspService::EventRtspServer *)ptr2)->bev),
        data, bytes);
    return 0;
}

int OnDescribe(void *ptr, rtsp_server_t *rtsp, const char *uri)
{
    VLOG(1) << "ondescribe:\n" << uri;
    RtspService::EventRtspServer *pServer = (RtspService::EventRtspServer *)ptr;
    VideoRequest video(uri);
    std::scoped_lock _(pServer->owner->mProxyInfoMutex);
    auto it = pServer->owner->mVideoRequest2ProxyInfo.find(video);
    if (it == pServer->owner->mVideoRequest2ProxyInfo.end())
    {
        LOG(WARNING) << "video " << video
                     << " not found, may some error when register or process";
        return -1; // TODO: error code ( 一般是由于取流报错, 这一路被删除了 )
    }
    std::unique_lock clientsLock(it->second->mClientMutex);
    it->second->beforeDescribeClients.erase(pServer);
    // 构造 callback
    auto processSdpFunc = std::bind(processSdpFunction, it->second.get(), _1);
    auto consumePktFunc =
        std::bind(consumePktFunction, it->second.get(), _1, _2);
    auto processErrorFunc =
        std::bind(processErrorFunction, it->second.get(), _1);
    if (it->second->sourceSdp.empty() && it->second->waitingSdpClients.empty())
    {
        // 既没有获取到 sdp, 也没有正在等待返回
        // 将 bev 设置为 waiting sdp
        it->second->waitingSdpClients.insert(pServer);
        // register 这一路 video
        pServer->owner->mpRtpProducer->RegisterVideo(
            video, processSdpFunc, consumePktFunc, processErrorFunc);
    }
    else if (!it->second->sourceSdp.empty())
    {
        // 已经获取到 sdp
        // 直接返回 sdp
        rtsp_server_reply_describe(pServer->server, 200,
                                   it->second->sourceSdp.c_str());
        // 状态为等待 play 请求, 注意必须在 replay 之后
        it->second->beforePlayClients.insert(pServer);
    }
    return 0;
}

int OnSetup(void *ptr, rtsp_server_t *rtsp, const char *uri,
            const char *session,
            const struct rtsp_header_transport_t transports[], size_t num)
{
    VLOG(1) << "setup:\n" << uri;
    std::string transportStr;
    if (transports->transport == RTSP_TRANSPORT_RTP_TCP)
    {
        transportStr =
            fmt::format("RTP/AVP/TCP;interleaved={}-{}",
                        transports->interleaved1, transports->interleaved2);
    }
    else
    {
        // 461 Unsupported Transport
        return rtsp_server_reply_setup(rtsp, 461, NULL, NULL);
    }
    if (session == NULL)
    {
        std::string sessionStr = fmt::format("{}", (void *)rtsp);
        return rtsp_server_reply_setup(rtsp, 200, sessionStr.c_str(),
                                       transportStr.c_str());
    }
    // TODO: what else
    return rtsp_server_reply_setup(rtsp, 461, NULL, NULL);
}

int OnPlay(void *ptr, rtsp_server_t *rtsp, const char *uri, const char *session,
           const int64_t *npt, const double *scale)
{
    VLOG(1) << "play:\n" << uri;
    RtspService::EventRtspServer *pServer = (RtspService::EventRtspServer *)ptr;
    VideoRequest video(uri);
    auto it = pServer->owner->mVideoRequest2ProxyInfo.find(video);
    if (it == pServer->owner->mVideoRequest2ProxyInfo.end())
    {
        LOG(WARNING) << "video " << video
                     << " not found, may some error when register or process";
        return -1; // TODO: error code ( 一般是由于取流报错, 将这一路删除了 )
    }
    // 更新状态为 playing
    std::unique_lock clientsLock(it->second->mClientMutex);
    if (it->second->beforePlayClients.count(pServer) == 0)
    {
        // 454 Session Not Found
        return rtsp_server_reply_play(rtsp, 454, NULL, NULL, NULL);
    }
    it->second->beforePlayClients.erase(pServer);
    it->second->playingClients.insert(pServer);
    return rtsp_server_reply_play(rtsp, 200, npt, NULL, NULL);
}

int OnPause(void *ptr, rtsp_server_t *rtsp, const char *uri,
            const char *session, const int64_t *npt)
{
    LOG(INFO) << "pause\n" << uri;
    return 0;
}

int OnTeardown(void *ptr, rtsp_server_t *rtsp, const char *uri,
               const char *session)
{
    LOG(INFO) << "teardown\n" << uri;
    return 0;
}

int OnAnnounce(void *ptr, rtsp_server_t *rtsp, const char *uri, const char *sdp)
{
    LOG(INFO) << "annonce\n" << uri;
    return 0;
}

int OnRecord(void *ptr, rtsp_server_t *rtsp, const char *uri,
             const char *session, const int64_t *npt, const double *scale)
{
    LOG(INFO) << "record\n" << uri;
    return 0;
}

int OnOptions(void *ptr, rtsp_server_t *rtsp, const char *uri)
{
    VLOG(1) << "options:\n" << uri;
    RtspService::EventRtspServer *pServer = (RtspService::EventRtspServer *)ptr;
    VideoRequest video(uri);
    std::scoped_lock _(pServer->owner->mProxyInfoMutex);
    auto it = pServer->owner->mVideoRequest2ProxyInfo.find(video);
    if (it == pServer->owner->mVideoRequest2ProxyInfo.end())
    {
        auto pProxyInfo = std::make_unique<RtspService::ProxyInfo>();
        pProxyInfo->sendBuffer =
            std::unique_ptr<char, std::function<void(char *)>>(
                new char[1600], [](char *buf) { delete[] buf; });
        pProxyInfo->beforeDescribeClients.insert(pServer);
        pServer->owner->mVideoRequest2ProxyInfo[video] = std::move(pProxyInfo);
    }
    else
    {
        std::unique_lock clientsLock(it->second->mClientMutex);
        it->second->beforeDescribeClients.insert(pServer);
    }
    rtsp_server_reply_options(rtsp, 200);
    return 0;
}

int OnGetParameter(void *ptr, rtsp_server_t *rtsp, const char *uri,
                   const char *session, const void *content, int bytes)
{
    LOG(INFO) << "get parameter\n" << uri;
    return 0;
}

int OnSetParameter(void *ptr, rtsp_server_t *rtsp, const char *uri,
                   const char *session, const void *content, int bytes)
{
    LOG(INFO) << "set paramter\n" << uri;
    return 0;
}

RtspService::RtspService() : mpRtpProducer(std::make_unique<RtpProducer>()) {}

RtspService::~RtspService()
{
    for (int i = 0; i < mEventBases.size(); ++i)
    {
        event_base_loopbreak(mEventBases[i].get());
        mEventLoopThreads[i]->join();
    }
}

std::error_code RtspService::Init(Config config,
                                  std::shared_ptr<RtpProducer> rtpProducer)
{
    mpRtpProducer = rtpProducer;

    // 先注册好 callback, 再监听端口
    mpRtspHandler = std::make_unique<rtsp_handler_t>();
    mpRtspHandler->close = Close;
    mpRtspHandler->send = Send;
    mpRtspHandler->ondescribe = OnDescribe;
    mpRtspHandler->onsetup = OnSetup;
    mpRtspHandler->onplay = OnPlay;
    mpRtspHandler->onpause = OnPause;
    mpRtspHandler->onteardown = OnTeardown;
    mpRtspHandler->onannounce = OnAnnounce;
    mpRtspHandler->onrecord = OnRecord;
    mpRtspHandler->onoptions = OnOptions;
    mpRtspHandler->ongetparameter = OnGetParameter;
    mpRtspHandler->onsetparameter = OnSetParameter;

    int ret = evthread_use_pthreads();
    if (ret != 0)
    {
        LOG(FATAL) << "evthread use pthreads failed, with errno: " << errno;
    }

    sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(config.rtsp_port);
    sin.sin_addr.s_addr = INADDR_ANY;

    int thread_num = config.event_thread_num == -1
                         ? std::thread::hardware_concurrency()
                         : config.event_thread_num;
    for (int i = 0; i < thread_num; ++i)
    {
        mEventBases.emplace_back(
            std::unique_ptr<event_base, std::function<void(event_base *)>>(
                event_base_new(), [](event_base *eb) { event_base_free(eb); }));

        if (!mEventBases.back())
        {
            LOG(FATAL) << "create event base failed, with errno: " << errno;
        }

        mEventListeners.emplace_back(
            std::unique_ptr<evconnlistener,
                            std::function<void(evconnlistener *)>>(
                evconnlistener_new_bind(
                    mEventBases.back().get(), accept_conn_cb, this,
                    LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE |
                        LEV_OPT_REUSEABLE_PORT,
                    -1, (sockaddr *)&sin, sizeof(sockaddr)),
                [](evconnlistener *el) { evconnlistener_free(el); }));
        if (!mEventListeners.back())
        {
            LOG(FATAL) << "create listener failed for " << config.rtsp_port
                       << ", with error: " << strerror(errno);
        }

        mEventLoopThreads.emplace_back(std::make_unique<std::thread>(
            [i](event_base *base) {
                int ret = event_base_dispatch(base);
                LOG_IF(ERROR, ret != 0)
                    << "rtsp event loop " << i << " exist with code " << ret;
            },
            mEventBases.back().get()));
    }

    return std::error_code();
}

void RtspService::Dispatch()
{
    while (!mEventLoopThreads.empty())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
