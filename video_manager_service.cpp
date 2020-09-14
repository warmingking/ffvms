#include <thread>
#include <glog/logging.h>
#include "video_manager_service.h"

const std::string FILE_PREFIX = "file/";

int VideoManagerService::writeCb (void* opaque, uint8_t* buf, int bufsize) {
    LOG(INFO) << "zapeng: write callback, ==============";
}

void VideoManagerService::InitializeRpc(const std::string& rpcFromHost, const uint16_t& rpcFromPort) {
    LOG(INFO) << "TODO: Initialize";
}

void VideoManagerService::addVideoSource(const std::string& url, AddVideoSourceCallback callback, const void* client) {
    VLOG(1) << "addVideoSource. url: " << url;
    VideoMeta videoMeta;
    videoMeta.data = (void*) &url;
    size_t found;
    do {
        if ((found = url.find(FILE_PREFIX)) != std::string::npos) {
            std::string filename = url.substr(found + FILE_PREFIX.size());
            AVFormatContext *iFmtCtx = NULL;
            int ret = avformat_open_input(&iFmtCtx, filename.c_str(), NULL, NULL);
            if (ret < 0) {
                LOG(ERROR) << "open file " << filename << " failed";
                break;
            }
            ret = avformat_find_stream_info(iFmtCtx, NULL);
            if (ret < 0) {
                LOG(ERROR) << "find stream info failed for file: " << filename;
                break;
            }
            av_dump_format(iFmtCtx, 0, filename.c_str(), 0);

            ret = av_find_best_stream(iFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
            if (ret < 0) {
                LOG(ERROR) << "find stream info failed for file: " << filename;
                break;
            }
            AVStream* stream = iFmtCtx->streams[ret];
            videoMeta.fps = std::make_pair<>(stream->avg_frame_rate.num, stream->avg_frame_rate.den);
            videoMeta.duration = stream->duration * av_q2d(stream->time_base);
            switch (stream->codecpar->codec_id) {
                case AV_CODEC_ID_H264: videoMeta.codec = CodecType::H264; break;
                case AV_CODEC_ID_H265: videoMeta.codec = CodecType::HEVC; break;
                default: LOG(ERROR) << "unsupport codec_id " << stream->codecpar->codec_id << ": " << avcodec_get_name(stream->codecpar->codec_id); break;
            }
            VideoContext* videoContext = new VideoContext(ret, videoMeta, iFmtCtx, stream);
            mUrl2VideoContextMap[url] = videoContext;
            callback(videoMeta, client);
        } else {
            LOG(ERROR) << "illegal url " << url << ", can't parse file or rtsp or gb";
            break;
        }
    } while (false);
}

int VideoManagerService::getFrame(const std::string& url, uint8_t* data, size_t& len) {
    if (mUrl2VideoContextMap.count(url) == 0) {
        LOG(ERROR) << "no video context for url " << url;
        return -1;
    }
    int ret;
    VideoContext* videoContext = mUrl2VideoContextMap[url];
    if (videoContext->oFmtCtx == NULL) {
        std::string formatName;
        switch (videoContext->meta.codec) {
            case CodecType::H264:
                formatName = "H264"; break;
            case CodecType::HEVC:
                formatName = "H265"; break;
            default:
                LOG(ERROR) << "unkonwn video codec type: " << videoContext->meta.codec;
                return -1;
        }
        avformat_alloc_output_context2(&videoContext->oFmtCtx, NULL, formatName.c_str(), NULL);
        if (videoContext->oFmtCtx == NULL) {
            LOG(ERROR) << "could not create output context for url: " << url;
            return -1;
        }
        videoContext->oFmtCtx->flags &= AVFMT_NOFILE; // TODO: is this ok?
        AVOutputFormat* oFmt = videoContext->oFmtCtx->oformat;
        AVStream *inStream = videoContext->iVideoStream;
        AVStream* outStream = avformat_new_stream(videoContext->oFmtCtx, NULL);
        if (outStream == NULL) {
            LOG(ERROR) << "failed allocating output stream for url" << url;
            return -1;
        }
        ret = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if (ret < 0) {
            LOG(ERROR) << "failed to copy codec parameters for url" << url;
            return -1;
        }
        outStream->codecpar->codec_tag = 0;
        videoContext->oVideoStream = outStream;
        AVIOContext* oioContext = avio_alloc_context(
            videoContext->buf,
            VideoContext::bufSize,
            1,
            (void*) &url,
            NULL,
            VideoManagerService::writeCb,
            NULL
        );
        videoContext->oFmtCtx->pb = oioContext;
        av_dump_format(videoContext->oFmtCtx, 0, NULL, 1);
        ret = avformat_write_header(videoContext->oFmtCtx, NULL);
        if (ret < 0) {
            LOG(ERROR) << "error occurred when opening output file for url " << url;
            return -1;
        }
    }
    ret = av_read_frame(videoContext->iFmtCtx, videoContext->pkt);
    if (ret < 0)
        return -1;
    if (videoContext->pkt->stream_index != videoContext->videoStreamId) {
        av_packet_unref(videoContext->pkt);
        return -1;
    }
    videoContext->pkt->stream_index = 0;

    /* copy packet */
    videoContext->pkt->pts = av_rescale_q_rnd(videoContext->pkt->pts, videoContext->iVideoStream->time_base, videoContext->oVideoStream->time_base, static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    videoContext->pkt->dts = av_rescale_q_rnd(videoContext->pkt->dts, videoContext->iVideoStream->time_base, videoContext->oVideoStream->time_base, static_cast<AVRounding>(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    videoContext->pkt->duration = av_rescale_q(videoContext->pkt->duration, videoContext->iVideoStream->time_base, videoContext->oVideoStream->time_base);
    videoContext->pkt->pos = -1;
    ret = av_interleaved_write_frame(videoContext->oFmtCtx, videoContext->pkt);
    if (ret < 0) {
        LOG(ERROR) << "error muxing packet for url" << url;
        return -1;
    }
    av_packet_unref(videoContext->pkt);
    data = videoContext->pkt->data;
    len = videoContext->pkt->size;
}
