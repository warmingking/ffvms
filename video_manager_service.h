#ifndef VIDEO_MANAGER_SERVICE_H
#define VIDEO_MANAGER_SERVICE_H

#include <map>
#include <unordered_map>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <string>
extern "C" {
    #include <libavformat/avformat.h>
}
#include "common.h"

class VideoSink {
public:
    virtual void writeRtpData(const VideoRequest& url, uint8_t* data, size_t len) = 0;
    virtual ~VideoSink();
};

struct VideoContext {
    std::mutex mutex;
    static const size_t bufSize = 4*1024*1024;
    bool repeatedly;
    int videoStreamId;
    std::pair<uint32_t, uint32_t> fps;
    AVFormatContext* iFmtCtx;
    AVFormatContext* oFmtCtx;
    // AVStream* iVideoStream;
    // AVStream* oVideoStream;
    AVPacket* pkt;
    unsigned char* buf;
    int64_t pts; // 只有在输入流pts为AV_NOPTS_VALUE时才有效

    VideoContext(bool repeatedly, int videoStreamId, std::pair<uint32_t, uint32_t> fps, AVFormatContext* iFmtCtx, AVFormatContext* oFmtCtx)
        : repeatedly(repeatedly)
          , videoStreamId(videoStreamId)
          , fps(fps)
          , iFmtCtx(iFmtCtx)
          , oFmtCtx(oFmtCtx)
          , pts(0) {
        pkt = new AVPacket;
        av_init_packet(pkt);
        pkt->data = NULL;
        pkt->size = 0;
        buf = (unsigned char*) av_malloc(bufSize);
    }

    // TODO: deconstruct
};

typedef void (*AddVideoSourceCallback) (const std::pair<const VideoRequest*, std::string*>& url2sdp);

class VideoManagerService {
public:
    // VideoManagerService();

    VideoManagerService(VideoSink* sink);

    void InitializeRpc(const std::string& rpcFromHost, const uint16_t& rpcFromPort);

    void addAndProbeVideoSourceAsync(const VideoRequest url, AddVideoSourceCallback callback);

    int getVideoFps(const VideoRequest& url, std::pair<uint32_t, uint32_t>& fps);

    int getFrame(const VideoRequest& url);

    int teardown(const VideoRequest& url);

private:
    void finishingWork(const std::string& url); // 取流结束后的处理
    // bool parseAndCheckUrl(const VideoRequest& url, VideoSourceParams& params);
    std::mutex mAddVideoMutex;
    // std::map<std::string, VideoSourceParams> mUrl2VideoSourceParamsMap;
    std::shared_mutex mVideoContextMapMutex;
    std::map<VideoRequest, VideoContext*> mUrl2VideoContextMap;

private:
    static VideoSink* videoSink; // TODO: 1. rtspserver 继承 videosink 2. vms 改为单例
    static int writeCb (void* opaque, uint8_t* buf, int bufsize);
};

#endif
