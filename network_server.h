#ifndef __NETWORK_SERVER_H__
#define __NETWORK_SERVER_H__

#include <ctpl/ctpl_stl.h>

#include <map>
#include <queue>
#include <shared_mutex>

class NetworkServer {
   private:
    struct RTPPacket {
        static const size_t bufSize = 1600;
        unsigned char* buf;
        size_t bufLen;
        uint64_t seq;
        uint64_t increasingSeq;
        size_t dataIdx;
        size_t dataLen;

        RTPPacket() { buf = new unsigned char[bufSize]; }

        ~RTPPacket() { delete[] buf; }
    };

    struct RAIIRTPPacket {
        std::shared_ptr<RTPPacket> pPacket;
        bool operator<(const RAIIRTPPacket& other) const { return this->pPacket->increasingSeq > other.pPacket->increasingSeq; }

        RAIIRTPPacket() { pPacket = std::make_shared<RTPPacket>(); }
    };

    struct RTPData {
        static const size_t cacheSize = 10000; // 每路最多缓存 5000 个包
        static const size_t tolerant = 20;
        // static const size_t dataSize = 4 * 1024 * 1024;  // 1s 的数据
        std::mutex mutex;
        std::priority_queue<RAIIRTPPacket> packetCacheQueue;
        uint64_t currentSeq;  // use increasingSeq
        bool isProbeFinish;

        RTPData() : currentSeq(0), isProbeFinish(false) {}
    };

   private:
    std::thread udpBossThread;
    ctpl::thread_pool* mpUDPIOThreadPool;
    std::shared_mutex mutex;
    std::map<std::string, std::shared_ptr<RTPData>> mPeerRTPMap;
    void startUDPIOLoop(int sock);

   public:
    void initUDPServer(uint16_t port);
    void registerPeer(const std::string& peer);
    void probeFinish(const std::string& peer);
    void unRisterPeer(const std::string& peer);
    void readData(const std::string& peer, const size_t& limit, unsigned char* data, size_t& len);

    void parsePacket(RAIIRTPPacket& packet);
    // friend void udp_cb(const int sock, short int which, void *arg);
};

#endif  // __NETWORK_SERVER_H__
