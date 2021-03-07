#include "network_server.h"

#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/thread.h>
#include <glog/logging.h>
#include <libavutil/intreadwrite.h>

static const size_t bufSize = 1024 * 1024;
#define RTP_FLAG_MARKER 0x2

void NetworkServer::parsePacket(NetworkServer::RAIIRTPPacket &packet)
{
    packet.pPacket->dataIdx = 0;
    packet.pPacket->dataLen = packet.pPacket->bufLen;

    unsigned int ssrc;
    int payload_type, seq, flags = 0;
    int ext, csrc;
    uint32_t timestamp;
    int rv = 0;

    csrc = packet.pPacket->buf[0] & 0x0f;
    ext = packet.pPacket->buf[0] & 0x10;
    payload_type = packet.pPacket->buf[1] & 0x7f;
    if (packet.pPacket->buf[1] & 0x80)
    {
        flags |= RTP_FLAG_MARKER;
    }
    packet.pPacket->increasingSeq = packet.pPacket->seq = AV_RB16(packet.pPacket->buf + 2);
    timestamp = AV_RB32(packet.pPacket->buf + 4);
    ssrc = AV_RB32(packet.pPacket->buf + 8);

    if (packet.pPacket->buf[0] & 0x20)
    {
        int padding = packet.pPacket->buf[packet.pPacket->bufLen - 1];
        if (packet.pPacket->bufLen >= 12 + padding)
        {
            packet.pPacket->dataLen -= padding;
        }
    }

    packet.pPacket->dataLen -= 12;
    packet.pPacket->dataIdx += 12;

    packet.pPacket->dataLen -= 4 * csrc;
    packet.pPacket->dataIdx += 4 * csrc;
    if (packet.pPacket->dataLen < 0)
    {
        LOG(ERROR) << "invailed rtp data, len < 0";
        packet.pPacket->dataLen = 0; // adhoc: treat as empty packet
        return;
    }

    /* RFC 3550 Section 5.3.1 RTP Header Extension handling */
    if (ext)
    {
        if (packet.pPacket->dataLen < 4)
        {
            packet.pPacket->dataLen = 0; // adhoc: treat as empty packet
            return;
        }
        /* calculate the header extension length (stored as number
         * of 32-bit words) */
        ext = (AV_RB16(packet.pPacket->buf + 2) + 1) << 2;

        if (packet.pPacket->dataLen < ext)
        {
            packet.pPacket->dataLen = 0; // adhoc: treat as empty packet
            return;
        }
        // skip past RTP header extension
        packet.pPacket->dataLen -= ext;
        packet.pPacket->dataIdx += ext;
    }
}

void NetworkServer::startUDPIOLoop(int sock)
{
    struct event_base *base = event_base_new();
    if (!base)
    {
        LOG(FATAL) << "failed to create event base";
    }

    struct event *udp_event = event_new(
        base, sock, EV_READ | EV_PERSIST,
        [](const int sock, short int which, void *arg) {
            NetworkServer *server = (NetworkServer *)arg;

            server->mpUDPIOThreadPool->push([server, sock](int id) {
                struct sockaddr_in server_sin;
                socklen_t server_sz = sizeof(server_sin);

                NetworkServer::RAIIRTPPacket packet;
                /* Recv the data, store the address of the sender in server_sin */
                packet.pPacket->bufLen = recvfrom(sock, packet.pPacket->buf, NetworkServer::RTPPacket::bufSize, 0,
                                                  (struct sockaddr *)&server_sin, &server_sz);
                if (packet.pPacket->bufLen == -1)
                {
                    LOG(ERROR) << "error recv udp packet " << strerror(errno);
                    return;
                }
                char *peer = new char[128 + 1 + 5 + 1]; // ip + ":" + port(itoa) + "\0"
                char *ip = inet_ntoa(server_sin.sin_addr);
                strcpy(peer, ip);
                peer[strlen(ip)] = ':';
                sprintf(&peer[strlen(ip) + 1], "%d", htons(server_sin.sin_port));
                std::shared_lock _(server->mutex);
                auto it = server->mPeerRTPMap.find(peer);
                if (it == server->mPeerRTPMap.end())
                {
                    VLOG(1) << "peer " << peer << " not registered";
                    return;
                }
                server->parsePacket(packet);
                if (it->second->currentSeq != 0)
                {
                    uint64_t seqModed = it->second->currentSeq % std::numeric_limits<uint16_t>::max();
                    // LOG(INFO) << "xxx seq " << seqModed << " vs " << packet.pPacket->seq;
                    if (packet.pPacket->seq < seqModed)
                    {
                        if (seqModed - packet.pPacket->seq > std::numeric_limits<int16_t>::max())
                        {
                            while (packet.pPacket->increasingSeq < it->second->currentSeq)
                            {
                                packet.pPacket->increasingSeq += std::numeric_limits<uint16_t>::max();
                            }
                        }
                        else
                        {
                            LOG(WARNING) << "rtp packet reach delayed " << seqModed - packet.pPacket->seq << " packets";
                            return;
                        }
                    }
                }
                else
                {
                    it->second->currentSeq = packet.pPacket->seq;
                }
                std::scoped_lock queueLock(it->second->mutex);
                if (it->second->packetCacheQueue.size() == NetworkServer::RTPData::cacheSize)
                {
                    LOG(ERROR) << "cache full";
                    return;
                }
                it->second->packetCacheQueue.push((packet)); // TODO: std::move
            });
        },
        this);
    event_add(udp_event, 0);
    event_base_dispatch(base);
}

void NetworkServer::initUDPServer(uint16_t port)
{
    // mpUDPIOThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpUDPIOThreadPool = new ctpl::thread_pool(1);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)))
    {
        LOG(FATAL) << "couldn't bind udp port " << port;
    }

    // for (int i = 0; i != mpUDPIOThreadPool->size(); ++i)
    // {
    //     mpUDPIOThreadPool->push([this, sock](int id) { startUDPIOLoop(sock); });
    // }
    udpBossThread = std::thread(&NetworkServer::startUDPIOLoop, this, sock);
}

void NetworkServer::registerPeer(const std::string &peer)
{
    std::unique_lock _(mutex);
    if (mPeerRTPMap.count(peer) == 0)
    {
        mPeerRTPMap[peer] = std::make_shared<RTPData>();
    } // TODO: else
}

void NetworkServer::probeFinish(const std::string &peer)
{
    std::scoped_lock _(mutex);
    auto it = mPeerRTPMap.find(peer);
    if (it == mPeerRTPMap.end())
    {
        LOG(WARNING) << "peer " << peer << " not registered";
        return;
    }
    it->second->isProbeFinish = true;
}

void NetworkServer::unRisterPeer(const std::string &peer)
{
    std::unique_lock _(mutex);
    mPeerRTPMap.erase(peer);
}

void little_sleep(std::chrono::microseconds us)
{
    auto start = std::chrono::high_resolution_clock::now();
    auto end = start + us;
    do
    {
        // std::this_thread::yield();
        usleep(1000);
    } while (std::chrono::high_resolution_clock::now() < end);
}

void NetworkServer::readData(const std::string &peer, const size_t &limit, unsigned char *data, size_t &len)
{
    len = 0;
    std::shared_lock _(mutex);
    auto it = mPeerRTPMap.find(peer);
    if (it == mPeerRTPMap.end())
    {
        LOG(ERROR) << "peer " << peer << " not registered";
        return;
    }
    size_t queueSize = 0;
    while (!it->second->isProbeFinish && queueSize < 1)
    {
        {
            std::scoped_lock rtpLoak(it->second->mutex);
            queueSize = it->second->packetCacheQueue.size();
        }
        little_sleep(std::chrono::microseconds(1000));
        // usleep(10000);
    }

    {
        std::scoped_lock rtpLoak(it->second->mutex);
        while ((!it->second->packetCacheQueue.empty()) && (len < limit - 1500) &&                          // TODO: max len
               (it->second->packetCacheQueue.top().pPacket->increasingSeq - it->second->currentSeq <= 1 || // 没有间隔
                it->second->packetCacheQueue.top().pPacket->increasingSeq - it->second->currentSeq >       // 超过最大容差限制
                    NetworkServer::RTPData::tolerant ||
                (it->second->packetCacheQueue.size() > 20)))
        {
            std::copy(&it->second->packetCacheQueue.top().pPacket->buf[it->second->packetCacheQueue.top().pPacket->dataIdx],
                      &it->second->packetCacheQueue.top().pPacket->buf[it->second->packetCacheQueue.top().pPacket->dataIdx +
                                                                       it->second->packetCacheQueue.top().pPacket->dataLen],
                      &data[len]);
            len += it->second->packetCacheQueue.top().pPacket->dataLen;
            it->second->currentSeq = it->second->packetCacheQueue.top().pPacket->increasingSeq;
            it->second->packetCacheQueue.pop();
        }
    }
}

// template <class T>
// void NetworkServer::CycleList<T>::appendData(const T *data, const size_t len) {
//     std::scoped_lock _(mutex);
//     if (dataLen + len > perimeter) {
//         LOG(ERROR) << "capacity not enough, not append";
//         return;
//     }
//     dataLen += len;
//     size_t writeCursor = (readCursor + dataLen) % perimeter;
//     if (writeCursor + len <= perimeter) {
//         std::copy(data, data[len], this->data[writeCursor]);
//     } else {
//         size_t partOne = perimeter - writeCursor;
//         std::copy(data, data[partOne], this->data[writeCursor]);
//         std::copy(data[partOne], data[len], this->data);
//     }
// }

// template <class T>
// void NetworkServer::CycleList<T>::getAllData(T *data, size_t &len) {
//     std::scoped_lock _(mutex);
//     len = dataLen;
//     if (readCursor + dataLen < perimeter) {
//         std::copy(this->data[readCursor], this->data[readCursor + dataLen], data);
//     } else {
//         size_t partOne = perimeter - readCursor;
//         std::copy(this->data[readCursor], this->data[perimeter], data);
//         std::copy(this->data, this->data[dataLen - partOne], data[partOne]);
//     }
// }
