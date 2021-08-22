#ifndef __NETWORK_SERVER_H__
#define __NETWORK_SERVER_H__

#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <shared_mutex>
#include <thread_pool.h>
#include <utils.hpp>
#include <uv.h>

namespace ffvms
{
namespace core
{

using ProcessDataFunction =
    std::function<void(const char *data, const size_t len)>;

class NetworkServer
{
public:
    struct Config
    {
        int port;
        int event_loop_num;
        int async_worker_num;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Config, port, event_loop_num,
                                       async_worker_num)
    };

public:
    NetworkServer();
    virtual ~NetworkServer();

    void initUdpServer(Config config);
    void registerPeer(const std::string &peer,
                      ProcessDataFunction &&processDataFunc);
    void unRegisterPeer(const std::string &peer);

private:
    struct Opaque
    {
        ProcessDataFunction processFunc;

        Opaque(ProcessDataFunction &&processFunc) : processFunc(processFunc) {}
    };

    struct UdpEventLoop
    {
        std::unique_ptr<uv_loop_t, std::function<void(uv_loop_t *)>> udpLoop;
        std::unique_ptr<uv_udp_t, std::function<void(uv_udp_t *)>> udpHandle;

        std::thread udpEventLoopThread;

        UdpEventLoop();
        ~UdpEventLoop();

        void Init(void *server, int port);
        void Run();
    };

private:
    std::vector<std::unique_ptr<UdpEventLoop>> mpUdpWorkers;
    std::vector<std::unique_ptr<common::ThreadPool>> mpUdpWorkerThreads;
    std::mutex mBufferMutex;
    // size == 2, udp 的收流 buffer, 交替使用
    std::vector<std::unique_ptr<char[]>> mpReceiveBuffers;
    int mBufferSize;
    int mReceiveBufferIndex; // 0 or 1
    int mCurPosition;        // current position in receive buffer
    std::shared_mutex mMutex;
    std::map<std::string, std::unique_ptr<Opaque>> mRegisteredPeer;
    void startUdpEventLoop(void *server, int port);
};

} // namespace core
} // namespace ffvms

#endif // __NETWORK_SERVER_H__
