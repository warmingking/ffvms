#include <sys/socket.h>
#include <fcntl.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <fstream>

#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/thread.h>

#include <arpa/inet.h>

#include "rtsp_server.h"

#define MAX_LINE 16384
#define NEWLINE "\r\n"

const std::string RTSPOK("RTSP/1.0 200 OK");
const std::string AllowedCommandNames("OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY");

// 从 RTSP client 接收数据, 并且处理为 RTSPCommand -> BaseCommand
void readcb(struct bufferevent *bev, void *ctx)
{
    int rtn;
    RTSPServer& rtspServer = RTSPServer::getInstance();;
    struct evbuffer *input = bufferevent_get_input(bev);
    char line[MAX_LINE];
    bool foundCommandName = false;

    int read = evbuffer_remove(input, &line, MAX_LINE);
    if (read == -1) {
        LOG(ERROR) << "bufferevent can't drain the buffer";
        return;
    }
    std::pair<RTSPCommand, BaseCommand> parsedCommand;
    size_t nparsed = RTSPParser::execteParse(line, read, parsedCommand);
    switch (parsedCommand.first) {
        case RTSPCommand::OPTIONS:
            rtspServer.processOptionCommand(bev, parsedCommand.second);
            break;
        case RTSPCommand::DESCRIBE:
            rtspServer.processDescribeCommand(bev, parsedCommand.second);
            break;
        case RTSPCommand::SETUP:
            rtspServer.processSetupCommand(bev, parsedCommand.second);
            break;
        case RTSPCommand::PLAY:
            rtspServer.processPlayCommand(bev, parsedCommand.second);
            break;
        default:
            LOG(ERROR) << "unknown rtsp command " << parsedCommand.first;
    }
}

// 处理 RTSP client 关闭连接
void errorcb(struct bufferevent *bev, short error, void *ctx)
{
    if (error & BEV_EVENT_EOF) {
        /* connection has been closed, do any clean up here */
        /* ... */
    } else if (error & BEV_EVENT_ERROR) {
        /* check errno to see what error occurred */
        /* ... */
    } else if (error & BEV_EVENT_TIMEOUT) {
        /* must be a timeout event handle, handle it */
        /* ... */
    }
    RTSPServer& server = RTSPServer::getInstance();
    if (server.mBev2ConnectionMap.count(bev) == 0) {
        LOG(ERROR) << "unexcept, don't find connection info";
    } else {
        struct sockaddr_in sin = server.mBev2ConnectionMap[bev].sin;
        // 1. 移除对应的 rtspconnection
        server.mBev2ConnectionMap.erase(bev);
        LOG(INFO) << "connection from " << inet_ntoa(sin.sin_addr) << ":" << htons(sin.sin_port) << " closed";
        // 2. 遍历并删除mPlayingVideoMap中对应的bev, 如果对应的路没有client了, 注册一个延时event, 在一定的时间后关闭这一路
        std::for_each(server.mPlayingVideoMap.begin(), server.mPlayingVideoMap.end(),
                      [&server, bev] (auto& url2bevs) {
            url2bevs.second.second.erase(bev);
            if (url2bevs.second.second.empty()) {
                // FIXME: 线程不安全
                struct event* event = event_new(server.mpBase, -1, EV_TIMEOUT, [] (evutil_socket_t fd, short what, void *arg) {
                    RTSPServer& server = RTSPServer::getInstance();
                    VideoRequest* pRequest = (VideoRequest*) arg;
                    // 3. 再次判断是否为空, 如果为空, 停掉这一路
                    if (server.mPlayingVideoMap[*pRequest].second.empty()) {
                        LOG(INFO) << "to stop request " << *pRequest;
                        if (server.mUrl2EventMap.count(*pRequest) == 0) {
                            LOG(INFO) << "unexcept error, not find playing event for request " << *pRequest;
                        }
                        struct event* ev = server.mUrl2EventMap[*pRequest];
                        event_del(ev);
                        event_free(ev);
                        server.mUrl2EventMap.erase(*pRequest);
                        delete[] server.mPlayingVideoMap[*pRequest].first;
                        server.mPlayingVideoMap.erase(*pRequest);
                    }
                }, const_cast<VideoRequest*>(&url2bevs.first));
                struct timeval frameInterval = {10, 0};
                event_add(event, &frameInterval);
            }
        });
    }
    bufferevent_free(bev);
}

void accept_conn_cb(struct evconnlistener *listener,
                   evutil_socket_t sock,
                   struct sockaddr *addr,
                   int len,
                   void *ptr)
{
    struct sockaddr_in* sin = (struct sockaddr_in*) addr;
    LOG(INFO) << "connection from " << inet_ntoa(sin->sin_addr) << ":" << htons(sin->sin_port);
    struct event_base *base = evconnlistener_get_base(listener);
    struct bufferevent *bev = bufferevent_socket_new(base, sock, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_THREADSAFE);

    RTSPServer& server = RTSPServer::getInstance();
    server.mBev2ConnectionMap[bev] = RTSPConnection{*sin, 0};

    // 不大于mtu, 确保数据可以即时发送走
    bufferevent_set_max_single_write(bev, 1400);
    bufferevent_setcb(bev, readcb, NULL, errorcb, NULL);
    bufferevent_enable(bev, EV_READ|EV_WRITE);
}

void writeFrameInEventLoop(evutil_socket_t fd, short what, void *arg) {
    VideoRequest* pVideoRequest = (VideoRequest*) arg;
    RTSPServer& server = RTSPServer::getInstance();

    server.mpGetFrameThreadPool->push([&server, pVideoRequest] (int id) {
        const auto t = std::chrono::system_clock::now();
        server.mpVideoManagerService->getFrame(*pVideoRequest);
        LOG_EVERY_N(INFO, 256) << "send one frame for " << *pVideoRequest << " spend "
                             << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - t).count()
                             << " microseconds";
    });
}

void RTSPServer::sendFrame(const std::set<struct bufferevent*>& bevs, const uint8_t* data, const size_t len) {
    LOG(INFO) << "========== sendFrame ============";
}

RTSPServer& RTSPServer::getInstance() {
    static RTSPServer instance;
    return instance;
}

RTSPServer::RTSPServer(): mpBase(NULL), mSessionId(0) {
    mpProbeVideoThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpGetFrameThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    // mpGetFrameThreadPool = new ctpl::thread_pool(1);
    mpVideoManagerService = new VideoManagerService(this);
};

RTSPServer::~RTSPServer() {
    // TODO: add lock
    if (mpVideoManagerService)
        delete mpProbeVideoThreadPool;
    if (mpGetFrameThreadPool)
        delete mpGetFrameThreadPool;
    if (mpVideoManagerService)
        delete mpVideoManagerService;
}

void RTSPServer::start(int port) {
    struct sockaddr_in sin;
    struct evconnlistener* connlistener;

    int ret = evthread_use_pthreads();
    if (ret != 0) {
        LOG(FATAL) << "evthread_use_pthreads failed";
    }
    mpBase = event_base_new();
    if (!mpBase)
        LOG(FATAL) << "failed to create event base";

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0);
    sin.sin_port = htons(port);
    connlistener = evconnlistener_new_bind(mpBase,
                                           accept_conn_cb,
                                           NULL,
                                           LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE,
                                           -1,
                                           (struct sockaddr*)&sin,
                                           sizeof(sin));

    if (!connlistener) {
        LOG(FATAL) << "Couldn't create listener";
    }
    event_base_dispatch(mpBase);
}

void RTSPServer::processOptionCommand(struct bufferevent* bev, const BaseCommand& baseCommand) {
    VLOG(1) << "receive cmmond OPTIONS: " << baseCommand.toString();
    if (mBev2ConnectionMap.count(bev) == 0) {
        LOG(ERROR) << "unexcept error, unknown bufferevent";
        return;
    }
    mBev2ConnectionMap[bev].currentCseq = baseCommand.cseq;
    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    response << NEWLINE
             << "CSeq: " << baseCommand.cseq
             << NEWLINE
             << "Public: " << AllowedCommandNames
             << NEWLINE
             << NEWLINE;
    response.seekp(0, std::ostringstream::end);
    auto len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

void RTSPServer::processDescribeCommand(struct bufferevent* bev, const BaseCommand& baseCommand) {
    VLOG(1) << "receive cmmond DESCRIBE: " << baseCommand.toString();
    if (mBev2ConnectionMap.count(bev) == 0) {
        LOG(ERROR) << "unexcept error, unknown bufferevent";
        return;
    }
    mBev2ConnectionMap[bev].currentCseq = baseCommand.cseq;
    std::scoped_lock _(mVideoSdpMutex);
    if (mVideoRequest2SdpMap.count(baseCommand.videoRequest) > 0) {
        sendDescribeSdp(bev, mVideoRequest2SdpMap[baseCommand.videoRequest]);
    } else if (mWaitingMetaMap.count(baseCommand.videoRequest) > 0) {
        mWaitingMetaMap[baseCommand.videoRequest].push_back(bev);
    } else {
        // 用insert是想得到insert之后的url，这个url函数结束不会释放
        const auto [it, success] = mWaitingMetaMap.insert(std::make_pair(baseCommand.videoRequest, std::list<struct bufferevent*> {bev}));
        if (!success) {
            LOG(ERROR) << "insert mWaitingMetaMap failed, request: " << baseCommand.videoRequest;
            return;
        }
        mpProbeVideoThreadPool->push([this, &baseCommand] (int id) {
            mpVideoManagerService->addAndProbeVideoSourceAsync(baseCommand.videoRequest, [] (const std::pair<const VideoRequest*, std::string*>& video2sdp) {
                const VideoRequest* url = video2sdp.first;
                const std::string* sdp = video2sdp.second;
                VLOG(1) << "request " << *url << " get sdp:\n" << *sdp;
                RTSPServer& server = RTSPServer::getInstance();
                std::scoped_lock _(server.mVideoSdpMutex);
                server.mVideoRequest2SdpMap[*url] = *sdp;
                auto it = server.mWaitingMetaMap.find(*url);
                if (it != server.mWaitingMetaMap.end()) {
                    for (const auto& output : server.mWaitingMetaMap[*url]) {
                        server.sendDescribeSdp(output, *sdp);
                    }
                    server.mWaitingMetaMap.erase(it);
                } else {
                    LOG(ERROR) << "no client waiting for video " << *url;
                }
            });
        });
    }
}

void RTSPServer::sendDescribeSdp(struct bufferevent* bev, const std::string& sdp) {
    if (mBev2ConnectionMap.count(bev) == 0) {
        LOG(ERROR) << "unexcept error, unknown bufferevent";
        return;
    }

    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    response << NEWLINE
             << "CSeq: " << mBev2ConnectionMap[bev].currentCseq
             << NEWLINE
             << "Content-Type: application/sdp"
             << NEWLINE
             << "Content-Length: " << sdp.length()
             << NEWLINE
             << NEWLINE
             << sdp;
    response.seekp(0, std::ostringstream::end);
    const size_t len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

std::string RTSPServer::getSessionId() {
    std::ostringstream oss;
    oss << std::hex << ++mSessionId;
    return oss.str();
}

void RTSPServer::processSetupCommand(struct bufferevent* bev, const BaseCommand& baseCommand) {
    VLOG(1) << "receive cmmond SETUP: " << baseCommand.toString();
    if (mBev2ConnectionMap.count(bev) == 0) {
        LOG(ERROR) << "unexcept error, unknown bufferevent";
        return;
    }
    mBev2ConnectionMap[bev].currentCseq = baseCommand.cseq;
    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    if (baseCommand.streamingMode == StreamingMode::TCP) {
        response << NEWLINE
             << "CSeq: " << baseCommand.cseq
             << NEWLINE
             << "Transport: RTP/AVP/TCP;interleaved=0-1"
             << NEWLINE
             << "Session: " << getSessionId() // TODO: 按照rtsp协议生成session
             << NEWLINE
             << NEWLINE;
    } else {
        LOG(ERROR) << "not support stream mode udp yet";
        return;
    }
    response.seekp(0, std::ostringstream::end);
    auto len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

void RTSPServer::processPlayCommand(struct bufferevent* bev, const BaseCommand& baseCommand) {
    VLOG(1) << "receive cmmond PLAY: " << baseCommand.toString();
    if (mBev2ConnectionMap.count(bev) == 0) {
        LOG(ERROR) << "unexcept error, unknown bufferevent";
        return;
    }
    mBev2ConnectionMap[bev].currentCseq = baseCommand.cseq;
    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    response << NEWLINE
             << "CSeq: " << baseCommand.cseq
             << NEWLINE
             << "Session: " << baseCommand.session
             << NEWLINE
             << NEWLINE;
    response.seekp(0, std::ostringstream::end);
    auto len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);

    // TODO: add lock
    if (mPlayingVideoMap.count(baseCommand.videoRequest) == 0) {
        LOG(INFO) << "new play command, session id: " << baseCommand.session;
        std::pair<uint32_t, uint32_t> fps;
        int ret = mpVideoManagerService->getVideoFps(baseCommand.videoRequest, fps);
        if (ret < 0) {
            LOG(ERROR) << "unexcept error, cannot get video fps for url " << baseCommand.url;
            return;
        }
        std::set<bufferevent*> playingBev{bev};
        const auto [it, success] = mPlayingVideoMap.insert(std::make_pair(baseCommand.videoRequest, std::make_pair(new uint8_t[4], playingBev)));
        if (!success) {
            LOG(ERROR) << "unexcept error, cannot insert bufferevent for url " << baseCommand.url;
            return;
        }
        long microseconds = 1e6 * fps.second / fps.first;
        struct timeval frameInterval = {0, microseconds};
        struct event* event = event_new(mpBase, -1, EV_TIMEOUT | EV_PERSIST, writeFrameInEventLoop, const_cast<VideoRequest*>(&it->first));
        event_add(event, &frameInterval);
        mUrl2EventMap[baseCommand.videoRequest] = event;
    } else {
        mPlayingVideoMap[baseCommand.videoRequest].second.insert(bev);
    }
}

void RTSPServer::writeRtpData(const VideoRequest& url, uint8_t* data, size_t len) {
    if (mPlayingVideoMap.count(url) == 0) {
        LOG(ERROR) << "unexcept error, mPlayingVideoMap not found url " << url;
        return;
    }
    int payload = data[1] & 0x7f;
    int interleaved(0);
    if (payload >= 72 && payload <= 76) { // RTCP
        interleaved = 1;
    }
    const auto& buf2bevs = mPlayingVideoMap[url];
    buf2bevs.first[0] = '$';
    buf2bevs.first[1] = interleaved;
    buf2bevs.first[2] = len >> 8;
    buf2bevs.first[3] = len & 0xff;
    // TODO: 这里可以控制的更精细一点，ref: http://www.wangafu.net/~nickm/libevent-book/Ref7_evbuffer.html#Avoiding~data~copies~with~evbuffer-based~IO
    // 同时evbuffer_ref_cleanup_cb方法可以监听数据发送完成，
    // 手动控制event
    for (auto& bev : buf2bevs.second) {
        evbuffer_add(bufferevent_get_output(bev), buf2bevs.first, 4);
        evbuffer_add(bufferevent_get_output(bev), data, len);
    }
}
