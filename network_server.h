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

        bool operator<(const RTPPacket& other) const { return this->increasingSeq < other.increasingSeq; }
    };

    struct RAIIRTPPacket {
        std::shared_ptr<RTPPacket> pPacket;
        bool operator<(const RAIIRTPPacket& other) const { return this->pPacket > other.pPacket; }

        RAIIRTPPacket() { pPacket = std::make_shared<RTPPacket>(); }
    };

    // template <class T>
    // struct CycleList {
    //     std::mutex mutex;  // this is thread safe
    //     size_t perimeter;
    //     size_t readCursor;
    //     size_t dataLen;
    //     T* data;

    //     void appendData(const T* data, const size_t len);
    //     void getAllData(T* data, size_t& len);

    //     CycleList() = delete;
    //     CycleList(const size_t len) : perimeter(len) { data = new T[len]; }
    //     ~CycleList() { delete[] data; }
    // };

    struct RTPData {
        static const size_t cacheSize = 1000;
        static const size_t tolerant = 20;
        // static const size_t dataSize = 4 * 1024 * 1024;  // 1s 的数据
        std::mutex mutex;
        std::priority_queue<RAIIRTPPacket> packetCacheQueue;
        uint64_t currentSeq;  // use increasingSeq
        bool isProbeFinish;
        // std::shared_ptr<CycleList<unsigned char>> data;  // cycle cache

        RTPData() : currentSeq(0), isProbeFinish(false) {}
        // RTPData() : currentSeq(0) { data = std::make_shared<CycleList<unsigned char>>(dataSize); }
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
