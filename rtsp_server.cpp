#include <sys/socket.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <event2/listener.h>
#include <event2/buffer.h>

#include <arpa/inet.h>

#include "rtsp_server.h"

#define MAX_LINE 16384
#define NEWLINE "\r\n"

const std::string RTSPOK("RTSP/1.0 200 OK");
const std::string AllowedCommandNames("OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY");

std::mutex RTSPServer::videoMetaMutex;
std::map<std::string, VideoMeta> RTSPServer::videoMetaMap;
std::map<std::string, std::list<struct bufferevent*>> RTSPServer::waitingMetaMap;

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
    // if (strlen(line) > read) {
    //     line[read] = '\0'; // don't know why, but sometime they are not equal
    // }
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

void accept_conn_cb(struct evconnlistener *listener, 
                   evutil_socket_t sock,
                   struct sockaddr *addr,
                   int len,
                   void *ptr)
{
    struct sockaddr_in* sin = (struct sockaddr_in*) addr;
    LOG(INFO) << "connection from " << inet_ntoa(sin->sin_addr) << ":" << htons(sin->sin_port);
    struct event_base *base = evconnlistener_get_base(listener);
    struct bufferevent *bev = bufferevent_socket_new(base, sock, BEV_OPT_CLOSE_ON_FREE);

    RTSPServer* server = (RTSPServer*) ptr;
    server->mBev2ConnectionMap[bev] = RTSPConnection{*sin, 0};
    bufferevent_setcb(bev, readcb, NULL, errorcb, ptr);

    bufferevent_enable(bev, EV_READ|EV_WRITE);
}

void RTSPServer::addVideoSourceCallback(const VideoMeta& meta, const void* client) {
    VLOG(1) << "video meta: " << meta.toString();
    std::string* url = (std::string*) meta.data;
    RTSPServer* server = (RTSPServer*) client;
    std::scoped_lock _(videoMetaMutex);
    videoMetaMap[*url] = meta;
    auto it = waitingMetaMap.find(*url);
    if (it != waitingMetaMap.end()) {
        for (const auto& output : waitingMetaMap[*url]) {
            server->sendDescribeSdp(output, meta);
        }
        waitingMetaMap.erase(it);
    } else {
        LOG(ERROR) << "no client waiting for video " << *url;
    }
}

RTSPServer::RTSPServer(int p): mPort(p) {
    mpProbeVideoThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpVideoManagerService = new VideoManagerService();
};

RTSPServer::~RTSPServer() {
    if (mpVideoManagerService)
        delete mpProbeVideoThreadPool;
    if (mpVideoManagerService)
        delete mpVideoManagerService;
}

void RTSPServer::start() {
    struct sockaddr_in sin;
    struct event_base *base;
    struct event *listener_event;
    struct evconnlistener* connlistener;

    base = event_base_new();
    if (!base)
        return; /*XXXerr*/

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0);
    sin.sin_port = htons(mPort);
    connlistener = evconnlistener_new_bind(base,
                                           accept_conn_cb,
                                           this,
                                           LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE,
                                           -1,
                                           (struct sockaddr*)&sin,
                                           sizeof(sin));

    if (!connlistener) {
        LOG(FATAL) << "Couldn't create listener";
    }
    event_base_dispatch(base);
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
    std::scoped_lock _(videoMetaMutex);
    if (videoMetaMap.count(baseCommand.url) > 0) {
        sendDescribeSdp(bev, videoMetaMap[baseCommand.url]);
    } else if (waitingMetaMap.count(baseCommand.url) > 0) {
        waitingMetaMap[baseCommand.url].push_back(bev);
    } else {
        waitingMetaMap[baseCommand.url] = std::list<struct bufferevent*>{bev};
        const std::string& url = baseCommand.url;
        mpProbeVideoThreadPool->push([this, url] (int id) {
            mpVideoManagerService->addVideoSource(url, RTSPServer::addVideoSourceCallback, this);
        });
    }
}

void RTSPServer::sendDescribeSdp(struct bufferevent* bev, const VideoMeta& meta) {
    if (mBev2ConnectionMap.count(bev) == 0) {
        LOG(ERROR) << "unexcept error, unknown bufferevent";
        return;
    }

    std::ostringstream sdp("", std::ostringstream::ate);
    std::string payload;
    switch (meta.codec) {
        case CodecType::H264:
            payload = "H264"; break;
        case CodecType::HEVC:
            payload = "H265"; break;
        default:
            LOG(ERROR) << "video codec not supported, video meta: " << meta.toString();
            return;
    }
    sdp << "m=video 0 RTP/AVP 98"
        << NEWLINE
        << "a=rtpmap:98 " << payload << "/9000";
    // TODO: what about other parameters, such as duration
    sdp.seekp(0, std::ostringstream::end);
    auto len = sdp.tellp();

    std::ostringstream response(RTSPOK, std::ostringstream::ate);
    response << NEWLINE
             << "CSeq: " << mBev2ConnectionMap[bev].currentCseq
             << NEWLINE
             << "Content-Type: application/sdp"
             << NEWLINE
             << "Content-Length: " << len
             << NEWLINE
             << NEWLINE
             << sdp.str();
    response.seekp(0, std::ostringstream::end);
    len = response.tellp();
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
    response << NEWLINE
             << "CSeq: " << baseCommand.cseq
             << NEWLINE
             << "Transport: RTP/AVP/TCP;interleaved=0-1"
             << NEWLINE
             << "Session: 12345678"
             << NEWLINE
             << NEWLINE;
    response.seekp(0, std::ostringstream::end);
    auto len = response.tellp();
    LOG(INFO) << "zapeng: len: " << len << "\n" << response.str();
    evbuffer_add(bufferevent_get_output(bev), response.str().c_str(), len);
}

void RTSPServer::processPlayCommand(struct bufferevent* bev, const BaseCommand& baseCommand) {
    VLOG(1) << "receive cmmond PLAY: " << baseCommand.toString();
    uint8_t* data = new uint8_t[1024];
    size_t size;
    mpVideoManagerService->getFrame(baseCommand.url, data, size);
    LOG(INFO) << "data: " << data;
}
