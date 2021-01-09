#include <thread>
#include <glog/logging.h>
#include "video_manager_service.h"
#include "rtsp_server.h"

const size_t MAX_SDP_LENGTH = 1024;
const size_t MAX_RTP_PKT_SIZE = 1400;

VideoSink::~VideoSink() {}

VideoSink *VideoManagerService::videoSink = NULL;

int VideoManagerService::writeCb(void *opaque, uint8_t *buf, int bufsize)
{
    const VideoRequest *request = (VideoRequest *)opaque;
    videoSink->writeRtpData(*request, buf, bufsize);
    return 0;
}

VideoManagerService::VideoManagerService(VideoSink *sink)
{
    videoSink = sink;
    mpAddAndProbeVideoThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpGetAndSendFrameThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
    mpTeardownThreadPool = new ctpl::thread_pool(std::thread::hardware_concurrency());
}

void VideoManagerService::InitializeRpc(const std::string &rpcFromHost, const uint16_t &rpcFromPort)
{
    LOG(INFO) << "TODO: Initialize";
}

void VideoManagerService::addAndProbeVideoSourceAsync(const VideoRequest& request, AddVideoSourceCallback callback)
{
    mpAddAndProbeVideoThreadPool->push([this, request, callback](int id) {
        addAndProbeVideoSource(request, callback);
    });
}

void VideoManagerService::addAndProbeVideoSource(const VideoRequest request, AddVideoSourceCallback callback)
{
    VLOG(1) << "addAndProbeVideoSource. request: " << request;

    int ret(0);
    std::string sdp;
    AVFormatContext *iFmtCtx = NULL, *oFmtCtx = NULL;
    do
    {
        if (request._use_file)
        {
            ret = avformat_open_input(&iFmtCtx, request.filename.c_str(), NULL, NULL);
            if (ret < 0)
            {
                LOG(ERROR) << "open file " << request.filename << " failed";
                break;
            }
            ret = avformat_find_stream_info(iFmtCtx, NULL);
            if (ret < 0)
            {
                LOG(ERROR) << "find stream info failed for request: " << request;
                break;
            }
            // av_dump_format(iFmtCtx, 0, request.filename.c_str(), 0);

            int streamid = av_find_best_stream(iFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
            if (streamid < 0)
            {
                LOG(ERROR) << "find stream info failed for request: " << request;
                break;
            } // 以上可能会阻塞, 放在锁外面
            {
                { 
                    // 拿写锁, 保存到 map 里面
                    std::unique_lock _(mVideoContextMapMutex);
                    AVStream *stream = iFmtCtx->streams[streamid];
                    auto fps = std::make_pair<>(stream->avg_frame_rate.num, stream->avg_frame_rate.den);
                    ret = avformat_alloc_output_context2(&oFmtCtx, NULL, "rtp", NULL);
                    if (ret < 0)
                    {
                        LOG(ERROR) << "could not create output context for request " << request;
                        break;
                    }

                    VideoContext *videoContext = new VideoContext(request.repeatedly, streamid, fps, iFmtCtx, oFmtCtx);
                    auto [it, succ] = mUrl2VideoContextMap.insert(std::make_pair(request, videoContext));
                    if (!succ)
                    {
                        LOG(ERROR) << "unexcept error, insert video context failed for request " << request;
                        return;
                    }

                    av_dict_set(&oFmtCtx->metadata, "title", request.filename.c_str(), 0);

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
                        videoContext->buf, VideoContext::bufSize,
                        1,
                        const_cast<VideoRequest *>(&it->first),
                        NULL,
                        VideoManagerService::writeCb,
                        NULL);
                    oioContext->max_packet_size = MAX_RTP_PKT_SIZE;
                    videoContext->oFmtCtx->pb = oioContext;
                    // av_dump_format(videoContext->oFmtCtx, 0, NULL, 1);
                    char *buf = new char[MAX_SDP_LENGTH];
                    ret = av_sdp_create(&videoContext->oFmtCtx, 1, buf, MAX_SDP_LENGTH);
                    if (ret < 0)
                    {
                        LOG(ERROR) << "create sdp failed for request " << request;
                        break;
                    }
                    sdp = std::string(buf);
                    delete[] buf; // FIXME: ugly
                    // oFmt->flags &= AVFMT_NOTIMESTAMPS; // 没什么用
                    ret = avformat_write_header(videoContext->oFmtCtx, NULL);
                    if (ret < 0)
                    {
                        LOG(ERROR) << "error occurred when write header for request " << request.filename;
                        break;
                    }
                }
                callback(request, 0, sdp); // 只有在初始化output成功后才调用callback
            }
        }
        else
        {
            LOG(ERROR) << "illegal request " << request << ", can't parse file or rtsp or gb";
            break;
        }
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
}

int VideoManagerService::getVideoFps(const VideoRequest &request, std::pair<uint32_t, uint32_t> &fps)
{
    if (mUrl2VideoContextMap.count(request) == 0)
    {
        LOG(ERROR) << "not found video context for request" << request;
        return -1;
    }
    fps = mUrl2VideoContextMap[request]->fps;
    return 0;
}

void VideoManagerService::getFrameAsync(const VideoRequest &request)
{
    mpGetAndSendFrameThreadPool->push([this, request](int id) {
        getFrame(request);
    });
}

void VideoManagerService::getFrame(const VideoRequest request)
{
    const auto t = std::chrono::system_clock::now();
    std::shared_lock _(mVideoContextMapMutex);
    if (mUrl2VideoContextMap.count(request) == 0)
    {
        LOG(ERROR) << "no video context for request " << request;
        return;
        // FIXME: this is unexcept, what should do?
    }
    VideoContext *videoContext = mUrl2VideoContextMap[request];
    if (videoContext->mutex.try_lock())
    {
        // 用来过滤音频流
        bool getVideoPkt(false);
        bool locked(true);
        int ret(0);
        do
        {
            ret = av_read_frame(videoContext->iFmtCtx, videoContext->pkt);
            if (ret < 0)
            {
                // 如果是 eof 且需要循环取流, seek 到开始位置, 重新播放
                if ((ret == AVERROR_EOF) && videoContext->repeatedly)
                {
                    if (videoContext->repeatedly)
                    {
                        LOG(INFO) << "read frame for request " << request << " eof, will repeat read";
                        bool seekSucc(true);
                        for (size_t i = 0; i < videoContext->iFmtCtx->nb_streams; ++i)
                        {
                            const auto stream = videoContext->iFmtCtx->streams[i];
                            ret = avio_seek(videoContext->iFmtCtx->pb, 0, SEEK_SET);
                            if (ret < 0)
                            {
                                seekSucc = false;
                                LOG(ERROR) << "error occurred when seeking to start";
                                break;
                            }
                            ret = avformat_seek_file(videoContext->iFmtCtx, i, 0, 0, stream->duration, 0);
                            if (ret < 0)
                            {
                                seekSucc = false;
                                LOG(ERROR) << "error occurred when seeking to start";
                                break;
                            }
                        }
                        if (seekSucc)
                        {
                            videoContext->oFmtCtx->streams[0]->cur_dts = 0;
                            LOG(WARNING) << "seek to start for request " << request;
                            continue;
                        }
                        else
                        {
                            break;
                        }
                    } else
                    {
                        videoContext->finish = true;
                        break;
                    }
                }
                else
                {
                    LOG(ERROR) << "error occurred when read frame for request " << request;
                    break;
                }
            }
            if (videoContext->pkt->stream_index != videoContext->videoStreamId)
            {
                VLOG(2) << "ignore packet not video stream";
                av_packet_unref(videoContext->pkt);
                continue;
            }
            else
            {
                getVideoPkt = true;
            }
            videoContext->pkt->stream_index = 0;

            /* copy packet */
            const auto &streamId = videoContext->videoStreamId; // alise
            videoContext->pkt->pts = av_rescale_q_rnd(videoContext->pkt->pts,
                                                      videoContext->iFmtCtx->streams[streamId]->time_base,
                                                      videoContext->oFmtCtx->streams[0]->time_base,
                                                      static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            videoContext->pkt->dts = av_rescale_q_rnd(videoContext->pkt->dts,
                                                      videoContext->iFmtCtx->streams[streamId]->time_base,
                                                      videoContext->oFmtCtx->streams[0]->time_base,
                                                      static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            videoContext->pkt->duration = av_rescale_q(videoContext->pkt->duration,
                                                       videoContext->iFmtCtx->streams[streamId]->time_base,
                                                       videoContext->oFmtCtx->streams[0]->time_base);
            if (videoContext->pkt->duration == 0)
            {
                videoContext->pkt->duration = (videoContext->iFmtCtx->streams[streamId]->time_base.den * videoContext->fps.second) / (videoContext->fps.first * videoContext->iFmtCtx->streams[streamId]->time_base.num);
            }
            videoContext->pkt->pos = -1;
            if (videoContext->pkt->pts == AV_NOPTS_VALUE)
            {
                if (videoContext->pkt->dts == AV_NOPTS_VALUE)
                {
                    videoContext->pts += videoContext->pkt->duration;
                    videoContext->pkt->pts = videoContext->pts;
                    videoContext->pkt->dts = videoContext->pts;
                }
                else
                {
                    videoContext->pkt->pts = videoContext->pkt->dts;
                }
            }
            // av_interleaved_write_frame前释放锁, 避免回调里面有锁, 形成死锁
            videoContext->mutex.unlock();
            locked = false;
            ret = av_interleaved_write_frame(videoContext->oFmtCtx, videoContext->pkt);
            if (ret < 0)
            {
                LOG(ERROR) << "error muxing packet for request " << request;
                break;
            }
        } while (!getVideoPkt);
        if (ret != 0)
        {
            if (locked) {
                videoContext->mutex.unlock();
            }
            char buf[1024];
            LOG(WARNING) << "get frame failed for request " << request << ", for reason: " << av_make_error_string(buf, 1024, ret);
            if (ret != AVERROR(EAGAIN))
            {
                std::shared_lock _(mVideoContextMapMutex);
                if (mUrl2VideoContextMap.count(request) == 0)
                {
                    LOG(ERROR) << "no video context for request " << request;
                    // FIXME: this is unexcept, what should do?
                    return;
                }
                VideoContext *videoContext = mUrl2VideoContextMap[request];
                std::scoped_lock videoLock(videoContext->mutex);
                if (videoContext->finish)
                {
                    LOG(INFO) << "write trailer for " << request;
                    int ret = av_write_trailer(videoContext->oFmtCtx);
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
                                  << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - t).count()
                                  << " microseconds";
    }
    else
    {
        LOG(ERROR) << "request " << request << " is locked";
    }
}

void VideoManagerService::teardownAsync(const VideoRequest &request)
{
    mpTeardownThreadPool->push([this, request](int id) {
        teardown(request);
    });
}

int VideoManagerService::teardown(const VideoRequest request)
{
    std::unique_lock _(mVideoContextMapMutex);
    if (mUrl2VideoContextMap.count(request) == 0)
    {
        LOG(WARNING) << "request " << request << " not found, may not be added";
        return -1; // -1 表示没找到, 需要手动清理
    }
    VideoContext *pVideoContext = mUrl2VideoContextMap[request];
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
    delete pVideoContext;
    mUrl2VideoContextMap.erase(request);
    LOG(INFO) << "teardown " << request << " succ";
    return 0;
}

void VideoManagerService::finishingWork(const std::string &request)
{
    LOG(INFO) << "finish =============";
}
