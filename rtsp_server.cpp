#include "rtsp_server.h"

#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/thread.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#define MAX_LINE 16384
#define NEWLINE "\r\n"

const std::string RTSPOK("RTSP/1.0 200 OK");
const std::string RTSPNOTFOUND("RTSP/1.0 404 Not Found");
const std::string RTSPSERVERBUSY("RTSP/1.0 500 Internal Server Busy");
const std::string AllowedCommandNames("OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY");

// 从 RTSP client 接收数据, 并且处理为 RTSPCommand -> BaseCommand
void readcb(struct bufferevent *bev, void *ctx) {
    int rtn;
    RTSPServer &rtspServer = RTSPServer::getInstance();
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
            VLOG(1) << "unsupport rtsp command " << parsedCommand.first;
    }
}

// 处理 RTSP client 关闭连接
void errorcb(struct bufferevent *bev, short error, void *ctx) {
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

    char *addressStr = (char *)ctx;
    LOG(INFO) << "connection closed from " << addressStr;
    delete[] addressStr;

    RTSPServer &server = RTSPServer::getInstance();
    std::shared_lock _(server.mMutex);
    for (auto &request2video : server.mProcessingVideoMap) {
        // alias
        RTSPServer::VideoObject *pVideo = request2video.second;
        if (pVideo->mBev2ConnectionMap.count(bev) != 0) {
            std::unique_lock videoLock(pVideo->mMutex);
            pVideo->mBev2ConnectionMap.erase(bev);
            if (pVideo->mBev2ConnectionMap.empty()) {
                LOG(INFO) << "add delay event for request " << request2video.first;
                struct event *event = event_new(
                    bufferevent_get_base(bev), -1, EV_TIMEOUT,
                    [](evutil_socket_t fd, short what, void *arg) {
                        RTSPServer &server = RTSPServer::getInstance();
                        // alias
                        VideoRequest *pRequest = (VideoRequest *)arg;
                        std::unique_lock _(server.mMutex);  // 加锁，防止正在停的时候来了新的请求
                        auto it = server.mProcessingVideoMap.find(*pRequest);
                        if (it != server.mProcessingVideoMap.end() && it->second->mBev2ConnectionMap.empty()) {
                            // 如果这一路依然存在
                            // 且connection为空, 停掉这一路
                            server.teardown(*pRequest);
                        }
                    },
                    const_cast<VideoRequest *>(&request2video.first));
                struct timeval frameInterval = {5, 0};
                event_add(event, &frameInterval);
            }
            bufferevent_free(bev);
            break;  // 已经找到对应的路, 跳过 for 循环
        }
    }
}

void accept_conn_cb(struct evconnlistener *listener, evutil_socket_t sock, struct sockaddr *addr, int len, void *ptr) {
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    char *addressStr = new char[128 + 1 + 5 + 1];  // ip + ":" + port(itoa) + "\0"
    char *ip = inet_ntoa(sin->sin_addr);
    strcpy(addressStr, ip);
    addressStr[strlen(ip)] = ':';
    sprintf(&addressStr[strlen(ip) + 1], "%d", htons(sin->sin_port));
    LOG(INFO) << "connection from " << addressStr;
    struct event_base *base = evconnlistener_get_base(listener);
    struct bufferevent *bev =
        bufferevent_socket_new(base, sock, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS);

    bufferevent_set_max_single_write(bev, 1400);  // 不大于mtu, 确保数据可以即时发送走
    bufferevent_setcb(bev, readcb, NULL, errorcb, addressStr);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

void writeFrameInEventLoop(evutil_socket_t fd, short what, void *arg) {
    VideoRequest *pVideoRequest = (VideoRequest *)arg;
    RTSPServer &server = RTSPServer::getInstance();
    server.mpVideoManagerService->getFrameAsync(*pVideoRequest);
}

RTSPServer::VideoObject::VideoObject() : sdpReady(false), pPlayingEvent(NULL) {}

RTSPServer &RTSPServer::getInstance() {
    static RTSPServer instance;
    return instance;
}

RTSPServer::RTSPServer() : mSessionId(0) {
    mpIOThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency() - 1);
    mpVideoManagerService = new VideoManagerService(this);
};

RTSPServer::~RTSPServer() {
    // TODO: to expand
    if (mpVideoManagerService) delete mpVideoManagerService;
}

void RTSPServer::start(uint16_t port) {
    struct sockaddr_in sin;

    int ret = evthread_use_pthreads();
    if (ret != 0) {
        LOG(FATAL) << "evthread_use_pthreads failed";
    }

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0);
    sin.sin_port = htons(port);

    for (int i = 0; i != mpIOThreadPool->size(); ++i) {
        mpIOThreadPool->push([this, &sin](int id) { startIOLoop(sin); });
    }
    startIOLoop(sin);
}

void RTSPServer::startIOLoop(struct sockaddr_in &sin) {
    struct event_base *base = event_base_new();
    if (!base) {
        LOG(FATAL) << "failed to create event base";
    }
    struct evconnlistener *connlistener = evconnlistener_new_bind(
        base, accept_conn_cb, NULL, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_REUSEABLE_PORT, -1, (struct sockaddr *)&sin, sizeof(sin));

    if (!connlistener) {
        LOG(FATAL) << "Couldn't create listener";
    }
    event_base_dispatch(base);
}

void RTSPServer::processOptionCommand(struct bufferevent *bev, const BaseCommand &baseCommand) {
    VLOG(1) << "receive command OPTIONS: " << baseCommand;
    RTSPServer::VideoObject *pVideo;
    {  // 这里假设client都从OPTION开始
        std::unique_lock _(mMutex);
        if (mProcessingVideoMap.count(baseCommand.videoRequest) == 0) {
            pVideo = new RTSPServer::VideoObject();
            pVideo->mBev2ConnectionMap[bev] = RTSPConnection{baseCommand.cseq};
            mProcessingVideoMap[baseCommand.videoRequest] = pVideo;
        } else {
            pVideo = mProcessingVideoMap[baseCommand.videoRequest];
            std::shared_lock videoLock(pVideo->mMutex);
            pVideo->mBev2ConnectionMap[bev].currentCseq = baseCommand.cseq;
        }
    }
    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    response << NEWLINE << "CSeq: " << baseCommand.cseq << NEWLINE << "Public: " << AllowedCommandNames << NEWLINE << NEWLINE;
    response.seekp(0, std::ostringstream::end);
    auto len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

void RTSPServer::processDescribeCommand(struct bufferevent *bev, const BaseCommand &baseCommand) {
    VLOG(1) << "receive command DESCRIBE: " << baseCommand;
    std::shared_lock _(mMutex);
    if (mProcessingVideoMap.count(baseCommand.videoRequest) == 0) {
        LOG(ERROR) << "unexcept error, request " << baseCommand.videoRequest << " not recorded";
        return;  // TODO: return 400
    }
    RTSPServer::VideoObject *pVideo = mProcessingVideoMap[baseCommand.videoRequest];
    pVideo->mBev2ConnectionMap[bev].currentCseq = baseCommand.cseq;
    // std::scoped_lock _(pVideo->mMutex);
    if (pVideo->sdpReady) {
        sendDescribeSdp(bev, baseCommand.cseq, pVideo->sdp);
    } else {
        mpVideoManagerService->addAndProbeVideoSourceAsync(baseCommand.videoRequest, [](const VideoRequest &request, const int ret, const std::string &sdp) {
            VLOG(1) << "request " << request << " get sdp:\n" << sdp;
            RTSPServer &server = RTSPServer::getInstance();
            std::shared_lock _(server.mMutex);
            if (server.mProcessingVideoMap.count(request) == 0) {
                LOG(WARNING) << "no recorded request " << request << ", maybe disconnected";
                return;  // TODO: return 400
            }
            RTSPServer::VideoObject *pVideo = server.mProcessingVideoMap[request];
            if (ret == 0) {
                for (const auto &bev2connection : pVideo->mBev2ConnectionMap) {
                    server.sendDescribeSdp(bev2connection.first, bev2connection.second.currentCseq, sdp);
                }
                pVideo->sdp = sdp;
                pVideo->sdpReady = true;
            } else {
                for (const auto &bev2connection : pVideo->mBev2ConnectionMap) {
                    server.sendRtspError(bev2connection.first, bev2connection.second.currentCseq, ret);
                }
            }
        });
    }
}

void RTSPServer::sendRtspError(struct bufferevent *bev, const int currentCseq, const int error) {
    // 不用加锁, 调用它的函数已经加锁
    std::string msg;
    if (error == 404) {
        msg = RTSPNOTFOUND;
    } else {
        msg = RTSPSERVERBUSY;
    }
    std::ostringstream response(msg, std::ostringstream::ate);
    response << NEWLINE << "CSeq: " << currentCseq << NEWLINE << NEWLINE;
    response.seekp(0, std::ostringstream::end);
    const size_t len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

void RTSPServer::sendDescribeSdp(struct bufferevent *bev, const int currentCseq, const std::string &sdp) {
    // 不用加锁, 调用它的函数已经加锁
    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    response << NEWLINE << "CSeq: " << currentCseq << NEWLINE << "Content-Type: application/sdp" << NEWLINE << "Content-Length: " << sdp.length() << NEWLINE
             << NEWLINE << sdp;
    response.seekp(0, std::ostringstream::end);
    const size_t len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

std::string RTSPServer::getSessionId() {
    std::ostringstream oss;
    oss << std::hex << ++mSessionId;
    return oss.str();
}

void RTSPServer::processSetupCommand(struct bufferevent *bev, const BaseCommand &baseCommand) {
    VLOG(1) << "receive command SETUP: " << baseCommand;
    std::shared_lock _(mMutex);
    if (mProcessingVideoMap.count(baseCommand.videoRequest) == 0) {
        LOG(ERROR) << "unexcept error, request " << baseCommand.videoRequest << " not recorded";
        return;  // TODO: return 400
    }
    RTSPServer::VideoObject *pVideo = mProcessingVideoMap[baseCommand.videoRequest];
    pVideo->mBev2ConnectionMap[bev].currentCseq = baseCommand.cseq;

    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    if (baseCommand.streamingMode == StreamingMode::TCP) {
        response << NEWLINE << "CSeq: " << baseCommand.cseq << NEWLINE << "Transport: RTP/AVP/TCP;interleaved=0-1" << NEWLINE
                 << "Session: " << getSessionId()  // TODO: 按照rtsp协议生成session
                 << NEWLINE << NEWLINE;
    } else {
        LOG(ERROR) << "not support stream mode udp yet";  // TODO: add udp
        return;
    }
    response.seekp(0, std::ostringstream::end);
    auto len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

void RTSPServer::processPlayCommand(struct bufferevent *bev, const BaseCommand &baseCommand) {
    VLOG(1) << "receive command PLAY: " << baseCommand;
    std::shared_lock _(mMutex);
    auto it = mProcessingVideoMap.find(baseCommand.videoRequest);
    if (it == mProcessingVideoMap.end()) {
        LOG(ERROR) << "unexcept error, request " << baseCommand.videoRequest << " not recorded";
        return;  // TODO: return 400
    }
    RTSPServer::VideoObject *pVideo = it->second;
    pVideo->mBev2ConnectionMap[bev].currentCseq = baseCommand.cseq;
    LOG(INFO) << "new play command for request " << baseCommand.videoRequest << ", session id: " << baseCommand.session;

    if (pVideo->pPlayingEvent == NULL) {
        std::pair<uint32_t, uint32_t> fps;
        int ret = mpVideoManagerService->getVideoFps(baseCommand.videoRequest, fps);
        if (ret < 0) {
            LOG(ERROR) << "unexcept error, cannot get video fps for url " << baseCommand.url;
            return;
        }

        long microseconds = 1e6 * fps.second / fps.first;
        struct timeval frameInterval = {0, microseconds};
        pVideo->pPlayingEvent =
            event_new(bufferevent_get_base(bev), -1, EV_TIMEOUT | EV_PERSIST, writeFrameInEventLoop, const_cast<VideoRequest *>(&it->first));
        event_add(pVideo->pPlayingEvent, &frameInterval);
    }

    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    response << NEWLINE << "CSeq: " << baseCommand.cseq << NEWLINE << "Session: " << baseCommand.session << NEWLINE << NEWLINE;
    response.seekp(0, std::ostringstream::end);
    auto len = response.tellp();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

void RTSPServer::teardown(const VideoRequest &request) {
    // 无需加锁, 调用者已经加锁了
    VideoObject *pVideo = mProcessingVideoMap[request];
    if (pVideo->pPlayingEvent != NULL) {
        // event_del(pVideo->pPlayingEvent);
        // It is safe to call event_free() on an event that is pending or active:
        // doing so makes the event non-pending and inactive before deallocating it.
        event_free(pVideo->pPlayingEvent);
        LOG(INFO) << "to teardown request " << request;
        mpVideoManagerService->teardownAsync(request);
    }
    LOG(INFO) << "clear request " << request;
    mProcessingVideoMap.erase(request);
    delete pVideo;
}

void RTSPServer::writeRtpData(const VideoRequest &url, uint8_t *data, size_t len) {
    std::shared_lock _(mMutex);
    if (mProcessingVideoMap.count(url) == 0) {
        LOG(ERROR) << "unexcept error, request " << url << " not recorded";
        return;  // TODO: return 400
    }
    RTSPServer::VideoObject *pVideo = mProcessingVideoMap[url];
    std::shared_lock videoLock(pVideo->mMutex);

    int payload = data[1] & 0x7f;
    int interleaved(0);
    if (payload >= 72 && payload <= 76) {  // RTCP
        interleaved = 1;
    }

    pVideo->rtpHeader[0] = '$';
    pVideo->rtpHeader[1] = interleaved;
    pVideo->rtpHeader[2] = len >> 8;
    pVideo->rtpHeader[3] = len & 0xff;
    // TODO: 这里可以控制的更精细一点，ref: http://www.wangafu.net/~nickm/libevent-book/Ref7_evbuffer.html#Avoiding~data~copies~with~evbuffer-based~IO
    // 同时evbuffer_ref_cleanup_cb方法可以监听数据发送完成，
    // 手动控制event
    for (const auto &bev2connection : pVideo->mBev2ConnectionMap) {
        evbuffer_add(bufferevent_get_output(bev2connection.first), pVideo->rtpHeader, 4);
        evbuffer_add(bufferevent_get_output(bev2connection.first), data, len);
    }
}

void RTSPServer::streamComplete(const VideoRequest &request) {
    LOG(INFO) << "stream " << request << " complete, will close all connections";
    std::unique_lock _(mMutex);
    if (mProcessingVideoMap.count(request) == 0) {
        LOG(ERROR) << "unexcept error, request " << request << " not recorded";
        return;  // TODO: return 400
    }
    RTSPServer::VideoObject *pVideo = mProcessingVideoMap[request];
    event_free(pVideo->pPlayingEvent);
    for (const auto &bev2connection : pVideo->mBev2ConnectionMap) {
        evutil_closesocket(bufferevent_getfd(bev2connection.first));
        bufferevent_free(bev2connection.first);
    }
    pVideo->mBev2ConnectionMap.clear();
    mProcessingVideoMap.erase(request);
}
