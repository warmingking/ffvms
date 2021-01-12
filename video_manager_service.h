#ifndef VIDEO_MANAGER_SERVICE_H
#define VIDEO_MANAGER_SERVICE_H

#include <map>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <functional>
extern "C" {
    #include <libavformat/avformat.h>
}
#include "CTPL/ctpl_stl.h"
#include "common.h"
#include "gb_manager_service.h"

class VideoSink {
public:
    virtual void writeRtpData(const VideoRequest& request, uint8_t* data, size_t len) = 0;
    virtual void streamComplete(const VideoRequest& request) = 0;
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
    bool finish;

    VideoContext(bool repeatedly, int videoStreamId, std::pair<uint32_t, uint32_t> fps, AVFormatContext* iFmtCtx, AVFormatContext* oFmtCtx)
        : repeatedly(repeatedly)
          , videoStreamId(videoStreamId)
          , fps(fps)
          , iFmtCtx(iFmtCtx)
          , oFmtCtx(oFmtCtx)
          , pts(0)
          , finish(false) {
        pkt = new AVPacket;
        av_init_packet(pkt);
        pkt->data = NULL;
        pkt->size = 0;
        buf = (unsigned char*) av_malloc(bufSize);
    }

    // TODO: deconstruct
};

using AddVideoSourceCallback = std::function<void(const VideoRequest& request, const int ret, const std::string& sdp)>;

class VideoManagerService {
public:
    // VideoManagerService();

    VideoManagerService(VideoSink* sink);

    void InitializeRpc(const std::string& rpcFromHost, const uint16_t& rpcFromPort);

    void addAndProbeVideoSourceAsync(const VideoRequest& request, const AddVideoSourceCallback& callback);

    int getVideoFps(const VideoRequest& request, std::pair<uint32_t, uint32_t>& fps);

    void getFrameAsync(const VideoRequest& request);

    void teardownAsync(const VideoRequest& request);

    // TODO: add destructor

private:
    void finishingWork(const std::string& request); // 取流结束后的处理
    void addAndProbeVideoSource(const VideoRequest request, const AddVideoSourceCallback& callback);
    void getFrame(const VideoRequest request);
    int teardown(const VideoRequest request);

    GbManagerService* gbService;

    ctpl::thread_pool* mpAddAndProbeVideoThreadPool;
    ctpl::thread_pool* mpGetAndSendFrameThreadPool;
    ctpl::thread_pool* mpTeardownThreadPool;

    std::mutex mAddVideoCallbackMapMutex;
    std::map<VideoRequest, AddVideoSourceCallback*> mRequest2AddVideoCallbackMap;

    // bool parseAndCheckUrl(const VideoRequest& request, VideoSourceParams& params);
    std::mutex mAddVideoMutex;
    // std::map<std::string, VideoSourceParams> mUrl2VideoSourceParamsMap;
    std::shared_mutex mVideoContextMapMutex;
    std::map<VideoRequest, VideoContext*> mRequest2VideoContextMap;

private:
    static VideoSink* videoSink; // TODO: 1. rtspserver 继承 videosink 2. vms 改为单例
    static int writeCb (void* opaque, uint8_t* buf, int bufsize);
};

#endif
