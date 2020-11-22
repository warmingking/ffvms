#include <sys/socket.h>
#include <fcntl.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/thread.h>

#include <arpa/inet.h>

#include "rtsp_server.h"

#define MAX_LINE 16384
#define NEWLINE "\r\n"

const std::string RTSPOK("RTSP/1.0 200 OK");
const std::string AllowedCommandNames("OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY");

std::map<struct event*, RTSPServer*> RTSPServer::event2RTSPServerMap;

void readcb(struct bufferevent *bev, void *ctx)
{
    int rtn;
    RTSPServer* rtspServer = (RTSPServer*) ctx;
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
            rtspServer->processOptionCommand(bev, parsedCommand.second);
            break;
        case RTSPCommand::DESCRIBE:
            rtspServer->processDescribeCommand(bev, parsedCommand.second);
            break;
        case RTSPCommand::SETUP:
            rtspServer->processSetupCommand(bev, parsedCommand.second);
            break;
        case RTSPCommand::PLAY:
            rtspServer->processPlayCommand(bev, parsedCommand.second);
            break;
        default:
            LOG(ERROR) << "unknown rtsp command " << parsedCommand.first;
    }
}

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
    RTSPServer* server = (RTSPServer*) ctx;
    if (server->mBev2ConnectionMap.count(bev) == 0) {
        LOG(ERROR) << "unexcept, don't find connection info";
    } else {
        struct sockaddr_in sin = server->mBev2ConnectionMap[bev].sin;
        server->mBev2ConnectionMap.erase(bev);
        LOG(INFO) << "connection from " << inet_ntoa(sin.sin_addr) << ":" << htons(sin.sin_port) << " closed";
    }
    bufferevent_free(bev);
}

// void accept_error_cb(struct evconnlistener *listener, void *ctx)
// {
//     struct event_base *base = evconnlistener_get_base(listener);
//     int err = EVUTIL_SOCKET_ERROR();
//     LOG(INFO) << "Shutting down because of got an error on the listener:\n\t"
//               << err << ":" << evutil_socket_error_to_string(err);

//     event_base_loopexit(base, NULL);
// }

void writecc(struct bufferevent *bev, void *ctx) {
    LOG(INFO) << "-----------------------";
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

    RTSPServer* server = (RTSPServer*) ptr;
    server->mBev2ConnectionMap[bev] = RTSPConnection{*sin, 0};

    bufferevent_set_max_single_write(bev, 1400);
    bufferevent_setcb(bev, readcb, NULL, errorcb, ptr);
    bufferevent_enable(bev, EV_READ|EV_WRITE);
}

void addVideoSourceCallback(const std::pair<const std::string*, const std::string*>& url2sdp, const void* client) {
    const std::string* url = url2sdp.first;
    const std::string* sdp = url2sdp.second;
    VLOG(1) << "url " << *url << " get sdp:\n" << *sdp;
    RTSPServer* server = (RTSPServer*) client;
    std::scoped_lock _(server->mVideoSdpMutex);
    server->mUrl2SdpMap[*url] = *sdp;
    auto it = server->mWaitingMetaMap.find(*url);
    if (it != server->mWaitingMetaMap.end()) {
        for (const auto& output : server->mWaitingMetaMap[*url]) {
            server->sendDescribeSdp(output, *sdp);
        }
        server->mWaitingMetaMap.erase(it);
    } else {
        LOG(ERROR) << "no client waiting for video " << *url;
    }
}

void writeFrameInEventLoop(evutil_socket_t fd, short what, void *arg) {
    struct event* me = (struct event*) arg;
    RTSPServer* server = RTSPServer::event2RTSPServerMap[me];
    if (!server) {
        LOG(ERROR) << "unexcept error, RTSPServer nullptr";
        return;
    }

    server->mpGetFrameThreadPool->push([server, me] (int id) {
        const auto t = std::chrono::system_clock::now();
        std::vector<size_t> lens;
        const std::string& url = server->mEvent2UrlMap[me];
        server->mpVideoManagerService->getFrame(url);
        LOG_EVERY_N(INFO, 250) << "send one frame spend "
                             << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - t).count()
                             << " microseconds";
    });
}

void RTSPServer::sendFrame(const std::set<struct bufferevent*>& bevs, const uint8_t* data, const size_t len) {
    LOG(INFO) << "========== sendFrame ============";
}

RTSPServer::RTSPServer(int p): mPort(p), mpBase(NULL){
    mpProbeVideoThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpGetFrameThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpVideoManagerService = new VideoManagerService();
};

RTSPServer::~RTSPServer() {
    // TODO: add lock
    for (auto it = event2RTSPServerMap.begin(); it != event2RTSPServerMap.end(); it++) {
        if (it->second == this) {
            it->second = nullptr;
        }
    }
    if (mpVideoManagerService)
        delete mpProbeVideoThreadPool;
    if (mpGetFrameThreadPool)
        delete mpGetFrameThreadPool;
    if (mpVideoManagerService)
        delete mpVideoManagerService;
}

void RTSPServer::start() {
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
    sin.sin_port = htons(mPort);
    connlistener = evconnlistener_new_bind(mpBase,
                                           accept_conn_cb,
                                           this,
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
    if (mUrl2SdpMap.count(baseCommand.url) > 0) {
        sendDescribeSdp(bev, mUrl2SdpMap[baseCommand.url]);
    } else if (mWaitingMetaMap.count(baseCommand.url) > 0) {
        mWaitingMetaMap[baseCommand.url].push_back(bev);
    } else {
        // 用insert是想得到insert之后的url，这个url函数结束不会释放
        const auto [it, success] = mWaitingMetaMap.insert(std::make_pair(baseCommand.url, std::list<struct bufferevent*> {bev}));
        if (!success) {
            LOG(ERROR) << "insert mWaitingMetaMap failed, url: " << baseCommand.url;
            return;
        }
        const std::string *url = &it->first;
        mpProbeVideoThreadPool->push([this, url] (int id) {
            mpVideoManagerService->addVideoSource(url, addVideoSourceCallback, this);
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
             << "Session: 12345678" // TODO: 按照rtsp协议生成session
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

    if (mPlayingVideoMap.count(baseCommand.url) == 0) {
        LOG(INFO) << "new play command, session id: " << baseCommand.session;
        struct event* event = event_new(mpBase, -1, EV_TIMEOUT | EV_PERSIST, writeFrameInEventLoop, event_self_cbarg());
        mEvent2UrlMap[event] = baseCommand.url;
        event2RTSPServerMap[event] = this;
        std::pair<uint32_t, uint32_t> fps;
        int ret = mpVideoManagerService->getVideoFps(baseCommand.url, fps);
        if (ret < 0) {
            LOG(ERROR) << "unexcept error, cannot get video fps for url " << baseCommand.url;
        }
        long microseconds = 1e6 * fps.second / fps.first;
        struct timeval frameInterval = {0, microseconds};
        event_add(event, &frameInterval);
        mPlayingVideoMap[baseCommand.url] = {bev};
    } else {
        mPlayingVideoMap[baseCommand.url].insert(bev);
    }
}

uint8_t* buffer = new uint8_t[1500];

void RTSPServer::writeRtpData(std::string& url, uint8_t* data, size_t len) {
    if (mPlayingVideoMap.count(url) == 0) {
        LOG(ERROR) << "unexcept error, mPlayingVideoMap not found url " << url;
        return;
    }
    int payload = data[1] & 0x7f;
    int interleaved(0);
    if (payload >= 72 && payload <= 76) {
        interleaved = 1;
    }
    for (auto& bev : mPlayingVideoMap[url]) {
        buffer[0] = '$';
        buffer[1] = interleaved;
        buffer[2] = len >> 8;
        buffer[3] = len & 0xff;
        std::copy(data, &data[len], &buffer[4]);
        evbuffer_add(bufferevent_get_output(bev), buffer, len + 4);
    }
}
