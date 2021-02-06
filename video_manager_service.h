#ifndef VIDEO_MANAGER_SERVICE_H
#define VIDEO_MANAGER_SERVICE_H

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
extern "C" {
#include <libavformat/avformat.h>
}
#include <ctpl/ctpl_stl.h>
#include <video_greeter.grpc.pb.h>

#include "common.h"
#include "network_server.h"

class VideoSink {
   public:
    virtual void writeRtpData(const VideoRequest& request, uint8_t* data, size_t len) = 0;
    virtual void streamComplete(const VideoRequest& request) = 0;
    virtual ~VideoSink();
};

struct VideoContext {
    std::mutex mutex;
    static const size_t bufSize = 4 * 1024 * 1024;
    // 请求信息
    std::string filename;
    bool repeatedly;
    std::string peerInfo;
    // 视频信息
    int videoStreamId;
    std::pair<uint32_t, uint32_t> fps;
    AVFormatContext* iFmtCtx;
    AVFormatContext* oFmtCtx;
    // AVStream* iVideoStream;
    // AVStream* oVideoStream;
    AVPacket* pkt;
    unsigned char* buf;
    int64_t pts;  // 只有在输入流pts为AV_NOPTS_VALUE时才有效
    int64_t prePts;
    bool finish;

    VideoContext() : pts(0), prePts(0), finish(false) {
        pkt = new AVPacket;
        av_init_packet(pkt);
        pkt->data = NULL;
        pkt->size = 0;
        buf = (unsigned char*)av_malloc(bufSize);
    }

    ~VideoContext() {
        if (pkt != NULL) {
            av_packet_free(&pkt);
        }
    }
};
using VideoContextPtr = std::shared_ptr<VideoContext>;

using AddVideoSourceCallback = std::function<void(const VideoRequest& request, const int ret, const std::string& sdp)>;

class VideoManagerService {
   public:
    // VideoManagerService();
    VideoManagerService(VideoSink* sink);
    void addAndProbeVideoSourceAsync(const VideoRequest& request, const AddVideoSourceCallback& callback);
    int getVideoFps(const VideoRequest& request, std::pair<uint32_t, uint32_t>& fps);
    void getFrameAsync(const VideoRequest& request);
    void teardownAsync(const VideoRequest& request);
    // TODO: add destructor
   private:
    void finishingWork(const std::string& request);  // 取流结束后的处理
    void addAndProbeVideoSource(const VideoRequest request, const AddVideoSourceCallback callback);
    void getFrame(const VideoRequest request);
    int teardown(const VideoRequest request);

    NetworkServer* networkServer;

    ctpl::thread_pool* mpAddAndProbeVideoThreadPool;
    ctpl::thread_pool* mpGetAndSendFrameThreadPool;
    ctpl::thread_pool* mpTeardownThreadPool;

    // bool parseAndCheckUrl(const VideoRequest& request, VideoSourceParams& params);
    std::mutex mAddVideoMutex;
    // std::map<std::string, VideoSourceParams> mUrl2VideoSourceParamsMap;
    std::shared_mutex mVideoContextMapMutex;
    std::map<VideoRequest, VideoContextPtr> mRequest2VideoContextMap;

    int rtp_open(AVFormatContext **pctx, const std::string& peer, AVDictionary **options);

   private:
    static VideoSink* videoSink;  // TODO: 1. rtspserver 继承 videosink 2. vms 改为单例

   private:
    // rpc
    class AsyncCallData {
       public:
        virtual void onResponse(const grpc::Status& status) = 0;
    };

    void asyncCompleteRpc();

    std::thread rpcThread;
    grpc::CompletionQueue cq_;
    std::unique_ptr<ffvms::videogreeter::Stub> stub_;

    // struct for keeping state and data information
    struct AsyncInviteCallData : public AsyncCallData {
        using InviteCompleteCallback = std::function<void(const std::string& peerInfo, const int error)>;

        VideoRequest videoRequest;
        const InviteCompleteCallback& callback;
        // Container for the data we expect from the server.
        ffvms::InviteResponse response;
        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        grpc::ClientContext context;
        // Storage for the status of the RPC upon completion.
        grpc::Status status;
        std::unique_ptr<grpc::ClientAsyncResponseReader<ffvms::InviteResponse>> response_reader;

        AsyncInviteCallData(const VideoRequest& request, const InviteCompleteCallback& callback);
        void onResponse(const grpc::Status& status);
    };
};

#endif
