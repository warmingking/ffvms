#include "network_server.h"

#include <arpa/inet.h>
#include <event2/thread.h>
#include <fcntl.h>
#include <fmt/core.h>
#include <fstream>
#include <glog/logging.h>

using namespace ffvms::core;

static const size_t bufSize = 1024 * 1024;
#define RTP_FLAG_MARKER 0x2

NetworkServer::NetworkServer() {}

NetworkServer::~NetworkServer()
{
    if (mpUDPEvent)
    {
        event_del(mpUDPEvent.get());
    }
    if (mpUDPBase)
    {
        event_base_loopbreak(mpUDPBase.get());
    }
    if (mpUDPEventThreadPool)
    {
        mpUDPEventThreadPool->stop();
    }
    for (int i = 0; i < mpUDPIOThreadPools.size(); i++)
    {
        mpUDPIOThreadPools[i]->stop();
    }
    udpBossThread.join();
    if (mpSocket)
    {
        close(*mpSocket);
    }
}

void NetworkServer::startUDPIOLoop()
{
    if (evthread_use_pthreads() != 0)
    {
        LOG(FATAL) << "evthread use pthreads failed " << strerror(errno);
    }
    mpUDPBase = std::unique_ptr<struct event_base,
                                std::function<void(struct event_base *)>>(
        event_base_new(), [](event_base *eb) { event_base_free(eb); });
    if (!mpUDPBase)
    {
        LOG(FATAL) << "failed to create event base " << strerror(errno);
    }

    mpUDPEvent =
        std::unique_ptr<struct event, std::function<void(struct event *)>>(
            event_new(
                mpUDPBase.get(), *mpSocket, EV_READ | EV_PERSIST,
                [](const int sock, short int which, void *arg) {
                    if (!which & EV_READ)
                    {
                        VLOG(1) << "unknown event " << which << ", do nothing";
                        return;
                    }

                    NetworkServer *server = (NetworkServer *)arg;

                    if (server->mCurPosition + 1600 >= server->mBufferSize)
                    {
                        server->mReceiveBufferIndex ^= 1;
                        server->mCurPosition = 0;
                    }
                    char *data =
                        &server->mpReceiveBuffers[server->mReceiveBufferIndex]
                                                 [server->mCurPosition];

                    struct sockaddr_in server_sin;
                    socklen_t server_size = sizeof(server_sin);

                    /* Recv the data, store the address of the sender in
                     * server_sin
                     */
                    int len =
                        recvfrom(sock, data, 1600, 0,
                                 (struct sockaddr *)&server_sin, &server_size);
                    if (len == -1)
                    {
                        LOG_IF(ERROR, errno != EAGAIN)
                            << "error recv udp packet " << strerror(errno);
                        return;
                    }
                    server->mCurPosition += len;

                    server->mpUDPEventThreadPool->push([=](int threadId) {
                        std::string peer =
                            fmt::format("{}:{}", inet_ntoa(server_sin.sin_addr),
                                        htons(server_sin.sin_port));

                        uint8_t *udata = (uint8_t *)data;
                        LOG_EVERY_N(INFO, 1000) << "[" << threadId << "] data from: " << peer
                                  << ", seq: " << (udata[2] << 8 | udata[3])
                                  << ", len: " << len;
                        /*
                        if (peer == "127.0.0.1:12300")
                        {
                            LOG(INFO) << "[" << threadId << "] data from: " <<
                        peer
                                      << ", seq: " << (udata[2] << 8 | udata[3])
                                      << ", len: " << len;
                            LOG(INFO) << "seq: " << (udata[3] << 8 |
                            udata[4])
                                      << ", len: " << len;
                            LOG_EVERY_N(INFO, 1000)
                                << "dump peer " << peer << " to file
                                tbut_in.rtp";
                            std::ofstream
                            file("/workspaces/ffvms/tbut_in.rtp",
                                               std::ios::binary |
                                               std::ios::app);
                            // 先用 2 位保存 rtp 包的长度, 然后保存 rtp 包
                            char lenHeader[2] = {0, 0};
                            lenHeader[0] = len >> 8;
                            lenHeader[1] = len & 0xFF;
                            file.write(lenHeader, 2);
                            file.write((const char *)data, len);
                        }
                        */

                        size_t idx = std::hash<std::string>{}(peer) %
                                     server->mpUDPIOThreadPools.size();
                        server->mpUDPIOThreadPools[idx]->push(
                            [server, data, len, peer](int id) {
                                VLOG(2) << "new task in thread id " << id;
                                auto &buffer = server->mpReceiveBuffers[id];

                                std::shared_lock _(server->mutex);
                                auto it = server->mRegisteredPeer.find(peer);
                                if (it == server->mRegisteredPeer.end())
                                {
                                    LOG(WARNING)
                                        << "peer " << peer << " not registered";
                                    return;
                                }
                                it->second->processFunc(data, len);
                            });
                    });
                },
                this),
            [](event *e) { event_free(e); });
    event_add(mpUDPEvent.get(), 0);
    event_base_dispatch(mpUDPBase.get());
}

void NetworkServer::initUDPServer(Config config)
{
    int threadNum = config.event_thread_num == -1
                        ? std::thread::hardware_concurrency()
                        : config.event_thread_num;
    for (int i = 0; i < threadNum; i++)
    {
        mpUDPIOThreadPools.emplace_back(std::make_unique<ctpl::thread_pool>(1));
    }
    // 每个线程处理 20 路, 每路带宽 0.5 MB/s, buffer 最大缓存 1s 的数据
    mBufferSize = threadNum * 20 * 1024 * 1024;
    for (int i = 0; i < 2; ++i)
    {
        mpReceiveBuffers.emplace_back(
            std::unique_ptr<char[]>(new char[mBufferSize]));
    }
    mReceiveBufferIndex = 0;

    int workThreadNum = config.work_thread_num == -1
                            ? std::thread::hardware_concurrency()
                            : config.work_thread_num;
    mpUDPEventThreadPool = std::make_unique<ctpl::thread_pool>(workThreadNum);

    mpSocket = std::make_unique<int>();
    *mpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(*mpSocket, F_SETFL, O_NONBLOCK);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(config.port);

    int udp_recv_buf = 1024 * 1024 * 10;
    if (setsockopt(*mpSocket, SOL_SOCKET, SO_RCVBUF, &udp_recv_buf,
                   sizeof(udp_recv_buf)) < 0)
    {
        LOG(FATAL) << "Error setsockopt rcvbuf -> " << strerror(errno);
    }

    if (bind(*mpSocket, (struct sockaddr *)&sin, sizeof(sin)))
    {
        LOG(FATAL) << "couldn't bind udp port " << config.port;
    }

    udpBossThread = std::thread(&NetworkServer::startUDPIOLoop, this);
}

void NetworkServer::registerPeer(const std::string &peer,
                                 ProcessDataFunction &&processDataFunc)
{
    std::unique_lock _(mutex);
    if (mRegisteredPeer.count(peer) == 0)
    {
        LOG(INFO) << "register peer " << peer;
        mRegisteredPeer[peer] =
            std::make_unique<Opaque>(std::move(processDataFunc));
    }
    else
    {
        LOG(WARNING) << "peer " << peer << " already registered";
    }
}

void NetworkServer::unRegisterPeer(const std::string &peer)
{
    std::unique_lock _(mutex);
    mRegisteredPeer.erase(peer);
}
