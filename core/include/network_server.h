#ifndef __NETWORK_SERVER_H__
#define __NETWORK_SERVER_H__

#include <ctpl/ctpl.h>
#include <event2/event.h>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <shared_mutex>

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
        int event_thread_num;
        int work_thread_num;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Config, port, event_thread_num, work_thread_num)
    };

public:
    NetworkServer();
    virtual ~NetworkServer();

    void initUDPServer(Config config);
    void registerPeer(const std::string &peer,
                      ProcessDataFunction &&processDataFunc);
    void unRegisterPeer(const std::string &peer);

private:
    struct Opaque
    {
        ProcessDataFunction processFunc;

        Opaque(ProcessDataFunction &&processFunc) : processFunc(processFunc) {}
    };

private:
    std::unique_ptr<int> mpSocket;
    std::unique_ptr<struct event_base, std::function<void(struct event_base *)>>
        mpUDPBase;
    std::unique_ptr<struct event, std::function<void(struct event *)>>
        mpUDPEvent;
    std::thread udpBossThread;
    std::unique_ptr<ctpl::thread_pool> mpUDPEventThreadPool;
    std::vector<std::unique_ptr<ctpl::thread_pool>> mpUDPIOThreadPools;
    // size == 2, UDP 的收流 buffer, 交替使用
    std::vector<std::unique_ptr<char[]>> mpReceiveBuffers;
    int mBufferSize;
    int mReceiveBufferIndex; // 0 or 1
    int mCurPosition;        // current position in receive buffer
    std::shared_mutex mutex;
    std::map<std::string, std::unique_ptr<Opaque>> mRegisteredPeer;
    void startUDPIOLoop();
};
} // namespace core
} // namespace ffvms

#endif // __NETWORK_SERVER_H__
