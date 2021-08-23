#ifndef __NETWORK_SERVER_H__
#define __NETWORK_SERVER_H__

#include <event2/event.h>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <shared_mutex>
#include <thread_pool.h>

namespace ffvms
{
namespace core
{

using ProcessDataFunction = std::function<void(const char *data, const size_t len)>;

class NetworkServer
{
public:
    struct Config
    {
        int port;
        int event_loop_num;
        int async_worker_num;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Config, port, event_loop_num, async_worker_num)
    };

public:
    NetworkServer();
    virtual ~NetworkServer();

    void initUdpServer(Config config);
    void registerPeer(const std::string &peer, ProcessDataFunction &&processDataFunc);
    void unRegisterPeer(const std::string &peer);

private:
    struct Peer
    {
        std::string name;
        ProcessDataFunction processFunc;

        Peer(const std::string &name, ProcessDataFunction &&processFunc)
            : name(name), processFunc(processFunc)
        {
        }
    };

    struct UdpEventLoop
    {
        int socket;
        std::unique_ptr<struct event_base, std::function<void(struct event_base *)>> pUdpBase;
        std::unique_ptr<struct event, std::function<void(struct event *)>> pUdpEvent;
        std::thread udpEventLoopThread;

        UdpEventLoop() = default;
        ~UdpEventLoop();

        void Init(void *server, int port);
        void Run();
    };

private:
    std::vector<std::unique_ptr<UdpEventLoop>> mpUdpWorkers;
    std::vector<std::unique_ptr<common::ThreadPool>> mpUdpWorkerThreads;
    // size == 2, udp 的收流 buffer, 交替使用
    std::mutex mBufferMutex;
    std::vector<std::unique_ptr<char[]>> mpReceiveBuffers;
    int mBufferSize;
    int mReceiveBufferIndex; // 0 or 1
    int mCurPosition;        // current position in receive buffer
    std::shared_mutex mMutex;
    std::map<std::string, std::unique_ptr<Peer>> mRegisteredPeers;
    void startUdpEventLoop(void *server, int port);
};
} // namespace core
} // namespace ffvms

#endif // __NETWORK_SERVER_H__