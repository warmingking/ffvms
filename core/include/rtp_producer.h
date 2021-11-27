#ifndef __RTP_PRODUCER_H__
#define __RTP_PRODUCER_H__

#include "async_client.h"
#include "metrics_manager.h"
#include "network_server.h"
#include "video_greeter.grpc.pb.h"
#include <event2/event.h>
#include <functional>
#include <map>
#include <rtp-demuxer.h>
#include <rtp-payload.h>
#include <rtp.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>
#include <thread_pool.h>
extern "C"
{
#include <libavformat/avformat.h>
}
#include "common.h"

namespace ffvms
{
namespace core
{

/**
 * @brief 处理 sdp 的回调函数
 * @param sdp sdp string ( may be meaningless if not ec )
 */
using ProcessSdpFunction = std::function<void(std::string &&sdp)>;

/**
 * @brief rtp 包的回调函数
 * @param data pkt data
 * @param len pkt length
 */
using ConsumePktFuction = std::function<void(const char *data, const size_t len)>;

/**
 * @brief 错误处理的回调函数
 * @param ec error code
 */
using ProcessErrorFunction = std::function<void(std::error_code &&ec)>;

class RtpProducer
{
public:
    struct Config
    {
        int file_event_thread_num;
        std::string network_receive_host;
        std::string sipper_host;
        int rtp_jitter_in_ms;
        int max_bandwidth_in_mb;
        int rpc_timeout_in_ms;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Config, file_event_thread_num, network_receive_host,
                                       sipper_host, rtp_jitter_in_ms, max_bandwidth_in_mb,
                                       rpc_timeout_in_ms)
    };

public:
    RtpProducer();
    virtual ~RtpProducer();

    // 初始化 grpc client ( 和 sipper 通信 )
    std::error_code Init(Config config, std::shared_ptr<NetworkServer> networkServer);

    void RegisterVideo(VideoRequest video, ProcessSdpFunction &&processSdpFunc,
                       ConsumePktFuction &&consumePktFunc, ProcessErrorFunction &&processErrorFunc);

    void UnregisterVideo(const VideoRequest &video);

private:
    Config mConfig;

public:
    // file 相关
    struct FileVideoContext
    {
        static const size_t bufSize = 4 * 1024 * 1024;

        // 输入信息
        VideoRequest videoRequest;
        ConsumePktFuction consumePktFunc;
        ProcessErrorFunction processErrorFunc;
        // 视频信息
        std::pair<int, int> fps;
        int videoStreamId;
        AVFormatContext *iFmtCtx;
        AVFormatContext *oFmtCtx;
        std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> pkt;
        std::unique_ptr<unsigned char, std::function<void(unsigned char *)>> buf;
        int64_t pts; // 只有在输入流 pts 为 AV_NOPTS_VALUE 时才有效
        int64_t prePts;
        // 标志位
        int error;
        std::unique_ptr<struct event, std::function<void(struct event *)>> mpFileEvent;

        FileVideoContext(const VideoRequest &video, ConsumePktFuction &&consumePktFunc,
                         ProcessErrorFunction &&processErrorFunc)
            : pts(0), prePts(0), error(false), videoRequest(video), consumePktFunc(consumePktFunc),
              processErrorFunc(processErrorFunc)
        {
            pkt = std::unique_ptr<AVPacket, std::function<void(AVPacket *)>>(
                new AVPacket, [](AVPacket *p) { av_packet_free(&p); });
            av_init_packet(pkt.get());
            pkt->data = NULL;
            pkt->size = 0;
            buf = std::unique_ptr<unsigned char, std::function<void(unsigned char *)>>(
                (unsigned char *)av_malloc(bufSize), [](unsigned char *p) { av_free(p); });
        }

        ~FileVideoContext()
        {
            if (mpFileEvent)
            {
                event_del(mpFileEvent.get());
            }
            if (iFmtCtx != NULL)
            {
                avformat_close_input(&iFmtCtx);
            }
            if (oFmtCtx != NULL && oFmtCtx->oformat != NULL && oFmtCtx->pb != NULL)
            {
                if (!(oFmtCtx->oformat->flags & AVFMT_NOFILE))
                {
                    avio_context_free(&oFmtCtx->pb);
                }
                avformat_free_context(oFmtCtx);
            }
        }

        // ffmpeg 获取一帧, 并 remux 成 RTP packet
        static void FFGetFrame(evutil_socket_t, short, void *);
    };

private:
    // 离线文件, ffmpeg
    // gb, media-server
    void RegisterFileVideo(VideoRequest &&video, ProcessSdpFunction &&processSdpFunc);

    std::atomic_int mEventBaseCursor;
    std::vector<std::unique_ptr<std::thread>> mFileThreads;
    std::vector<std::unique_ptr<struct event_base, std::function<void(struct event_base *)>>>
        mFileIoBases;
    std::vector<std::unique_ptr<struct event, std::function<void(struct event *)>>>
        mFileKeepAliveEvents;
    std::unique_ptr<common::ThreadPool> mpFileIoThreadPool;
    std::map<VideoRequest, std::unique_ptr<FileVideoContext>> mFileVideoContextMap;
    std::shared_mutex mFileContextMutex;
    static const size_t MAX_SDP_LENGTH = 1024;
    static const size_t MAX_RTP_PKT_SIZE = 1400;

    // =================================================================
public:
    // gb 相关
    struct RtpVideoContext
    {
        // 监控
        common::metrics::prometheus::MetricsManager::UPCounter pProducedCounter;
        common::metrics::prometheus::MetricsManager::UPCounter pRemuxCounter;
        common::metrics::prometheus::MetricsManager::UPCounter pDemuxCounter;

        // 输入信息
        VideoRequest videoRequest;
        ProcessSdpFunction processSdpFunc;
        ConsumePktFuction consumePktFunc;
        ProcessErrorFunction processErrorFunc;

        // 取流信息
        std::string inviteId;
        std::unique_ptr<rtp_demuxer_t, std::function<void(rtp_demuxer_t *)>> demuxer;
        std::unique_ptr<void, std::function<void(void *)>> payloadEncoder;
        std::unique_ptr<void, std::function<void(void *)>> rtp;
        std::unique_ptr<char, std::function<void(char *)>> packet;
        // for h264 ( add start code )
        std::unique_ptr<char, std::function<void(char *)>> payloadBuf;

        // 放弃线程池的方式实现, 原因是需要一次 data copy
        // rtp demuxer 不是线程安全的, 使用 size = 1 的线程池保护
        // std::unique_ptr<common::ThreadPool> demuxThread;
        std::mutex demuxMutex;

        RtpVideoContext(const VideoRequest &video, ProcessSdpFunction &&processSdpFunc,
                        ConsumePktFuction &&consumePktFunc, ProcessErrorFunction &&processErrorFunc);

            static int RtpOnPacket(void *param, const void *packet, int bytes, uint32_t timestamp,
                                   int flags);
        static struct rtp_payload_t RtpPayloadFunc;
        static struct rtp_event_t RtpEventHandler;
    };

    std::map<VideoRequest, std::shared_ptr<RtpVideoContext>> mRtpVideoContextMap;
    std::shared_mutex mRtpContextMutex;
    std::unique_ptr<VideoGreeter::Stub> mpStub;
    std::unique_ptr<common::grpc::AsyncClient> mpGrpcClient;

    std::shared_ptr<NetworkServer> mpNetworkServer;
};
} // namespace core
} // namespace ffvms

#endif // __RTP_PRODUCER_H__