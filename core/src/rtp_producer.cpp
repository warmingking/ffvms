#include "rtp_producer.h"
#include "common.h"

#include <event2/thread.h>
#include <fmt/core.h>
#include <fstream>
#include <rtp-profile.h>

using namespace common::grpc;
using namespace ffvms::core;
using namespace grpc;
using namespace std::placeholders;
using namespace common::metrics::prometheus;

struct rtp_payload_t RtpProducer::RtpVideoContext::RtpPayloadFunc
{
    // alloc
    [](void *param, int bytes) -> void * {
        RtpVideoContext *context = static_cast<RtpVideoContext *>(param);
        LOG_IF(FATAL, bytes > 1600) << "alloc rtp bytes more than mtu";
        return context->packet.get();
    },
        // free
        [](void *param, void *packet) {
            RtpVideoContext *context = static_cast<RtpVideoContext *>(param);
            LOG_IF(FATAL, context->packet.get() != packet) << "free unknown rtp packet";
        },
        // packet
        [](void *param, const void *packet, int bytes, uint32_t timestamp, int flags) {
            VLOG_EVERY_N(1, 10) << "on packet length " << bytes << ", send to rtsp server";
            RtpVideoContext *context = static_cast<RtpVideoContext *>(param);

            /*
            if (context->videoRequest.gbid == "1234")
            {
                LOG_EVERY_N(INFO, 1000)
                    << "dump " << context->videoRequest << " to file tbut.rtp";
                std::ofstream file("/workspaces/ffvms/tbut.rtp",
            std::ios::binary | std::ios::app);
                // 先用 2 位保存 rtp 包的长度, 然后保存 rtp 包
                char len[2] = {0, 0};
                len[0] = bytes >> 8;
                len[1] = bytes & 0xFF;
                file.write(len, 2);
                file.write((const char *)packet, bytes);
            }
            */
            context->consumePktFunc((const char *)packet, bytes);
            context->pProducedCounter->Increment(bytes);
            return rtp_onsend(context->rtp.get(), packet, bytes);
        }
};

struct rtp_event_t RtpProducer::RtpVideoContext::RtpEventHandler
{
    [](void *param, const struct rtcp_msg_t *msg) {
        // TODO: need implement
    }
};

RtpProducer::RtpVideoContext::RtpVideoContext(const VideoRequest &video,
                                              ProcessSdpFunction &&processSdpFunc,
                                              ConsumePktFuction &&consumePktFunc,
                                              ProcessErrorFunction &&processErrorFunc)
    : videoRequest(video), processSdpFunc(processSdpFunc), consumePktFunc(consumePktFunc),
      processErrorFunc(processErrorFunc)
//   demuxThread(std::make_unique<common::ThreadPool>(1))
{
    std::map<std::string, std::string> labels;
    labels["module"] = "rtp producer";
    labels["info"] = "produced bytes";
    pProducedCounter = MMAddCounter(videoRequest.gbid, labels);
    labels["info"] = "remux input bytes";
    pRemuxCounter = MMAddCounter(videoRequest.gbid, labels);
    labels["info"] = "demux input bytes";
    pDemuxCounter = MMAddCounter(videoRequest.gbid, labels);
}

RtpProducer::~RtpProducer()
{
    for (int i = 0; i < mFileIoBases.size(); ++i)
    {
        event_del(mFileKeepAliveEvents.back().get());
        event_base_loopbreak(mFileIoBases[i].get());
        mFileThreads[i]->join();
    }
    if (mpFileIoThreadPool)
    {
        // TODO: stop thread pool
    }
}

RtpProducer::RtpProducer() : mEventBaseCursor(0) {}

std::error_code RtpProducer::Init(Config config, std::shared_ptr<NetworkServer> networkServer)
{
    mpNetworkServer = networkServer;
    mConfig = config;
    int ret = evthread_use_pthreads();
    if (ret != 0)
    {
        LOG(FATAL) << "evthread use pthread failed, ret: " << ret;
    }

    int file_thread_num = config.file_event_thread_num == -1 ? std::thread::hardware_concurrency()
                                                             : config.file_event_thread_num;
    for (int i = 0; i < file_thread_num; ++i)
    {
        mFileIoBases.emplace_back(
            std::unique_ptr<struct event_base, std::function<void(struct event_base *)>>(
                event_base_new(), [](struct event_base *eb) { event_base_free(eb); }));
        if (!mFileIoBases.back())
        {
            LOG(FATAL) << "create event base failed, with errno: " << errno;
        }

        struct timeval frameInterval = {60, 0};
        mFileKeepAliveEvents.emplace_back(
            std::unique_ptr<struct event, std::function<void(struct event *)>>(
                event_new(
                    mFileIoBases.back().get(), -1, EV_TIMEOUT | EV_PERSIST,
                    [](evutil_socket_t fd, short what, void *arg) {
                        VLOG(1) << "file io thread still alive";
                    },
                    NULL),
                [](struct event *ev) { event_free(ev); }));
        if (!mFileKeepAliveEvents.back()
            || event_add(mFileKeepAliveEvents.back().get(), &frameInterval) < 0)
        {
            LOG(FATAL) << "create/add keepalive event failed, with errno: " << errno;
        }

        mFileThreads.emplace_back(std::make_unique<std::thread>(
            [i](struct event_base *base) {
                int ret = event_base_dispatch(base);
                LOG_IF(ERROR, ret != 0) << "file event loop " << i << " exist with code " << ret;
            },
            mFileIoBases.back().get()));
    }
    mpFileIoThreadPool = std::make_unique<common::ThreadPool>(file_thread_num);

    mpGrpcClient = std::make_unique<AsyncClient>(std::thread::hardware_concurrency());

    mpStub = std::move(VideoGreeter::NewStub(
        grpc::CreateChannel(config.sipper_host, grpc::InsecureChannelCredentials())));

    return FFVMS_SUCC;
}

void RtpProducer::FileVideoContext::FFGetFrame(evutil_socket_t fd, short what, void *arg)
{
    // av_read_frame in event loop
    RtpProducer::FileVideoContext *pVideoContext = (RtpProducer::FileVideoContext *)arg;
    if (pVideoContext->error != 0)
    {
        LOG(ERROR) << "video " << pVideoContext->videoRequest << " stopped with error code "
                   << pVideoContext->error;
        return;
    }
    bool getVideoPkt(false); // 用来过滤音频流
    int ret(0);
    do
    {
        ret = av_read_frame(pVideoContext->iFmtCtx, pVideoContext->pkt.get());
        if (ret < 0)
        {
            // 如果是 eof 且需要循环取流, seek
            // 到开始位置, 重新播放
            if (ret == AVERROR_EOF)
            {
                if (pVideoContext->videoRequest.repeatedly)
                {
                    VLOG(1) << "read frame for video " << pVideoContext->videoRequest
                            << " eof, will repeat read";
                    bool seekSucc(true);
                    for (size_t i = 0; i < pVideoContext->iFmtCtx->nb_streams; ++i)
                    {
                        const auto stream = pVideoContext->iFmtCtx->streams[i];
                        ret = avio_seek(pVideoContext->iFmtCtx->pb, 0, SEEK_SET);
                        if (ret < 0)
                        {
                            seekSucc = false;
                            LOG(ERROR) << "error occurred "
                                          "when seeking "
                                          "to start";
                            break;
                        }
                        ret = avformat_seek_file(pVideoContext->iFmtCtx, i, 0, 0, stream->duration,
                                                 0);
                        if (ret < 0)
                        {
                            seekSucc = false;
                            LOG(ERROR) << "error occurred "
                                          "when seeking "
                                          "to start";
                            break;
                        }
                    }
                    if (seekSucc)
                    {
                        pVideoContext->oFmtCtx->streams[0]->cur_dts = 0;
                        VLOG(1) << "seek to start for "
                                   "video "
                                << pVideoContext->videoRequest;
                        continue;
                    }
                    else
                    {
                        break;
                    }
                }
                else
                {
                    pVideoContext->error = AVERROR_EOF;
                    break;
                }
            }
            else
            {
                LOG(ERROR) << "error occurred when read "
                              "frame for video "
                           << pVideoContext->videoRequest;
                break;
            }
        }
        if (pVideoContext->pkt->stream_index != pVideoContext->videoStreamId)
        {
            VLOG(2) << "ignore packet not video stream";
            av_packet_unref(pVideoContext->pkt.get());
            continue;
        }
        else
        {
            getVideoPkt = true;
        }
        pVideoContext->pkt->stream_index = 0;
        pVideoContext->pkt->pos = -1;

        /* copy packet */
        const auto &streamId = pVideoContext->videoStreamId; // alias
        av_packet_rescale_ts(pVideoContext->pkt.get(),
                             pVideoContext->iFmtCtx->streams[streamId]->time_base,
                             pVideoContext->oFmtCtx->streams[0]->time_base);
        // pVideoContext->pkt->pts = av_rescale_q_rnd(
        //     pVideoContext->pkt->pts,
        //     pVideoContext->iFmtCtx->streams[streamId]->time_base,
        //     pVideoContext->oFmtCtx->streams[0]->time_base,
        //     static_cast<AVRounding>(AV_ROUND_NEAR_INF |
        //     AV_ROUND_PASS_MINMAX));
        // pVideoContext->pkt->dts = av_rescale_q_rnd(
        //     pVideoContext->pkt->dts,
        //     pVideoContext->iFmtCtx->streams[streamId]->time_base,
        //     pVideoContext->oFmtCtx->streams[0]->time_base,
        //     static_cast<AVRounding>(AV_ROUND_NEAR_INF |
        //     AV_ROUND_PASS_MINMAX));
        // pVideoContext->pkt->duration =
        //     av_rescale_q(pVideoContext->pkt->duration,
        //                  pVideoContext->iFmtCtx->streams[streamId]->time_base,
        //                  pVideoContext->oFmtCtx->streams[0]->time_base);
        if (pVideoContext->pkt->duration == 0)
        {
            pVideoContext->pkt->duration =
                (pVideoContext->iFmtCtx->streams[streamId]->time_base.den
                 * pVideoContext->fps.second)
                / (pVideoContext->fps.first
                   * pVideoContext->iFmtCtx->streams[streamId]->time_base.num);
        }
        // VLOG_EVERY_N(1, 100) << "pts: " << pVideoContext->pkt->pts
        //                       << ", dts: " << pVideoContext->pkt->dts
        //                       << ", prePts: " << pVideoContext->prePts;
        if (pVideoContext->pkt->pts == AV_NOPTS_VALUE
            || pVideoContext->pkt->pts <= pVideoContext->prePts)
        {
            // VLOG_EVERY_N(1, 100) << "pts: " << pVideoContext->pkt->pts
            //                     << ", dts: " << pVideoContext->pkt->dts;
            if (pVideoContext->pkt->dts == AV_NOPTS_VALUE)
            {
                pVideoContext->pts += pVideoContext->pkt->duration;
                pVideoContext->pkt->pts = pVideoContext->pts;
                pVideoContext->pkt->dts = pVideoContext->pts;
            }
            else if (pVideoContext->pkt->dts <= pVideoContext->prePts)
            {
                pVideoContext->pts = pVideoContext->prePts + pVideoContext->pkt->duration;
                pVideoContext->pkt->pts = pVideoContext->pts;
                pVideoContext->pkt->dts = pVideoContext->pts;
            }
            else
            {
                pVideoContext->pkt->pts = pVideoContext->pkt->dts;
            }
        }
        pVideoContext->prePts = pVideoContext->pkt->pts;
        ret = av_interleaved_write_frame(pVideoContext->oFmtCtx, pVideoContext->pkt.get());
        if (ret < 0)
        {
            LOG(ERROR) << "error muxing packet for video " << pVideoContext->videoRequest;
            break;
        }
    } while (!getVideoPkt);
    if (ret != 0)
    {
        pVideoContext->error = ret;
        char buf[1024];
        LOG(ERROR) << "get frame failed for video " << pVideoContext->videoRequest
                   << ", for reason: " << av_make_error_string(buf, 1024, ret);
        pVideoContext->processErrorFunc(std::make_error_code(std::errc::invalid_argument));
    }
}

void RtpProducer::RegisterFileVideo(VideoRequest &&video, ProcessSdpFunction &&processSdpFunc)
{
    int ret(0);
    do
    {
        // 检查这路流是否已经被关闭
        std::shared_lock _(mFileContextMutex);
        auto it = mFileVideoContextMap.find(video);
        if (it == mFileVideoContextMap.end())
        {
            LOG(ERROR) << "video " << video << " not found in cache, may have been unregistered";
            break;
        }
        auto &pVideoContext = it->second;
        // probe file
        auto &iFmtCtx = pVideoContext->iFmtCtx;
        auto &oFmtCtx = pVideoContext->oFmtCtx;
        auto &fps = pVideoContext->fps;
        ret = avformat_open_input(&iFmtCtx, video.filename.c_str(), NULL, NULL);
        if (ret < 0)
        {
            LOG(ERROR) << "open video " << video << " failed";
            break;
        }
        ret = avformat_find_stream_info(iFmtCtx, NULL);
        if (ret < 0)
        {
            LOG(ERROR) << "find stream info failed for video " << video;
            break;
        }
        av_dump_format(iFmtCtx, 0, video.filename.c_str(), 0);
        pVideoContext->videoStreamId =
            av_find_best_stream(iFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (pVideoContext->videoStreamId < 0)
        {
            LOG(ERROR) << "find stream info failed for video " << video;
            ret = pVideoContext->videoStreamId;
            break;
        }

        // 获取 fps, 用于控制 read frame 的频率
        AVStream *stream = iFmtCtx->streams[pVideoContext->videoStreamId];
        fps = std::make_pair<>(stream->avg_frame_rate.num, stream->avg_frame_rate.den);
        // mux 成 RTP 格式输出
        ret = avformat_alloc_output_context2(&oFmtCtx, NULL, "rtp", NULL);
        if (ret < 0)
        {
            LOG(ERROR) << "create output context failed for video " << video;
            break;
        }

        // 设置生成的 RTSP 的 title ( sdp s= )
        av_dict_set(&oFmtCtx->metadata, "title", video.filename.c_str(), 0);

        AVStream *outStream = avformat_new_stream(oFmtCtx, NULL);
        if (outStream == NULL)
        {
            LOG(ERROR) << "failed allocating output stream for video " << video;
            break;
        }
        ret = avcodec_parameters_copy(outStream->codecpar, stream->codecpar);
        if (ret < 0)
        {
            LOG(ERROR) << "failed to copy codec parameters for video " << video;
            break;
        }
        outStream->codecpar->codec_tag = 0;

        // 自定义 write_packet callback
        AVIOContext *oioContext = avio_alloc_context(
            pVideoContext->buf.get(), FileVideoContext::bufSize, 1, it->second.get(), NULL,
            [](void *opaque, uint8_t *buf, int bufsize) {
                const FileVideoContext *context = (FileVideoContext *)opaque;
                context->consumePktFunc((char *)buf, bufsize);
                return 0;
            },
            NULL);

        // 设置最大 RTP 包大小
        oioContext->max_packet_size = MAX_RTP_PKT_SIZE;
        oFmtCtx->pb = oioContext;
        av_dump_format(pVideoContext->oFmtCtx, 0, NULL, 1);

        // 获取 sdp
        auto buf = std::unique_ptr<char, std::function<void(char *)>>(new char[MAX_SDP_LENGTH],
                                                                      [](char *c) { delete[] c; });
        ret = av_sdp_create(&oFmtCtx, 1, buf.get(), MAX_SDP_LENGTH);
        if (ret < 0)
        {
            LOG(ERROR) << "create sdp failed for video " << video;
            break;
        }
        ret = avformat_write_header(oFmtCtx, NULL);
        if (ret < 0)
        {
            LOG(ERROR) << "error occurred when write header for video " << video;
            break;
        }
        processSdpFunc(std::string(buf.get()));

        // start remux event according to fps
        const auto &index = mEventBaseCursor.fetch_add(1) % mFileIoBases.size();
        long microseconds = 1e6 * fps.second / fps.first;
        struct timeval frameInterval = {0, microseconds};
        pVideoContext->mpFileEvent =
            std::unique_ptr<struct event, std::function<void(struct event *)>>(
                event_new(mFileIoBases[index].get(), -1, EV_TIMEOUT | EV_PERSIST,
                          RtpProducer::FileVideoContext::FFGetFrame, it->second.get()),
                [](struct event *ev) { event_free(ev); });
        if (!it->second->mpFileEvent
            || event_add(it->second->mpFileEvent.get(), &frameInterval) < 0)
        {
            LOG(ERROR) << "create/add read frame event failed for video " << video;
            break;
        }
        return;
    } while (false);
    if (ret != 0)
    {
        char buf[1024];
        LOG(ERROR) << "probe failed for " << video
                   << ", for reason: " << av_make_error_string(buf, 1024, ret);
        // 检查这路流是否已经被关闭
        std::shared_lock _(mFileContextMutex);
        auto it = mFileVideoContextMap.find(video);
        if (it == mFileVideoContextMap.end())
        {
            LOG(ERROR) << "video " << video << " not found in cache, may have been unregistered";
        }
        it->second->processErrorFunc(std::make_error_code(std::errc::invalid_argument));
    }
    // probe 失败, 自动清理 mFileVideoContextMap, 不需要 unregister
    std::unique_lock _(mFileContextMutex);
    mFileVideoContextMap.erase(video);
}

int RtpProducer::RtpVideoContext::RtpOnPacket(void *param, const void *packet, int bytes,
                                              uint32_t timestamp, int flags)
{
    VLOG_EVERY_N(1, 10) << "rtp on packet length " << bytes << ", send to payload encoder";
    RtpProducer::RtpVideoContext *context = static_cast<RtpProducer::RtpVideoContext *>(param);
    context->pRemuxCounter->Increment(bytes);

    int ret(0);
    if (context->payloadBuf)
    {
        *context->payloadBuf.get() = 0;
        *(context->payloadBuf.get() + 1) = 0;
        *(context->payloadBuf.get() + 2) = 1;
        memcpy(context->payloadBuf.get() + 3, packet, bytes);

        ret = rtp_payload_encode_input(context->payloadEncoder.get(), context->payloadBuf.get(),
                                       bytes + 3, timestamp);
    }
    else
    {
        ret = rtp_payload_encode_input(context->payloadEncoder.get(), packet, bytes, timestamp);
    }

    LOG_IF(ERROR, ret != 0) << "payload input failed, ret " << ret;

    return ret;
}

void RtpProducer::RegisterVideo(VideoRequest video, ProcessSdpFunction &&processSdpFunc,
                                ConsumePktFuction &&consumePktFunc,
                                ProcessErrorFunction &&processErrorFunc)
{
    LOG(INFO) << "register video " << video;
    /**
     * 对于视频文件
     * 1 判断是否已经 registed
     * 2 在线程池中执行:
     * 2.1 使用 ffmpeg 获取视频信息, 初始化 output context, 注册 consumePktFunc,
     * 获取 sdp 并调用 processSdpFunc
     * 2.1.1 如果 probe 失败, 通过 processErrorFunc 返回报错 ( 会 unregister,
     * 不需要调用者处理 )
     * 2.2 添加 event, 根据视频的 fps 定期获取下一帧,
     * 转封装后喂给 rtsp server
     * 2.2.1 如果取帧失败, 通过 processErrorFunc
     * 返回报错, 并标记这一路已经失败, 后续 read frame 时候直接跳过 ( 并没有
     * unregister, 需要调用者处理 )
     */
    // file protocol
    if (video._use_file)
    {
        int ret(0);
        std::unique_lock _(mFileContextMutex);
        auto it = mFileVideoContextMap.find(video);
        if (it != mFileVideoContextMap.end())
        {
            LOG(INFO) << "video " << video << " already exist, not register multi times";
            processErrorFunc(std::make_error_code(std::errc::too_many_files_open));
            return;
        }
        mFileVideoContextMap[video] = std::make_unique<FileVideoContext>(
            video, std::move(consumePktFunc), std::move(processErrorFunc));
        mpFileIoThreadPool->submit(
            [this, video(std::move(video)), processSdpFunc(std::move(processSdpFunc))]() mutable {
                RegisterFileVideo(std::move(video), std::move(processSdpFunc));
            });
    }
    else if (video._use_gb)
    {
        std::unique_lock _(mRtpContextMutex);
        auto it = mRtpVideoContextMap.find(video);
        if (it != mRtpVideoContextMap.end())
        {
            LOG(ERROR) << "video " << video << " already exist, not register multi times";
            processErrorFunc(std::make_error_code(std::errc::too_many_files_open));
            return;
        }
        auto pContext = std::make_shared<RtpVideoContext>(video, std::move(processSdpFunc),
                                                          std::move(consumePktFunc),
                                                          std::move(processErrorFunc));
        std::weak_ptr<RtpVideoContext> pWeakContext = pContext;
        mRtpVideoContextMap[video] = pContext;

        InviteVideoRequest request;
        auto *gbInviteRequest = request.mutable_gbinviterequest();
        gbInviteRequest->set_cameraid(video.gbid);
        gbInviteRequest->set_receiveinfo(mConfig.network_receive_host);
        switch (video.gbStreamingMode)
        {
        case TCP:
            gbInviteRequest->set_transmission(InviteVideoRequest_GBInviteRequest_Transmission_TCP);
            break;
        case UDP:
            gbInviteRequest->set_transmission(InviteVideoRequest_GBInviteRequest_Transmission_UDP);
            break;
        default:
            LOG(ERROR) << "unkown gbStreamingMode " << video.gbStreamingMode;
            processErrorFunc(std::make_error_code(std::errc::invalid_argument));
            return;
        }
        mpGrpcClient->Call<InviteVideoRequest, InviteVideoResponse>(
            [this](ClientContext *context, const InviteVideoRequest &request, CompletionQueue *cq) {
                return mpStub->PrepareAsyncInviteVideo(context, request, cq);
            },
            request, mConfig.rpc_timeout_in_ms,
            [this, video(std::move(video)),
             pWeakContext(std::move(pWeakContext))](std::unique_ptr<InviteVideoResponse> &&response,
                                                    std::unique_ptr<::grpc::Status> &&error) {
                std::shared_lock _(mRtpContextMutex);
                auto pVideoContext = pWeakContext.lock();
                if (!pVideoContext)
                {
                    LOG(ERROR) << "video " << video
                               << " not found in cache, may have been "
                                  "unregistered";
                    return;
                }
                bool succ(false);
                do
                {
                    if (error && !error->ok())
                    {
                        LOG(ERROR) << "invite failed for video " << video
                                   << ", error code: " << error->error_code()
                                   << ", error message: " << error->error_message();
                        break;
                    }

                    // 检查这路流是否已经被关闭
                    // TODO: 感觉会死锁, 上面拿的锁还没有释放
                    // std::shared_lock _(mRtpContextMutex);
                    switch (response->inviteResponse_case())
                    {
                    case InviteVideoResponse::kGbInviteResponse:
                    {
                        auto &gbInviteResponse = response->gbinviteresponse();
                        pVideoContext->packet = std::unique_ptr<char, std::function<void(char *)>>(
                            new char[1600], [](char *packet) { delete[] packet; });
                        if (gbInviteResponse.encoding() == "H264")
                        {
                            pVideoContext->payloadBuf =
                                std::unique_ptr<char, std::function<void(char *)>>(
                                    new char[mConfig.max_bandwidth_in_mb * 1024 * 1024],
                                    [](char *buf) { delete[] buf; });
                        }
                        pVideoContext->inviteId = response->inviteid();
                        pVideoContext->demuxer =
                            std::unique_ptr<rtp_demuxer_t, std::function<void(rtp_demuxer_t *)>>(
                                rtp_demuxer_create(
                                    mConfig.rtp_jitter_in_ms, gbInviteResponse.frequency(),
                                    gbInviteResponse.payload(), gbInviteResponse.encoding().c_str(),
                                    RtpProducer::RtpVideoContext::RtpOnPacket,
                                    pVideoContext.get()), // context 作为 param
                                [](rtp_demuxer_t *demuxer) { rtp_demuxer_destroy(&demuxer); });
                        if (!pVideoContext->demuxer)
                        {
                            LOG(ERROR) << "create rtp demux failed for video " << video;
                            break;
                        }

                        uint32_t ssrc = rand();
                        pVideoContext->payloadEncoder =
                            std::unique_ptr<void, std::function<void(void *)>>(
                                rtp_payload_encode_create(
                                    gbInviteResponse.payload(), gbInviteResponse.encoding().c_str(),
                                    (uint16_t)ssrc, ssrc,
                                    &RtpProducer::RtpVideoContext::RtpPayloadFunc,
                                    pVideoContext.get()),
                                [](void *encoder) { rtp_payload_encode_destroy(encoder); });
                        if (!pVideoContext->payloadEncoder)
                        {
                            LOG(ERROR) << "create payload encoder failed for video " << video;
                            break;
                        }

                        pVideoContext->rtp = std::unique_ptr<void, std::function<void(void *)>>(
                            rtp_create(&RtpProducer::RtpVideoContext::RtpEventHandler,
                                       pVideoContext.get(), ssrc, ssrc,
                                       gbInviteResponse.frequency(),
                                       mConfig.max_bandwidth_in_mb * 1024 * 1024, 1 /*sender*/),
                            [](void *rtp) { rtp_destroy(rtp); });

                        succ = true;
                        // TODO: 根据 payload 调整
                        pVideoContext->processSdpFunc(fmt::format(
                            "v=0\n"
                            "o=- 0 0 IN IP4 127.0.0.1\n"
                            "s=gb:{}\n"
                            "c=IN IP4 127.0.0.1\n"
                            "m=video 0 RTP/AVP {}\n"
                            "t=0 0\n"
                            "a=rtpmap:{} {}/{}",
                            video.gbid, gbInviteResponse.payload(), gbInviteResponse.payload(),
                            gbInviteResponse.encoding(), gbInviteResponse.frequency()));
                        // fmt::format("v=0\n"
                        //             "o=- 0 0 IN IP4 127.0.0.1\n"
                        //             "s=gb:{}\n"
                        //             "c=IN IP4 127.0.0.1\n"
                        //             "t=0 0\n"
                        //             "a=tool:libavformat 58.29.100\n"
                        //             "m=video 8086 RTP/AVP 96\n"
                        //             "b=AS:7403\n"
                        //             "a=rtpmap:96 H264/90000\n"
                        //             "a=fmtp:96 packetization-mode=1; "
                        //             "sprop-parameter-sets="
                        //             "Z0LAKZp0A8Az0IAAy6eAJiWgR4wZUA==,"
                        //             "aM48gA==; profile-level-id=42C029",
                        //             video.gbid));
                        // 注册这路流, 默认 UDP 模式, 收到数据 ( 一个 RTP 包 )
                        // 后直接丢给 rtp_demuxer
                        LOG(INFO) << "register peer " << gbInviteResponse.peerinfo()
                                  << " for video " << video;
                        mpNetworkServer->registerPeer(
                            gbInviteResponse.peerinfo(),
                            [this, video, pWeakContext(std::move(pWeakContext))](const char *data,
                                                                                 const size_t len) {
                                std::shared_lock _(mRtpContextMutex);
                                auto pVideoContext = pWeakContext.lock();
                                if (!pVideoContext)
                                {
                                    VLOG(1) << "video context not found, "
                                               "may be unregistered";
                                    return;
                                }
                                VLOG_EVERY_N(1, 10)
                                    << "video " << video << " get packet, size : " << len;

                                // if (pVideoContext->videoRequest.gbid == "demo6")
                                // {
                                //     LOG_EVERY_N(INFO, 1000) << "dump " <<
                                //     pVideoContext->videoRequest
                                //                             << " to file tbut_in.rtp";
                                //     std::ofstream file("/ffvms/tbut_in.rtp",
                                //                        std::ios::binary | std::ios::app);
                                //     // 先用 2 位保存 rtp 包的长度, 然后保存 rtp
                                //     // 包
                                //     char lenHeader[2] = {0, 0};
                                //     lenHeader[0] = len >> 8;
                                //     lenHeader[1] = len & 0xFF;
                                //     file.write(lenHeader, 2);
                                //     file.write(data, len);
                                // }

                                pVideoContext->pDemuxCounter->Increment(len);
                                rtp_demuxer_input(pVideoContext->demuxer.get(), data, len);
                            });

                        SendAckRequest request;
                        request.set_inviteid(response->inviteid());
                        mpGrpcClient->Call<SendAckRequest, SendAckResponse>(
                            [this](ClientContext *context, const SendAckRequest &request,
                                   CompletionQueue *cq) {
                                return mpStub->PrepareAsyncSendAck(context, request, cq);
                            },
                            request, 0,
                            [video(std::move(video))](std::unique_ptr<SendAckResponse> &&response,
                                                      std::unique_ptr<::grpc::Status> &&error) {
                                if (error && !error->ok())
                                {
                                    LOG(ERROR) << "send ack failed for video " << video
                                               << ", error code: " << error->error_code()
                                               << ", error message: " << error->error_message();
                                }
                                else
                                {
                                    VLOG(1) << "send ack succ for video " << video;
                                }
                            });
                        break;
                    }
                    default:
                    {
                        LOG(ERROR) << "unknown response type: " << response->inviteResponse_case();
                        break;
                    }
                    }
                } while (false);
                if (!succ)
                {
                    // TODO: 应该用哪个 error code
                    pVideoContext->processErrorFunc(
                        std::make_error_code(std::errc::invalid_argument));
                    return;
                }
            });
    }
}

void RtpProducer::UnregisterVideo(const VideoRequest &video)
{
    LOG(INFO) << "unregister video " << video;
    // file protocol
    if (video._use_file)
    {
        std::unique_lock _(mFileContextMutex);
        mFileVideoContextMap.erase(video);
    }
    else if (video._use_gb)
    {
        std::unique_lock _(mRtpContextMutex);
        auto it = mRtpVideoContextMap.find(video);
        if (it != mRtpVideoContextMap.end())
        {
            if (!it->second->inviteId.empty())
            {
                TearDownRequest request;
                request.set_inviteid(it->second->inviteId);
                mpGrpcClient->Call<TearDownRequest, TearDownResponse>(
                    [this](ClientContext *context, const TearDownRequest &request,
                           CompletionQueue *cq) {
                        return mpStub->PrepareAsyncTearDown(context, request, cq);
                    },
                    request, 0,
                    [video(std::move(video))](std::unique_ptr<TearDownResponse> &&response,
                                              std::unique_ptr<::grpc::Status> &&error) {
                        if (error && !error->ok())
                        {
                            LOG(ERROR) << "teardown failed for video " << video
                                       << ", error code: " << error->error_code()
                                       << ", error message: " << error->error_message();
                        }
                        else
                        {
                            VLOG(1) << "teardown succ for video " << video;
                        }
                    });
            }
        }
        mRtpVideoContextMap.erase(video);
    }
}
