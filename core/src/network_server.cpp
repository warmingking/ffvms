#include "network_server.h"

#include <arpa/inet.h>
#include <error_check.h>
#include <fcntl.h>
#include <fmt/core.h>
#include <fstream>
#include <glog/logging.h>

using namespace ffvms::core;

static const size_t bufSize = 1024 * 1024;
static const size_t mtu = 1600;

NetworkServer::UdpEventLoop::UdpEventLoop()
    : udpLoop(new uv_loop_t,
              [](uv_loop_t *loop) {
                  uv_loop_close(loop);
                  free(loop);
              }),
      udpHandle(new uv_udp_t, [](uv_udp_t *udp) {
          uv_close(reinterpret_cast<uv_handle_t *>(udp),
                   nullptr /* uv_close_cb */);
          free(udp);
      })
{
}

void NetworkServer::UdpEventLoop::Init(void *server, int port)
{
    int ret = uv_loop_init(udpLoop.get());
    if (ret)
    {
        std::error_code rtn = ErrorFromErrno(errno);
        CHECK_RTN_LOGF(rtn);
    }
    /* 使用 UV_UDP_RECVMMSG 需要给 nv 分配更多内存,
       The use of this feature requires a buffer larger than 2 * 64KB to be
       passed to alloc_cb.
    */
    // ret = uv_udp_init_ex(udpLoop.get(), udpHandle.get(), UV_UDP_RECVMMSG);
    ret = uv_udp_init(udpLoop.get(), udpHandle.get());
    if (ret)
    {
        std::error_code rtn = ErrorFromErrno(errno);
        CHECK_RTN_LOGF(rtn);
    }
    uv_handle_set_data(reinterpret_cast<uv_handle_t *>(udpHandle.get()),
                       server);
    struct sockaddr_in recv_addr;
    ret = uv_ip4_addr("0.0.0.0", port, &recv_addr);
    if (ret)
    {
        std::error_code rtn = ErrorFromErrno(errno);
        CHECK_RTN_LOGF(rtn);
    }
    ret = uv_udp_bind(udpHandle.get(), (const struct sockaddr *)&recv_addr,
                      UV_UDP_REUSEADDR);
    if (ret)
    {
        std::error_code rtn = ErrorFromErrno(errno);
        CHECK_RTN_LOGF(rtn);
    }
    int udpRecvBuf = 1024 * 1024 * 10;
    ret = uv_recv_buffer_size(reinterpret_cast<uv_handle_t *>(udpHandle.get()),
                              &udpRecvBuf);
    if (ret)
    {
        std::error_code rtn = ErrorFromErrno(errno);
        CHECK_RTN_LOGF(rtn);
    }
    ret = uv_udp_recv_start(
        udpHandle.get(),
        [](uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
            NetworkServer *server = static_cast<NetworkServer *>(handle->data);
            {
                std::scoped_lock _(server->mBufferMutex);
                if (server->mCurPosition + mtu >= server->mBufferSize)
                {
                    VLOG(1) << "memory pool " << server->mReceiveBufferIndex
                            << " full, use another one";
                    server->mReceiveBufferIndex ^= 1;
                    server->mCurPosition = 0;
                }
                buf->base =
                    &server->mpReceiveBuffers[server->mReceiveBufferIndex]
                                             [server->mCurPosition];
                server->mCurPosition += mtu;
            }
            buf->len = mtu;
        },
        [](uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
           const struct sockaddr *addr, unsigned flags) {
            // The receive callback will be called with nread == 0 and addr ==
            // NULL when there is nothing to read, and with nread == 0 and addr
            // != NULL when an empty UDP packet is received.
            if (nread == 0)
            {
                if (addr == NULL)
                {
                    VLOG(2) << "udp nothing to read"; /* it's valid */
                }
                else
                {
                    LOG(WARNING) << "recv empty udp packet";
                }
                return;
            }

            NetworkServer *server = static_cast<NetworkServer *>(handle->data);
            const struct sockaddr_in *sin =
                reinterpret_cast<const struct sockaddr_in *>(addr);
            std::string peer = fmt::format("{}:{}", inet_ntoa(sin->sin_addr),
                                           htons(sin->sin_port));
            // 根据发送端的 ip + 端口确定用哪个线程处理,
            // 可以保证同一路一直使用同一个线程
            // TODO: 是否有必要
            size_t idx = std::hash<std::string>{}(peer)
                         % server->mpUdpWorkerThreads.size();
            server->mpUdpWorkerThreads[idx]->submit(
                [server, data(buf->base), nread, peer]() {
                    std::shared_lock _(server->mMutex);
                    auto it = server->mRegisteredPeer.find(peer);
                    if (it == server->mRegisteredPeer.end())
                    {
                        LOG(WARNING) << "peer " << peer << " not registered";
                        return;
                    }
                    it->second->processFunc(data, nread);
                });
        });
    if (ret)
    {
        std::error_code rtn = ErrorFromErrno(errno);
        CHECK_RTN_LOGF(rtn);
    }
}

void NetworkServer::UdpEventLoop::Run()
{
    udpEventLoopThread = std::thread([this]() {
        int ret = uv_run(udpLoop.get(), UV_RUN_DEFAULT);
        LOG_IF(WARNING, ret != 0)
            << "udp loop close, there are " << ret << " handles still alive";
    });
}

NetworkServer::UdpEventLoop::~UdpEventLoop()
{
    udpHandle.reset();
    uv_stop(udpLoop.get());
    udpEventLoopThread.join();
    udpLoop.reset();
}

NetworkServer::NetworkServer() {}

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
    int eventNum = config.event_loop_num == -1
                       ? std::thread::hardware_concurrency()
                       : config.event_loop_num;
    for (int i = 0; i < eventNum; i++)
    {
        startUdpEventLoop(this, config.port);
    }
    // 每个线程处理 20 路, 每路带宽 0.5 MB/s, buffer 最大缓存 1M 的数据
    mBufferSize = eventNum * 20 * 1024 * 1024;
    // size == 2, udp 的收流 buffer, 交替使用
    for (int i = 0; i < 2; ++i)
    {
        mpReceiveBuffers.emplace_back(
            std::unique_ptr<char[]>(new char[mBufferSize]));
    }

    int workerNum = config.async_worker_num == -1
                        ? std::thread::hardware_concurrency()
                        : config.async_worker_num;
    for (int i = 0; i < workerNum; i++)
    {
        mpUdpWorkerThreads.emplace_back(
            std::make_unique<common::ThreadPool>(1));
    }
    mReceiveBufferIndex = 0;
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
