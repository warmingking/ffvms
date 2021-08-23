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

void NetworkServer::UdpEventLoop::Init(void *server, int port)
{
    socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(socket, F_SETFL, O_NONBLOCK);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    int udpRecvBuf = 1024 * 1024 * 10;
    if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &udpRecvBuf, sizeof(udpRecvBuf)) < 0)
    {
        LOG(FATAL) << "Error setsockopt rcvbuf -> " << strerror(errno);
    }

    int reusePort = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &reusePort, sizeof(reusePort)) < 0)
    {
        LOG(FATAL) << "Error setsockopt reuseport -> " << strerror(errno);
    }

    if (bind(socket, (struct sockaddr *)&sin, sizeof(sin)))
    {
        LOG(FATAL) << "couldn't bind udp port " << port;
    }

    if (evthread_use_pthreads() != 0)
    {
        LOG(FATAL) << "evthread use pthreads failed " << strerror(errno);
    }
    pUdpBase = std::unique_ptr<struct event_base, std::function<void(struct event_base *)>>(
        event_base_new(), [](event_base *eb) { event_base_free(eb); });
    if (!pUdpBase)
    {
        LOG(FATAL) << "failed to create event base " << strerror(errno);
    }

    pUdpEvent = std::unique_ptr<struct event, std::function<void(struct event *)>>(
        event_new(
            pUdpBase.get(), socket, EV_READ | EV_PERSIST | EV_ET,
            [](const int sock, short int which, void *arg) {
                if (!which & EV_READ)
                {
                    VLOG(1) << "unknown event " << which << ", do nothing";
                    return;
                }

                NetworkServer *server = (NetworkServer *)arg;

                int len = -1;
                while (true)
                {
                    char *data;
                    {
                        std::scoped_lock _(server->mBufferMutex);
                        if (server->mCurPosition + 1600 >= server->mBufferSize)
                        {
                            VLOG(1) << "memory pool " << server->mReceiveBufferIndex
                                    << " full, use another one";
                            server->mReceiveBufferIndex ^= 1;
                            server->mCurPosition = 0;
                        }
                        data = &server->mpReceiveBuffers[server->mReceiveBufferIndex]
                                                        [server->mCurPosition];
                        server->mCurPosition += 1600;
                    }

                    struct sockaddr server_sin;
                    socklen_t server_size = sizeof(struct sockaddr);

                    //  Recv the data, store the address of the sender inserver_sin
                    int len = recvfrom(sock, data, 1600, 0, &server_sin, &server_size);

                    if (len == -1)
                    {
                        LOG_IF(ERROR, errno != EAGAIN)
                            << "error recv udp packet " << strerror(errno);
                        return;
                    }

                    struct sockaddr_in *addr = reinterpret_cast<struct sockaddr_in *>(&server_sin);
                    std::string peer =
                        fmt::format("{}:{}", inet_ntoa(addr->sin_addr), htons(addr->sin_port));

                    // uint8_t *udata = (uint8_t *)data;
                    // if (peer == "localhost:12300")
                    // {
                    //     LOG_EVERY_N(INFO, 1)
                    //         << "seq: " << (udata[2] << 8 | udata[3])
                    //         << ", len: " << len << ", data: "
                    //         << fmt::format("{0:#x}",
                    //                        uint32_t(udata[0] << 24 |
                    //                                 udata[1] << 16 |
                    //                                 udata[2] << 8 |
                    //                                 udata[3]))
                    //         << " "
                    //         << fmt::format("{0:#x}",
                    //                        uint32_t(udata[4] << 24 |
                    //                                 udata[5] << 16 |
                    //                                 udata[6] << 8 |
                    //                                 udata[7]))
                    //         << " "
                    //         << fmt::format("{0:#x}",
                    //                        uint32_t(udata[8] << 24 |
                    //                                 udata[9] << 16 |
                    //                                 udata[10] << 8 |
                    //                                 udata[11]))
                    //         << " "
                    //         << fmt::format(
                    //                "{0:#x}",
                    //                uint32_t(udata[12] << 24 | udata[13] << 16
                    //                |
                    //                         udata[14] << 8 | udata[15]));
                    //     // LOG_EVERY_N(INFO, 1000)
                    //     //     << "dump peer " << peer << " to file
                    //     tbut_in.rtp
                    //     //     ";
                    //     // std::ofstream
                    //     file("/workspaces/ffvms/tbut_in.rtp",
                    //     //                    std::ios::binary |
                    //     std::ios::app);
                    //     // // 先用 2 位保存 rtp 包的长度, 然后保存 rtp 包
                    //     // char lenHeader[2] = {0, 0};
                    //     // lenHeader[0] = len >> 8;
                    //     // lenHeader[1] = len & 0xFF;
                    //     // file.write(lenHeader, 2);
                    //     // file.write((const char *)data, len);
                    // }

                    // 这个线程池的 size 可以是 0, 表示同步进行 remux
                    // 理论上这样性能更优
                    // TODO: 观察 recv-q
                    if (server->mpUdpWorkerThreads.empty())
                    {
                        std::shared_lock _(server->mMutex);
                        auto it = server->mRegisteredPeers.find(peer);
                        if (it == server->mRegisteredPeers.end())
                        {
                            LOG(WARNING) << "peer " << peer << " not registered";
                            return;
                        }
                        it->second->processFunc(data, len);
                    }
                    else
                    {
                        size_t idx =
                            std::hash<std::string>{}(peer) % server->mpUdpWorkerThreads.size();
                        server->mpUdpWorkerThreads[idx]->submit([server, data, len, peer]() {
                            std::shared_lock _(server->mMutex);
                            auto it = server->mRegisteredPeers.find(peer);
                            if (it == server->mRegisteredPeers.end())
                            {
                                LOG(WARNING) << "peer " << peer << " not registered";
                                return;
                            }
                            it->second->processFunc(data, len);
                        });
                    }
                }
            },
            server),
        [](event *e) { event_free(e); });
    event_add(pUdpEvent.get(), 0);
}

void NetworkServer::UdpEventLoop::Run()
{
    udpEventLoopThread = std::thread([this]() { event_base_dispatch(pUdpBase.get()); });
}

NetworkServer::UdpEventLoop::~UdpEventLoop()
{
    if (pUdpEvent)
    {
        event_del(pUdpEvent.get());
    }
    if (pUdpBase)
    {
        event_base_loopbreak(pUdpBase.get());
    }

    udpEventLoopThread.join();
    close(socket);
}

NetworkServer::NetworkServer() : mReceiveBufferIndex(0), mCurPosition(0) {}

NetworkServer::~NetworkServer()
{
    for (int i = 0; i < mpUdpWorkerThreads.size(); i++)
    {
        // TODO: stop thread pool
    }
}

void NetworkServer::startUdpEventLoop(void *server, int port)
{
    auto pUdpWorker = std::make_unique<UdpEventLoop>();
    pUdpWorker->Init(server, port);
    mpUdpWorkers.emplace_back(std::move(pUdpWorker));
    mpUdpWorkers.back()->Run();
}

void NetworkServer::initUdpServer(Config config)
{
    int eventNum =
        config.event_loop_num == -1 ? std::thread::hardware_concurrency() : config.event_loop_num;
    for (int i = 0; i < eventNum; i++)
    {
        startUdpEventLoop(this, config.port);
    }
    // 每个线程处理 20 路, 每路缓存 1M ( 约 2s )
    mBufferSize = eventNum * 20 * 1024 * 1024;
    // size == 2, udp 的收流 buffer, 交替使用
    for (int i = 0; i < 2; ++i)
    {
        mpReceiveBuffers.emplace_back(std::unique_ptr<char[]>(new char[mBufferSize]));
    }
    int workerNum = config.async_worker_num == -1 ? std::thread::hardware_concurrency()
                                                  : config.async_worker_num;
    for (int i = 0; i < workerNum; i++)
    {
        mpUdpWorkerThreads.emplace_back(std::make_unique<common::ThreadPool>(1));
    }
}

void NetworkServer::registerPeer(const std::string &peer, ProcessDataFunction &&processDataFunc)
{
    std::unique_lock _(mMutex);
    if (mRegisteredPeers.count(peer) == 0)
    {
        LOG(INFO) << "register peer " << peer;
        mRegisteredPeers[peer] = std::make_unique<Peer>(peer, std::move(processDataFunc));
    }
    else
    {
        LOG(WARNING) << "peer " << peer << " already registered";
    }
}

void NetworkServer::unRegisterPeer(const std::string &peer)
{
    std::unique_lock _(mMutex);
    mRegisteredPeers.erase(peer);
}