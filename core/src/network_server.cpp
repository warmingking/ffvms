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

void NetworkServer::UdpWorker::Init(void *server, int port)
{
    socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(socket, F_SETFL, O_NONBLOCK);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    int udpRecvBuf = 1024 * 1024 * 10;
    if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &udpRecvBuf,
                   sizeof(udpRecvBuf)) < 0)
    {
        LOG(FATAL) << "Error setsockopt rcvbuf -> " << strerror(errno);
    }

    int reusePort = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &reusePort,
                   sizeof(reusePort)) < 0)
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
    pUdpBase = std::unique_ptr<struct event_base,
                               std::function<void(struct event_base *)>>(
        event_base_new(), [](event_base *eb) { event_base_free(eb); });
    if (!pUdpBase)
    {
        LOG(FATAL) << "failed to create event base " << strerror(errno);
    }

    pUdpEvent =
        std::unique_ptr<struct event, std::function<void(struct event *)>>(
            event_new(
                pUdpBase.get(), socket, EV_READ | EV_PERSIST,
                [](const int sock, short int which, void *arg) {
                    if (!which & EV_READ)
                    {
                        VLOG(1) << "unknown event " << which << ", do nothing";
                        return;
                    }

                    NetworkServer *server = (NetworkServer *)arg;

                    if (server->mCurPosition + 1600 >= server->mBufferSize)
                    {
                        LOG(INFO) << "==================";
                        server->mReceiveBufferIndex ^= 1;
                        server->mCurPosition = 0;
                    }
                    char *data =
                        &server->mpReceiveBuffers[server->mReceiveBufferIndex]
                                                 [server->mCurPosition];

                    // struct sockaddr_in server_sin;
                    // socklen_t server_size = sizeof(server_sin);
                    // memset(&server_sin, 0, sizeof(server_sin));

                    struct sockaddr_storage server_sin;
                    socklen_t server_size = sizeof(struct sockaddr_storage);

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

                    char host[NI_MAXHOST], service[NI_MAXSERV];
                    int s = getnameinfo((struct sockaddr *)&server_sin,
                                        server_size, host, NI_MAXHOST,
                                        service, NI_MAXSERV, NI_NUMERICSERV);
                    std::string peer = fmt::format("{}:{}", host, service);

                    // std::string peer =
                    //     fmt::format("{}:{}", inet_ntoa(server_sin.sin_addr),
                    //                 htons(server_sin.sin_port));

                    uint8_t *udata = (uint8_t *)data;
                    if (peer == "localhost:12300")
                    {
                        LOG_EVERY_N(INFO, 1)
                            << "seq: " << (udata[2] << 8 | udata[3])
                            << ", len: " << len << ", data: "
                            << fmt::format("{0:#x}",
                                           uint32_t(udata[0] << 24 |
                                                    udata[1] << 16 |
                                                    udata[2] << 8 | udata[3]))
                            << " "
                            << fmt::format("{0:#x}",
                                           uint32_t(udata[4] << 24 |
                                                    udata[5] << 16 |
                                                    udata[6] << 8 | udata[7]))
                            << " "
                            << fmt::format("{0:#x}",
                                           uint32_t(udata[8] << 24 |
                                                    udata[9] << 16 |
                                                    udata[10] << 8 | udata[11]))
                            << " "
                            << fmt::format(
                                   "{0:#x}",
                                   uint32_t(udata[12] << 24 | udata[13] << 16 |
                                            udata[14] << 8 | udata[15]));
                        // LOG_EVERY_N(INFO, 1000)
                        //     << "dump peer " << peer << " to file tbut_in.rtp
                        //     ";
                        // std::ofstream file("/workspaces/ffvms/tbut_in.rtp",
                        //                    std::ios::binary | std::ios::app);
                        // // 先用 2 位保存 rtp 包的长度, 然后保存 rtp 包
                        // char lenHeader[2] = {0, 0};
                        // lenHeader[0] = len >> 8;
                        // lenHeader[1] = len & 0xFF;
                        // file.write(lenHeader, 2);
                        // file.write((const char *)data, len);
                    }

                    size_t idx = std::hash<std::string>{}(peer) %
                                 server->mpUdpIOThreadPools.size();
                    server->mpUdpIOThreadPools[idx]->push(
                        [server, data, len, peer](int id) {
                            VLOG(2) << "new task in thread id " << id;
                            auto &buffer = server->mpReceiveBuffers[id];

                            std::shared_lock _(server->mMutex);
                            auto it = server->mRegisteredPeer.find(peer);
                            if (it == server->mRegisteredPeer.end())
                            {
                                LOG(WARNING)
                                    << "peer " << peer << " not registered";
                                return;
                            }
                            it->second->processFunc(data, len);
                        });
                },
                server),
            [](event *e) { event_free(e); });
    event_add(pUdpEvent.get(), 0);
}

void NetworkServer::UdpWorker::Run()
{
    udpEventLoopThread =
        std::thread([this]() { event_base_dispatch(pUdpBase.get()); });
}

NetworkServer::UdpWorker::~UdpWorker()
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

NetworkServer::NetworkServer() {}

NetworkServer::~NetworkServer()
{
    for (int i = 0; i < mpUdpIOThreadPools.size(); i++)
    {
        mpUdpIOThreadPools[i]->stop();
    }
}

void NetworkServer::startUdpWorker(void *server, int port)
{
    auto pUdpWorker = std::make_unique<UdpWorker>();
    pUdpWorker->Init(server, port);
    mpUdpWorkers.emplace_back(std::move(pUdpWorker));
    mpUdpWorkers.back()->Run();
}

void NetworkServer::initUdpServer(Config config)
{
    int threadNum = config.event_thread_num == -1
                        ? std::thread::hardware_concurrency()
                        : config.event_thread_num;
    for (int i = 0; i < threadNum; i++)
    {
        mpUdpIOThreadPools.emplace_back(std::make_unique<ctpl::thread_pool>(1));
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

    for (int i = 0; i < workThreadNum; i++)
    {
        startUdpWorker(this, config.port);
    }
}

void NetworkServer::registerPeer(const std::string &peer,
                                 ProcessDataFunction &&processDataFunc)
{
    std::unique_lock _(mMutex);
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
    std::unique_lock _(mMutex);
    mRegisteredPeer.erase(peer);
}
