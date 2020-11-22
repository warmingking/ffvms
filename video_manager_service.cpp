#include <thread>
#include <glog/logging.h>
#include "video_manager_service.h"
#include "rtsp_server.h"

const size_t MAX_SDP_LENGTH = 1024;
const std::string FILE_PREFIX = "file/";
const std::string REPEATEDLY_FLAG = "?repeatedly=";
const size_t MAX_RTP_PKT_SIZE = 1400;

std::map<void*, std::string> VideoManagerService::opaque2urlMap;

int VideoManagerService::writeCb(void* opaque, uint8_t* buf, int bufsize) {
    std::string& url = opaque2urlMap[opaque];
    RTSPServer* server = (RTSPServer*) opaque;
    server->writeRtpData(url, buf, bufsize);

    return 0;
}

void VideoManagerService::InitializeRpc(const std::string& rpcFromHost, const uint16_t& rpcFromPort) {
    LOG(INFO) << "TODO: Initialize";
}

void VideoManagerService::addVideoSource(const std::string* url, AddVideoSourceCallback callback, const void* client) {
    VLOG(1) << "addVideoSource. url: " << *url;
    VideoSourceParams params;
    if (parseAndCheckUrl(*url, params)) {
        do {
            if (params._use_file) {
                AVFormatContext *iFmtCtx = NULL, *oFmtCtx = NULL;
                int ret = avformat_open_input(&iFmtCtx, params.filename.c_str(), NULL, NULL);
                if (ret < 0) {
                    LOG(ERROR) << "open file " << params.filename << " failed";
                    break;
                }
                ret = avformat_find_stream_info(iFmtCtx, NULL);
                if (ret < 0) {
                    LOG(ERROR) << "find stream info failed for file: " << params.filename;
                    break;
                }
                LOG(INFO) << "dump input format for url: " << *url;
                av_dump_format(iFmtCtx, 0, params.filename.c_str(), 0);

                int streamid = av_find_best_stream(iFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
                if (streamid < 0) {
                    LOG(ERROR) << "find stream info failed for file: " << params.filename;
                    break;
                }
                AVStream* stream = iFmtCtx->streams[streamid];
                auto fps = std::make_pair<>(stream->avg_frame_rate.num, stream->avg_frame_rate.den);
                ret = avformat_alloc_output_context2(&oFmtCtx, NULL, "rtp", NULL);
                if (ret < 0) {
                    LOG(ERROR) << "could not create output context for url " << *url;
                    break;
                }
                VideoContext* videoContext = new VideoContext(params.repeatedly, streamid, fps, iFmtCtx, oFmtCtx);
                AVOutputFormat* oFmt = oFmtCtx->oformat;
                AVStream* outStream = avformat_new_stream(oFmtCtx, NULL);
                if (outStream == NULL) {
                    LOG(ERROR) << "failed allocating output stream for url " << *url;
                    break;
                }
                ret = avcodec_parameters_copy(outStream->codecpar, stream->codecpar);
                if (ret < 0) {
                    LOG(ERROR) << "failed to copy codec parameters for url " << url;
                    break;
                }
                outStream->codecpar->codec_tag = 0;
                AVIOContext* oioContext = avio_alloc_context(videoContext->buf, VideoContext::bufSize, 1, const_cast<void*>(client), NULL, VideoManagerService::writeCb, NULL);
                oioContext->max_packet_size = MAX_RTP_PKT_SIZE;
                videoContext->oFmtCtx->pb = oioContext;
                LOG(INFO) << "dump output format for url: " << *url;
                av_dump_format(videoContext->oFmtCtx, 0, NULL, 1);
                char* buf = new char[MAX_SDP_LENGTH];
                ret = av_sdp_create(&videoContext->oFmtCtx, 1, buf, MAX_SDP_LENGTH);
                if (ret < 0) {
                    LOG(ERROR) << "create sdp failed for url " << *url;
                    break;
                }
                std::string sdp(buf);
                delete[] buf; // FIXME: ugly
                ret = avformat_write_header(videoContext->oFmtCtx, NULL);
                if (ret < 0) {
                    LOG(ERROR) << "error occurred when opening output file for url " << url;
                    break;
                }
                mUrl2VideoContextMap[*url] = videoContext;
                opaque2urlMap[const_cast<void*> (client)] = *url;
                callback(std::make_pair(url, &sdp), client); // 只有在初始化output成功后才调用callback
            } else {
                LOG(ERROR) << "illegal url " << *url << ", can't parse file or rtsp or gb";
                break;
            }
        } while (false);
    }
}

int VideoManagerService::getVideoFps(const std::string& url, std::pair<uint32_t, uint32_t>& fps) {
    if (mUrl2VideoContextMap.count(url) == 0) {
        LOG(ERROR) << "not found video context for url" << url;
        return -1;
    }
    fps = mUrl2VideoContextMap[url]->fps;
    return 0;
}

#include <execinfo.h>
void print_trace(void) {
    char **strings;
    size_t i, size;
    enum Constexpr { MAX_SIZE = 1024 };
    void *array[MAX_SIZE];
    size = backtrace(array, MAX_SIZE);
    strings = backtrace_symbols(array, size);
    for (i = 0; i < size; i++)
        printf("%s\n", strings[i]);
    puts("");
    free(strings);
}

int VideoManagerService::getFrame(const std::string& url) {
    if (mUrl2VideoContextMap.count(url) == 0) {
        LOG(ERROR) << "no video context for url " << url;
        return -1;
    }
    // print_trace();
    VideoContext* videoContext = mUrl2VideoContextMap[url];
    if (videoContext->mutex.try_lock()) {
        bool getVideoPkt(false);
        int ret(0);
        do {
            ret = av_read_frame(videoContext->iFmtCtx, videoContext->pkt);
            if (ret < 0) {
                LOG(ERROR) << "error occurred when read frame for url " << url;
                if((ret == AVERROR_EOF) && videoContext->repeatedly) {
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
                        LOG(WARNING) << "seek to start for url " << url;
                       continue;
                    } else {
                        break;
                    }
                } else {
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
            videoContext->pkt->pos = -1;
            ret = av_interleaved_write_frame(videoContext->oFmtCtx, videoContext->pkt);
            if (ret < 0) {
                LOG(ERROR) << "error muxing packet for url " << url;
                break;
            }
        } while (!getVideoPkt);
        if (ret != 0) {
            char buf;
            LOG(WARNING) << "get frame failed for url " << url << ", for reason: " << av_make_error_string(&buf, 1024, ret);;
        }
        videoContext->mutex.unlock();
    } else {
        LOG(ERROR) << "url " << url << " are locked";
    }
}


bool VideoManagerService::parseAndCheckUrl(const std::string& url, VideoSourceParams& params) {
    params.reset();
    size_t found;
    if ((found = url.find(FILE_PREFIX)) != std::string::npos) {
        params._use_file = true;
        params.repeatedly = false;
        std::string filename = url.substr(found + FILE_PREFIX.size());
        if ((found = filename.find(REPEATEDLY_FLAG)) != std::string::npos) {
            std::string repeatedly = filename.substr(found + REPEATEDLY_FLAG.size());
            if (repeatedly == "true") {
                params.repeatedly = true;
            }
            filename = filename.substr(0, found);
        }
        params.filename = filename;
    }
    {
        std::scoped_lock _(mParseUrlMutex);
        for (auto it = mUrl2VideoSourceParamsMap.begin(); it != mUrl2VideoSourceParamsMap.end(); ++it) {
            if (it->second.valueEqual(params)) {
                LOG(ERROR) << "duplicate request, url " << it->first;
                return false;
            }
        }
        mUrl2VideoSourceParamsMap[url] = params;
        return true;
    }
}

void VideoManagerService::finishingWork(const std::string& url) {

}
