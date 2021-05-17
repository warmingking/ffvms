#include "network_server.h"

#include <arpa/inet.h>
#include <event2/thread.h>
#include <fcntl.h>
#include <fmt/core.h>
#include <fstream>
#include <glog/logging.h>

#include "ethernet.h"
#include <fstream>
#include <pcap.h>

using namespace ffvms::core;

static const size_t bufSize = 1024 * 1024;
#define RTP_FLAG_MARKER 0x2

void NetworkServer::UdpWorker::Init(void *server, int port) {}

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
    mEthThDumpread = std::thread([this, port]() {
        u_char *user = (u_char *)&port;
        mEthThMap[user] = this;
        pcap_t *handle; /* Session handle */
        pcap_if *pcap;
        char *dev;                     /* The device to sniff on */
        char errbuf[PCAP_ERRBUF_SIZE]; /* Error string */
        struct bpf_program fp;         /* The compiled filter */
        std::string filter_exp =
            fmt::format("udp port {}", port); /* The filter expression */
        bpf_u_int32 mask;                     /* Our netmask */
        bpf_u_int32 net;                      /* Our IP */
        struct pcap_pkthdr header; /* The header that pcap gives us */
        const u_char *packet;      /* The actual packet */

        /* Define the device */
        pcap_findalldevs(&pcap, errbuf);
        if (pcap == NULL)
        {
            LOG(FATAL) << "could not find default device, " << errbuf;
        }
        // dev = pcap->name;
        dev = "lo";
        /* Find the properties for the device */
        if (pcap_lookupnet(dev, &net, &mask, errbuf) == -1)
        {
            LOG(FATAL) << "could not get netmask for device " << dev << ", "
                       << errbuf;
        }
        /* Open the session in promiscuous mode */
        handle = pcap_open_live(dev, BUFSIZ, 1, 1000, errbuf);
        if (handle == NULL)
        {
            LOG(FATAL) << "could not open device " << dev << ", " << errbuf;
        }
        /* Compile and apply the filter */
        if (pcap_compile(handle, &fp, filter_exp.c_str(), 0, net) == -1)
        {
            LOG(FATAL) << "could not parse filter " << filter_exp << ", "
                       << pcap_geterr(handle);
        }
        if (pcap_setfilter(handle, &fp) == -1)
        {
            LOG(FATAL) << "could not install filter " << filter_exp
                       << pcap_geterr(handle);
        }
        /* pcap loop */
        if (pcap_loop(
                handle, -1,
                [](u_char *args, const struct pcap_pkthdr *header,
                   const u_char *packet) {
                    const struct sniff_ethernet
                        *ethernet;               /* The ethernet header */
                    const struct sniff_ip *ip;   /* The IP header */
                    const struct sniff_udp *udp; /* The UDP header */
                    const char *payload;         /* Packet payload */

                    u_int size_ip;
                    u_int size_tcp;
                    ethernet = (struct sniff_ethernet *)(packet);
                    ip = (struct sniff_ip *)(packet + SIZE_ETHERNET);
                    size_ip = IP_HL(ip) * 4;
                    if (size_ip < 20)
                    {
                        LOG(ERROR) << "Invalid IP header length: " << size_ip
                                   << " bytes";
                        return;
                    }

                    if (ip->ip_p != IP_UDP)
                    {
                        LOG(ERROR) << "Invalid IP header protocol " << ip->ip_p;
                        return;
                    }

                    char *src = inet_ntoa(ip->ip_src);
                    udp =
                        (struct sniff_udp *)(packet + SIZE_ETHERNET + size_ip);

                    std::string sender =
                        fmt::format("{}:{}", src, ntohs(udp->uh_sport));

                    NetworkServer *server =
                        (NetworkServer *)NetworkServer::mEthThMap[args];
                    if (server->mCurPosition + 1600 >= server->mBufferSize)
                    {
                        LOG(INFO) << "==================";
                        server->mReceiveBufferIndex ^= 1;
                        server->mCurPosition = 0;
                    }
                    char *data =
                        &server->mpReceiveBuffers[server->mReceiveBufferIndex]
                                                 [server->mCurPosition];
                    int len = ntohs(udp->uh_ulen) - SIZE_UDP;
                    memcpy(data, packet + SIZE_ETHERNET + size_ip + SIZE_UDP,
                           len);
                    server->mCurPosition += len;

                    /*
                    uint8_t *udata = (uint8_t *)data;
                    if (sender == "127.0.0.1:12300")
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
                        LOG_EVERY_N(INFO, 1000) << "dump peer " << sender
                                                << " to file tbut_in.rtp";
                            std::ofstream file("/workspaces/ffvms/tbut_in.rtp",
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

                    size_t idx = std::hash<std::string>{}(sender) %
                                 server->mpUdpIOThreadPools.size();
                    server->mpUdpIOThreadPools[idx]->push(
                        [server, data, len, sender](int id) {
                            VLOG(2) << "new task in thread id " << id;
                            auto &buffer = server->mpReceiveBuffers[id];

                            std::shared_lock _(server->mMutex);
                            auto it = server->mRegisteredPeer.find(sender);
                            if (it == server->mRegisteredPeer.end())
                            {
                                LOG(WARNING)
                                    << "sender " << sender << " not registered";
                                return;
                            }
                            it->second->processFunc(data, len);
                        });
                },
                user) == -1)
        {
            LOG(FATAL) << "could not init pcap loop, " << pcap_geterr(handle);
        }
    });
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

    workThreadNum = 1;
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
