#include "video_manager_service.h"

#include <glog/logging.h>
#include <grpc/support/log.h>
#include <grpcpp/grpcpp.h>

#include <thread>

#include "rtsp_server.h"

DEFINE_string(rpc_addr, "127.0.0.1:10130", "rpc server address");
DEFINE_string(gb_receive_host, "127.0.0.1", "gb receive video host");
DEFINE_int32(gb_receive_port, 7000, "gb receive video port");

const size_t MAX_SDP_LENGTH = 1024;
const size_t MAX_RTP_PKT_SIZE = 1400;

VideoSink::~VideoSink() {}

VideoSink *VideoManagerService::videoSink = NULL;

VideoManagerService::AsyncInviteCallData::AsyncInviteCallData(const VideoRequest &request, const InviteCompleteCallback &callback)
    : videoRequest(request), callback(callback) {}

void VideoManagerService::AsyncInviteCallData::onResponse(const grpc::Status &status)
{
    VLOG(2) << "got invite response";
    if (status.ok())
    {
        if (response.ret().code() == 0)
        {
            callback(response.peerinfo(), 0);
        }
        callback(response.peerinfo(), response.ret().code());
    }
    else
    {
        callback(response.peerinfo(), 500);
    }
}

VideoManagerService::VideoManagerService(VideoSink *sink)
    : stub_(ffvms::videogreeter::NewStub(grpc::CreateChannel(FLAGS_rpc_addr, grpc::InsecureChannelCredentials())))
{
    videoSink = sink;
    mpProbeVideoThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpGetAndSendFrameThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpTeardownThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    rpcThread = std::thread(&VideoManagerService::asyncCompleteRpc, this);

    networkServer = new NetworkServer();
    networkServer->initUDPServer(7000);
    networkServer->registerPeer("127.0.0.1:12345");
    // av_log_set_callback([](void *ptr, int level, const char *fmt, va_list vlt) {
    //     if (level <= AV_LOG_INFO) {
    //         char str[1024];
    //         vsprintf(str, fmt, vlt);
    //         LOG(WARNING) << "[ffmpeg] " << str;
    //     }
    // });
    mProbeVideoOnlineThreadPoolCleanThread = std::thread([this]() {
        while (true)
        {
            LOG_EVERY_N(INFO, 3600) << "probe video online thread pool clean thread stail alive";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::scoped_lock _(mProbeVideoOnlineThreadPoolMutex);
            auto it = mProbeVideoOnlineThreadPool.begin();
            size_t count = 0;
            while (it != mProbeVideoOnlineThreadPool.end())
            {
                if (it->second.second)
                {
                    it = mProbeVideoOnlineThreadPool.erase(it);
                    count++;
                }
                else
                {
                    ++it;
                }
            }
            if (count)
            {
                VLOG(1) << count << " probe video online thread removed";
            }
        }
    });
}

void VideoManagerService::asyncCompleteRpc()
{
    void *got_tag;
    bool ok = false;

    // Block until the next result is available in the completion queue "cq".
    while (cq_.Next(&got_tag, &ok))
    {
        // The tag in this example is the memory location of the call object
        AsyncInviteCallData *call = static_cast<AsyncInviteCallData *>(got_tag);

        // Verify that the request was completed successfully. Note that "ok"
        // corresponds solely to the request for updates introduced by Finish().
        GPR_ASSERT(ok);

        call->onResponse(call->status);
        // Once we're complete, deallocate the call object.
        delete call;
    }
}

void VideoManagerService::addAndProbeVideoSourceAsync(const VideoRequest &request, const AddVideoSourceCallback &callback)
{
    std::unique_lock _(mVideoContextMapMutex);
    if (mRequest2VideoContextMap.count(request) != 0)
    {
        LOG(ERROR) << "unexcept error, request " << request << " already added for probe";
        std::string sdp;
        callback(request, 500, sdp); // repeated request
        return;
    }
    VideoContextPtr pVideoContext = std::make_shared<VideoContext>();
    std::scoped_lock videoLock(pVideoContext->mutex);
    mRequest2VideoContextMap[request] = pVideoContext;
    if (request._use_gb)
    {
        pVideoContext->peerInfo = "127.0.0.1:12345";
        mpProbeVideoThreadPool->push([this, &request, callback](int id) { addAndProbeVideoSource(request, callback); });
        return;

        ffvms::InviteRequest inviteRequest;
        ffvms::GBInviteRequest *gbInviteRequest = inviteRequest.mutable_gbinviterequest();
        gbInviteRequest->set_cameraid(request.gbid);
        gbInviteRequest->set_starttime(0);
        gbInviteRequest->set_endtime(0);
        gbInviteRequest->set_receiveip(FLAGS_gb_receive_host);
        gbInviteRequest->set_receiveport(FLAGS_gb_receive_port);
        gbInviteRequest->set_speed(1);
        gbInviteRequest->set_usetcp(false);
        gbInviteRequest->set_tcppositive(false);

        AsyncInviteCallData *call = new AsyncInviteCallData(request, [this, &request, callback](const std::string &peerInfo, const int error) {
            std::shared_lock _(mVideoContextMapMutex);
            auto it = mRequest2VideoContextMap.find(request);
            if (it != mRequest2VideoContextMap.end())
            {
                if (error != 0)
                {
                    std::string sdp; // tmp
                    callback(request, error, sdp);
                    return;
                }
                VideoContextPtr &pVideoContext = it->second; // alias
                pVideoContext->peerInfo = peerInfo;
                std::thread t(&VideoManagerService::addAndProbeVideoSource, this, request, callback);
                std::scoped_lock _(mProbeVideoOnlineThreadPoolMutex);
                mProbeVideoOnlineThreadPool[t.get_id()] = std::make_pair(std::move(t), false);
            }
            else
            {
                LOG(INFO) << "request " << request << " not found, may have been deleted";
                std::string sdp; // tmp
                callback(request, error, sdp);
            }
        });

        call->response_reader = stub_->PrepareAsyncinviteVideo(&call->context, inviteRequest, &cq_);
        call->response_reader->StartCall();
        call->response_reader->Finish(&call->response, &call->status, (void *)call);
    }
    else if (request._use_file)
    {
        pVideoContext->filename = request.filename;
        pVideoContext->repeatedly = request.repeatedly;
        mpProbeVideoThreadPool->push([this, &request, callback](int id) { addAndProbeVideoSource(request, callback); });
    }
}

struct SdpOpaque
{
    std::string peer;
    NetworkServer *server;
};

int sdp_read(void *opaque, uint8_t *buf, int size)
{
    assert(opaque);
    assert(buf);
    auto octx = static_cast<SdpOpaque *>(opaque);

    size_t count;
    octx->server->readData(octx->peer, size, buf, count);
    if (count == 0)
    {
        return AVERROR(EAGAIN);
    }

    return count;
}

int VideoManagerService::rtp_open(AVFormatContext **pctx, const std::string &peer, AVDictionary **options)
{
    assert(pctx);
    *pctx = avformat_alloc_context();
    assert(*pctx);
    // (*pctx)->flags |= AVFMT_FLAG_NONBLOCK;

    const size_t avioBufferSize = 1600;
    auto avioBuffer = static_cast<uint8_t *>(av_malloc(avioBufferSize));
    auto opaque = new SdpOpaque();

    opaque->peer = peer;
    opaque->server = networkServer;

    auto pbctx = avio_alloc_context(avioBuffer, avioBufferSize, 0, opaque, sdp_read, nullptr, nullptr);
    assert(pbctx);

    (*pctx)->pb = pbctx;

    auto infmt = av_find_input_format("mpegts");

    return avformat_open_input(pctx, NULL, infmt, options);
}

void sdp_close(AVFormatContext **fctx)
{
    assert(fctx);
    auto ctx = *fctx;

    // Opaque can be non-POD type, free it before and assign to null
    auto opaque = static_cast<SdpOpaque *>(ctx->pb->opaque);
    delete opaque;
    ctx->pb->opaque = nullptr;

    avio_close(ctx->pb);
    avformat_close_input(fctx);
}

void VideoManagerService::addAndProbeVideoSource(const VideoRequest request, const AddVideoSourceCallback callback)
{
    int ret(0);
    std::string sdp;
    std::string title;
    AVFormatContext *iFmtCtx = NULL, *oFmtCtx = NULL;
    auto it = mRequest2VideoContextMap.end();
    {
        std::shared_lock _(mVideoContextMapMutex);
        it = mRequest2VideoContextMap.find(request);
        if (it == mRequest2VideoContextMap.end())
        {
            LOG(INFO) << "request " << request << " not found, may have been deleted";
            callback(request, 404, sdp);
        }
    }
    VideoContextPtr &pVideoContext = it->second; // alias
    std::scoped_lock _(pVideoContext->mutex);
    do
    {
        if (!pVideoContext->filename.empty())
        {
            // 文件协议
            title = pVideoContext->filename;
            ret = avformat_open_input(&iFmtCtx, title.c_str(), NULL, NULL);
        }
        else if (!pVideoContext->peerInfo.empty())
        {
            title = pVideoContext->peerInfo;
            ret = rtp_open(&iFmtCtx, pVideoContext->peerInfo, NULL);
            // custom io
            // ffmpeg -re -stream_loop -1 -i tbut.mp4 -c copy -an -f mpegts
            // "udp://localhost:7000" ret = avformat_open_input(&iFmtCtx,
            // "udp://143.0.0.1:7000", NULL, NULL); ffmpeg -re -stream_loop -1
            // -i tbut.mp4 -c copy -an -f mpegts "tcp://localhost:7000" ret =
            // avformat_open_input(&iFmtCtx, "tcp://127.0.0.1:7000?listen",
            // NULL, NULL);
        }
        if (ret < 0)
        {
            LOG(ERROR) << "open request " << request << " failed";
            break;
        }
        ret = avformat_find_stream_info(iFmtCtx, NULL);
        if (ret < 0)
        {
            LOG(ERROR) << "find stream info failed for request " << request;
            break;
        }
        networkServer->probeFinish("127.0.0.1:12345");
        av_dump_format(iFmtCtx, 0, title.c_str(), 0); // TODO:  what if gb?

        int streamid = av_find_best_stream(iFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (streamid < 0)
        {
            LOG(ERROR) << "find stream info failed for request: " << request;
            ret = -1; // TODO 乱写的
            break;
        }
        AVStream *stream = iFmtCtx->streams[streamid];
        auto fps = std::make_pair<>(stream->avg_frame_rate.num, stream->avg_frame_rate.den);
        ret = avformat_alloc_output_context2(&oFmtCtx, NULL, "rtp", NULL);
        if (ret < 0)
        {
            LOG(ERROR) << "could not create output context for request " << request;
            break;
        }
        pVideoContext->videoStreamId = streamid;
        pVideoContext->fps = fps;
        pVideoContext->iFmtCtx = iFmtCtx;
        pVideoContext->oFmtCtx = oFmtCtx;

        av_dict_set(&oFmtCtx->metadata, "title", title.c_str(), 0);

        AVOutputFormat *oFmt = oFmtCtx->oformat;
        AVStream *outStream = avformat_new_stream(oFmtCtx, NULL);
        if (outStream == NULL)
        {
            LOG(ERROR) << "failed allocating output stream for request " << request;
            break;
        }
        ret = avcodec_parameters_copy(outStream->codecpar, stream->codecpar);
        if (ret < 0)
        {
            LOG(ERROR) << "failed to copy codec parameters for request " << request;
            break;
        }
        outStream->codecpar->codec_tag = 0;
        AVIOContext *oioContext = avio_alloc_context(
            pVideoContext->buf, VideoContext::bufSize, 1, const_cast<VideoRequest *>(&it->first), NULL,
            [](void *opaque, uint8_t *buf, int bufsize) {
                const VideoRequest *request = (VideoRequest *)opaque;
                videoSink->writeRtpData(*request, buf, bufsize);
                return 0;
            },
            NULL);
        oioContext->max_packet_size = MAX_RTP_PKT_SIZE;
        oFmtCtx->pb = oioContext;
        // av_dump_format(videoContext->oFmtCtx, 0, NULL, 1);
        char *buf = new char[MAX_SDP_LENGTH];
        ret = av_sdp_create(&oFmtCtx, 1, buf, MAX_SDP_LENGTH);
        if (ret < 0)
        {
            LOG(ERROR) << "create sdp failed for request " << request;
            break;
        }
        sdp = std::string(buf);
        delete[] buf; // FIXME: ugly
        ret = avformat_write_header(oFmtCtx, NULL);
        if (ret < 0)
        {
            LOG(ERROR) << "error occurred when write header for request " << request;
            break;
        }
        // 只有在初始化output成功后才调用callback
        callback(request, 0, sdp);
    } while (false);
    if (ret != 0)
    {
        char buf[1024];
        LOG(WARNING) << "probe video failed for " << request << ", for reason: " << av_make_error_string(buf, 1024, ret);
        if (ret == AVERROR(ENOENT))
        {
            callback(request, 404, sdp);
        }
        else
        {
            callback(request, 500, sdp);
        }

        if (teardown(request) == -1)
        {
            // TODO: -1 表示还没保存到 map 里面, 只需要手动清理 ctx 即可
            if (iFmtCtx != NULL)
            {
                avformat_close_input(&iFmtCtx);
            }
            if (oFmtCtx != NULL)
            {
                if (!(oFmtCtx->oformat->flags & AVFMT_NOFILE))
                {
                    avio_context_free(&oFmtCtx->pb);
                }
                av_dict_free(&oFmtCtx->metadata);
                avformat_free_context(oFmtCtx);
            }
        }
    }
    std::scoped_lock mProbeVideoOnlineThreadPoolLock(mProbeVideoOnlineThreadPoolMutex);
    auto probeVideoOnlineThreadPoolIt = mProbeVideoOnlineThreadPool.find(std::this_thread::get_id());
    if (probeVideoOnlineThreadPoolIt != mProbeVideoOnlineThreadPool.end())
    {
        probeVideoOnlineThreadPoolIt->second.second = true;
    }
}

int VideoManagerService::getVideoFps(const VideoRequest &request, std::pair<uint32_t, uint32_t> &fps)
{
    if (mRequest2VideoContextMap.count(request) == 0)
    {
        LOG(ERROR) << "not found video context for request" << request;
        return -1;
    }
    fps = mRequest2VideoContextMap[request]->fps;
    return 0;
}

void VideoManagerService::getFrameAsync(const VideoRequest &request)
{
    mpGetAndSendFrameThreadPool->push([this, request](int id) { getFrame(request); });
}

void VideoManagerService::getFrame(const VideoRequest request)
{
    const auto t = std::chrono::system_clock::now();
    std::shared_lock _(mVideoContextMapMutex);
    if (mRequest2VideoContextMap.count(request) == 0)
    {
        LOG(ERROR) << "no video context for request " << request;
        return;
        // FIXME: this is unexcept, what should do?
    }
    VideoContextPtr &pVideoContext = mRequest2VideoContextMap[request];
    if (pVideoContext->mutex.try_lock())
    {
        // 用来过滤音频流
        bool getVideoPkt(false);
        bool locked(true);
        int ret(0);
        do
        {
            pVideoContext->iFmtCtx->pb->error = 0;
            ret = av_read_frame(pVideoContext->iFmtCtx, pVideoContext->pkt);
            if (ret < 0)
            {
                // 如果是 eof 且需要循环取流, seek 到开始位置, 重新播放
                if ((ret == AVERROR_EOF) && pVideoContext->repeatedly)
                {
                    if (pVideoContext->repeatedly)
                    {
                        VLOG(1) << "read frame for request " << request << " eof, will repeat read";
                        bool seekSucc(true);
                        for (size_t i = 0; i < pVideoContext->iFmtCtx->nb_streams; ++i)
                        {
                            const auto stream = pVideoContext->iFmtCtx->streams[i];
                            ret = avio_seek(pVideoContext->iFmtCtx->pb, 0, SEEK_SET);
                            if (ret < 0)
                            {
                                seekSucc = false;
                                LOG(ERROR) << "error occurred when seeking to start";
                                break;
                            }
                            ret = avformat_seek_file(pVideoContext->iFmtCtx, i, 0, 0, stream->duration, 0);
                            if (ret < 0)
                            {
                                seekSucc = false;
                                LOG(ERROR) << "error occurred when seeking to start";
                                break;
                            }
                        }
                        if (seekSucc)
                        {
                            pVideoContext->oFmtCtx->streams[0]->cur_dts = 0;
                            VLOG(1) << "seek to start for request " << request;
                            continue;
                        }
                        else
                        {
                            break;
                        }
                    }
                    else
                    {
                        pVideoContext->finish = true;
                        break;
                    }
                }
                else
                {
                    LOG(ERROR) << "error occurred when read frame for request " << request;
                    break;
                }
            }
            if (pVideoContext->pkt->stream_index != pVideoContext->videoStreamId)
            {
                VLOG(2) << "ignore packet not video stream";
                av_packet_unref(pVideoContext->pkt);
                continue;
            }
            else
            {
                getVideoPkt = true;
            }
            pVideoContext->pkt->stream_index = 0;
            pVideoContext->pkt->pos = -1;

            /* copy packet */
            const auto &streamId = pVideoContext->videoStreamId; // alise
            pVideoContext->pkt->pts =
                av_rescale_q_rnd(pVideoContext->pkt->pts, pVideoContext->iFmtCtx->streams[streamId]->time_base, pVideoContext->oFmtCtx->streams[0]->time_base,
                                 static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            pVideoContext->pkt->dts =
                av_rescale_q_rnd(pVideoContext->pkt->dts, pVideoContext->iFmtCtx->streams[streamId]->time_base, pVideoContext->oFmtCtx->streams[0]->time_base,
                                 static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            pVideoContext->pkt->duration =
                av_rescale_q(pVideoContext->pkt->duration, pVideoContext->iFmtCtx->streams[streamId]->time_base, pVideoContext->oFmtCtx->streams[0]->time_base);
            if (pVideoContext->pkt->duration == 0)
            {
                pVideoContext->pkt->duration = (pVideoContext->iFmtCtx->streams[streamId]->time_base.den * pVideoContext->fps.second) /
                                               (pVideoContext->fps.first * pVideoContext->iFmtCtx->streams[streamId]->time_base.num);
            }
            VLOG_EVERY_N(1, 1000) << "pts: " << pVideoContext->pkt->pts << ", dts: " << pVideoContext->pkt->dts << " prePts: " << pVideoContext->prePts;
            if (pVideoContext->pkt->pts == AV_NOPTS_VALUE || pVideoContext->pkt->pts <= pVideoContext->prePts)
            {
                VLOG_EVERY_N(1, 10) << "pts: " << pVideoContext->pkt->pts << ", dts: " << pVideoContext->pkt->dts;
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
            // av_interleaved_write_frame前释放锁, 避免回调里面有锁, 形成死锁
            pVideoContext->mutex.unlock();
            locked = false;
            ret = av_interleaved_write_frame(pVideoContext->oFmtCtx, pVideoContext->pkt);
            if (ret < 0)
            {
                LOG(ERROR) << "error muxing packet for request " << request;
                break;
            }
        } while (!getVideoPkt);
        if (ret != 0)
        {
            if (locked)
            {
                pVideoContext->mutex.unlock();
            }
            char buf[1024];
            LOG(WARNING) << "get frame failed for request " << request << ", for reason: " << av_make_error_string(buf, 1024, ret);
            if (ret != AVERROR(EAGAIN))
            {
                std::shared_lock _(mVideoContextMapMutex);
                if (mRequest2VideoContextMap.count(request) == 0)
                {
                    LOG(ERROR) << "no video context for request " << request;
                    // FIXME: this is unexcept, what should do?
                    return;
                }
                VideoContextPtr pVideoContext = mRequest2VideoContextMap[request];
                std::scoped_lock videoLock(pVideoContext->mutex);
                if (pVideoContext->finish)
                {
                    LOG(INFO) << "write trailer for " << request;
                    int ret = av_write_trailer(pVideoContext->oFmtCtx);
                    if (ret < 0)
                    {
                        LOG(WARNING) << "write trailer failer for " << request << ", for reason: " << av_make_error_string(buf, 1024, ret);
                    }
                }
                teardownAsync(request);
                videoSink->streamComplete(request);
            }
        }
        LOG_EVERY_N(INFO, 100000) << "send one frame for " << request << " spend "
                                  << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - t).count() << " microseconds";
    }
    else
    {
        LOG(ERROR) << "request " << request << " is locked";
    }
}

void VideoManagerService::teardownAsync(const VideoRequest &request)
{
    mpTeardownThreadPool->push([this, request](int id) { teardown(request); });
}

int VideoManagerService::teardown(const VideoRequest request)
{
    std::unique_lock _(mVideoContextMapMutex);
    if (mRequest2VideoContextMap.count(request) == 0)
    {
        LOG(WARNING) << "request " << request << " not found, may not be added";
        return -1; // -1 表示没找到, 需要手动清理
    }
    VideoContextPtr pVideoContext = mRequest2VideoContextMap[request];
    avformat_close_input(&pVideoContext->iFmtCtx);
    if (pVideoContext->oFmtCtx != NULL)
    {
        if (!(pVideoContext->oFmtCtx->oformat->flags & AVFMT_NOFILE))
        {
            avio_context_free(&pVideoContext->oFmtCtx->pb);
        }
        avformat_free_context(pVideoContext->oFmtCtx);
    }

    av_packet_free(&pVideoContext->pkt);
    av_free(pVideoContext->buf);
    mRequest2VideoContextMap.erase(request);
    LOG(INFO) << "teardown " << request << " succ";
    return 0;
}

void VideoManagerService::finishingWork(const std::string &request) { LOG(INFO) << "finish ============="; }
