#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include <event2/listener.h>
#include <event2/buffer.h>

#include <arpa/inet.h>

#include "rtsp_server.h"

#define MAX_LINE 16384

void readcb(struct bufferevent *bev, void *ctx)
{
    int rtn;
    RTSPServer* rtspServer = (RTSPServer*) ctx;
    struct evbuffer *input, *output;
    char *line;
    size_t n;
    int i;
    input = bufferevent_get_input(bev);
    output = bufferevent_get_output(bev);
    bool foundCommandName = false;

    while ((line = evbuffer_readln(input, &n, EVBUFFER_EOL_ANY))) {
        LOG(INFO) << "receive msg: " << line;
        if (!foundCommandName) {
            RTSPCommand command = RTSPCommand::UNKNOWN;
            rtn = rtspServer->parseCommandLine(line, command);
            if (rtn < 0) {
                // todo
            }
            switch (command) {
            case RTSPCommand::OPTIONS:
                rtspServer->handleCmdOptions(output);
                break;
            default:
                break;
            }
        }

        evbuffer_add(output, line, n);
        evbuffer_add(output, "\n", 1);
        free(line);
    }

    if (evbuffer_get_length(input) >= MAX_LINE) {
        /* Too long; just process what there is and go on so that the buffer
         * doesn't grow infinitely long. */
        char buf[1024];
        while (evbuffer_get_length(input)) {
            int n = evbuffer_remove(input, buf, sizeof(buf));
            for (i = 0; i < n; ++i)
                fprintf(stdout, "%c", line[i]);
            evbuffer_add(output, buf, n);
        }
        evbuffer_add(output, "\n", 1);
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
    struct sockaddr_in* sin = (struct sockaddr_in*) ctx;
    LOG(INFO) << "connection from " << inet_ntoa(sin->sin_addr) << ":" << htons(sin->sin_port) << " closed";
    bufferevent_free(bev);
}

void accept_error_cb(struct evconnlistener *listener, void *ctx)
{
    struct event_base *base = evconnlistener_get_base(listener);
    int err = EVUTIL_SOCKET_ERROR();
    LOG(INFO) << "Shutting down because of got an error on the listener:\n\t"
              << err << ":" << evutil_socket_error_to_string(err);

    event_base_loopexit(base, NULL);
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
    struct bufferevent *bev = bufferevent_socket_new(base, sock, BEV_OPT_CLOSE_ON_FREE);

    bufferevent_setcb(bev, readcb, NULL, errorcb, (void*) ptr);

    bufferevent_enable(bev, EV_READ|EV_WRITE);
}

RTSPServer::RTSPServer(int p): mPort(p) {};

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

int RTSPServer::parseCommandLine(const char* const line, RTSPCommand& command) {
    for (int i = 0; i != strlen(line); ++i) {
        if (line[i] == ' ') continue;
        if ((i + COMMAND_LENGTH) > strlen(line)) return -1;
        if (line[i] == 'O'
            && line[i+1] == 'P'
            && line[i+2] == 'T'
            && line[i+3] == 'I'
            && line[i+4] == 'O'
            && line[i+5] == 'N'
            && line[i+6] == 'S') {
            command = RTSPCommand::OPTIONS;
            return 0;
        }
        return 1;
    }
}

void RTSPServer::handleCmdOptions(struct evbuffer* output)
{
    LOG(INFO) << "receive cmmond OPTIONS";
}
