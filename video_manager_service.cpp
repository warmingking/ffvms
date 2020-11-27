#include <thread>
#include <glog/logging.h>
#include "video_manager_service.h"
#include "rtsp_server.h"

const size_t MAX_SDP_LENGTH = 1024;
const size_t MAX_RTP_PKT_SIZE = 1400;

VideoSink::~VideoSink() {}

VideoSink* VideoManagerService::videoSink = NULL;

int VideoManagerService::writeCb(void* opaque, uint8_t* buf, int bufsize) {
    const VideoRequest* request = (VideoRequest*) opaque;
    videoSink->writeRtpData(*request, buf, bufsize);
    return 0;
}

VideoManagerService::VideoManagerService(VideoSink* sink) {
    videoSink = sink;
}

void VideoManagerService::InitializeRpc(const std::string& rpcFromHost, const uint16_t& rpcFromPort) {
    LOG(INFO) << "TODO: Initialize";
}

void VideoManagerService::addAndProbeVideoSourceAsync(const VideoRequest request, AddVideoSourceCallback callback) {
    VLOG(1) << "addAndProbeVideoSourceAsync. request: " << request;
    do {
        if (request._use_file) {
            AVFormatContext *iFmtCtx = NULL, *oFmtCtx = NULL;
            int ret = avformat_open_input(&iFmtCtx, request.filename.c_str(), NULL, NULL);
            if (ret < 0) {
                LOG(ERROR) << "open file " << request.filename << " failed";
                break;
            }
            ret = avformat_find_stream_info(iFmtCtx, NULL);
            if (ret < 0) {
                LOG(ERROR) << "find stream info failed for request: " << request;
                break;
            }
            // LOG(INFO) << "dump input format for request: " << request;
            av_dump_format(iFmtCtx, 0, request.filename.c_str(), 0);

            int streamid = av_find_best_stream(iFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
            if (streamid < 0) {
                LOG(ERROR) << "find stream info failed for request: " << request;
                break;
            }
            AVStream* stream = iFmtCtx->streams[streamid];
            auto fps = std::make_pair<>(stream->avg_frame_rate.num, stream->avg_frame_rate.den);
            ret = avformat_alloc_output_context2(&oFmtCtx, NULL, "rtp", request.filename.c_str());
            if (ret < 0) {
                LOG(ERROR) << "could not create output context for request " << request;
                break;
            }
            VideoContext* videoContext = new VideoContext(request.repeatedly, streamid, fps, iFmtCtx, oFmtCtx);
            auto [it, succ] = mUrl2VideoContextMap.insert(std::make_pair(request, videoContext));
            if (!succ) {
                LOG(ERROR) << "unexcept error, insert video context failed";
                break;
            }
            AVOutputFormat* oFmt = oFmtCtx->oformat;
            AVStream* outStream = avformat_new_stream(oFmtCtx, NULL);
            if (outStream == NULL) {
                LOG(ERROR) << "failed allocating output stream for request " << request;
                mUrl2VideoContextMap.erase(it);
                break;
            }
            ret = avcodec_parameters_copy(outStream->codecpar, stream->codecpar);
            if (ret < 0) {
                LOG(ERROR) << "failed to copy codec parameters for request " << request;
                mUrl2VideoContextMap.erase(it);
                break;
            }
            outStream->codecpar->codec_tag = 0;
            AVIOContext* oioContext = avio_alloc_context(
                videoContext->buf, VideoContext::bufSize,
                1,
                const_cast<VideoRequest*>(&it->first),
                NULL,
                VideoManagerService::writeCb,
                NULL);
            oioContext->max_packet_size = MAX_RTP_PKT_SIZE;
            videoContext->oFmtCtx->pb = oioContext;
            LOG(INFO) << "dump output format for request: " << request;
            av_dump_format(videoContext->oFmtCtx, 0, NULL, 1);
            char* buf = new char[MAX_SDP_LENGTH];
            ret = av_sdp_create(&videoContext->oFmtCtx, 1, buf, MAX_SDP_LENGTH);
            if (ret < 0) {
                LOG(ERROR) << "create sdp failed for request " << request;
                mUrl2VideoContextMap.erase(it);
                break;
            }
            std::string sdp(buf);
            delete[] buf; // FIXME: ugly
            // oFmt->flags &= AVFMT_NOTIMESTAMPS;
            ret = avformat_write_header(videoContext->oFmtCtx, NULL);
            if (ret < 0) {
                LOG(ERROR) << "error occurred when write header for request " << request.filename;
                mUrl2VideoContextMap.erase(it);
                break;
            }
            callback(std::make_pair(&request, &sdp)); // 只有在初始化output成功后才调用callback
        } else {
            LOG(ERROR) << "illegal request " << request << ", can't parse file or rtsp or gb";
            break;
        }
    } while (false);
}

int VideoManagerService::getVideoFps(const VideoRequest& request, std::pair<uint32_t, uint32_t>& fps) {
    if (mUrl2VideoContextMap.count(request) == 0) {
        LOG(ERROR) << "not found video context for request" << request;
        return -1;
    }
    fps = mUrl2VideoContextMap[request]->fps;
    return 0;
}

int VideoManagerService::getFrame(const VideoRequest& request) {
    if (mUrl2VideoContextMap.count(request) == 0) {
        LOG(ERROR) << "no video context for request " << request;
        return -1;
    }
    VideoContext* videoContext = mUrl2VideoContextMap[request];
    if (videoContext->mutex.try_lock()) {
        bool getVideoPkt(false);
        int ret(0);
        do {
            ret = av_read_frame(videoContext->iFmtCtx, videoContext->pkt);
            if (ret < 0) {
                if((ret == AVERROR_EOF) && videoContext->repeatedly) {
                    LOG(INFO) << "read frame for request " << request << " eof, will repeat read";
                    bool seekSucc(true);
                    for (size_t i = 0; i < videoContext->iFmtCtx->nb_streams; ++i) {
                        const auto stream = videoContext->iFmtCtx->streams[i];
                        ret = avio_seek(videoContext->iFmtCtx->pb, 0, SEEK_SET);
                        if (ret < 0) {
                            seekSucc = false;
                            LOG(ERROR) << "error occurred when seeking to start";
                            break;
                        }
                        ret = avformat_seek_file(videoContext->iFmtCtx, i, 0, 0, stream->duration, 0);
                        if (ret < 0) {
                            seekSucc = false;
                            LOG(ERROR) << "error occurred when seeking to start";
                            break;
                        }
                    }
                    if (seekSucc) {
                        videoContext->oFmtCtx->streams[0]->cur_dts = 0;
                        LOG(WARNING) << "seek to start for request " << request;
                       continue;
                    } else {
                        break;
                    }
                } else {
                    LOG(ERROR) << "error occurred when read frame for request " << request;
                    break;
                }
            }
            if (videoContext->pkt->stream_index != videoContext->videoStreamId) {
                VLOG(2) << "ignore packet not video stream";
                av_packet_unref(videoContext->pkt);
                continue;
            } else {
                getVideoPkt = true;
            }
            videoContext->pkt->stream_index = 0;

            /* copy packet */
            const auto& streamId = videoContext->videoStreamId; // alise
            videoContext->pkt->pts = av_rescale_q_rnd(videoContext->pkt->pts,
                                                    videoContext->iFmtCtx->streams[streamId]->time_base,
                                                    videoContext->oFmtCtx->streams[0]->time_base,
                                                    static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            videoContext->pkt->dts = av_rescale_q_rnd(videoContext->pkt->dts,
                                                    videoContext->iFmtCtx->streams[streamId]->time_base,
                                                    videoContext->oFmtCtx->streams[0]->time_base,
                                                    static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            videoContext->pkt->duration = av_rescale_q(videoContext->pkt->duration,
                                                    videoContext->iFmtCtx->streams[streamId]->time_base,
                                                    videoContext->oFmtCtx->streams[0]->time_base);
            if (videoContext->pkt->duration == 0) {
                videoContext->pkt->duration = (videoContext->iFmtCtx->streams[streamId]->time_base.den
                                               * videoContext->fps.second)
                                               / (videoContext->fps.first
                                               * videoContext->iFmtCtx->streams[streamId]->time_base.num);
            }
            videoContext->pkt->pos = -1;
            if (videoContext->pkt->pts == AV_NOPTS_VALUE) {
                if (videoContext->pkt->dts == AV_NOPTS_VALUE) {
                    videoContext->pts += videoContext->pkt->duration;
                    videoContext->pkt->pts = videoContext->pts;
                    videoContext->pkt->dts = videoContext->pts;
                } else {
                    videoContext->pkt->pts = videoContext->pkt->dts;
                }
            }
            ret = av_interleaved_write_frame(videoContext->oFmtCtx, videoContext->pkt);
            if (ret < 0) {
                LOG(ERROR) << "error muxing packet for request " << request;
                break;
            }
        } while (!getVideoPkt);
        if (ret != 0) {
            char buf;
            LOG(WARNING) << "get frame failed for request " << request << ", for reason: " << av_make_error_string(&buf, 1024, ret);
        }
        videoContext->mutex.unlock();
        return 0;
    } else {
        LOG(ERROR) << "request " << request << " is locked";
        return -1;
    }
}

void VideoManagerService::finishingWork(const std::string& request) {
    LOG(INFO) << "finish =============";
}
