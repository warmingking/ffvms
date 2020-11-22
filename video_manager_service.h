#ifndef VIDEO_MANAGER_SERVICE_H
#define VIDEO_MANAGER_SERVICE_H

#include <map>
#include <list>
#include <mutex>
#include <string>
extern "C" {
    #include <libavformat/avformat.h>
}
#include "common.h"

struct VideoSourceParams {
    bool _use_file;
    std::string filename;
    bool repeatedly;
    bool _use_gb;
    std::string gbid;
    StreamingMode gbStreamingMode;

    inline void reset() {
        _use_file = false;
        _use_gb = false;
    }

    inline bool valueEqual(const VideoSourceParams& other) const {
        return (_use_file == other._use_file)
               && (filename == other.filename)
               && (_use_gb == other._use_gb)
               && (gbid == other.gbid);
    }

    // inline bool valueAndParamsEqual(const VideoSourceParams& other) const {
    //     return (_use_file == other._use_file)
    //            && (filename == other.filename)
    //            && (repeatedly == other.repeatedly)
    //            && (_use_gb == other._use_gb)
    //            && (gbid == other.gbid)
    //            && (gbStreamingMode == other.gbStreamingMode);
    // }
};

// enum CodecType {
//     H264,
//     HEVC
// };

// struct VideoMeta {
//     std::pair<uint32_t, uint32_t> fps;
//     CodecType codec;
//     double duration;
//     void* data; // custom data

//     std::string toString() const {
//         return "fps: " + std::to_string(fps.first)
//                 + "/" + std::to_string(fps.second)
//                 + ",\tcodec: " + std::to_string(codec)
//                 + ",\tduration: " + std::to_string(duration);
//     }
// };

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

    VideoContext(bool repeatedly, int videoStreamId, std::pair<uint32_t, uint32_t> fps, AVFormatContext* iFmtCtx, AVFormatContext* oFmtCtx)
        : repeatedly(repeatedly)
          , videoStreamId(videoStreamId)
          , fps(fps)
          , iFmtCtx(iFmtCtx)
          , oFmtCtx(oFmtCtx) {
        pkt = new AVPacket;
        av_init_packet(pkt);
        pkt->data = NULL;
        pkt->size = 0;
        buf = (unsigned char*) av_malloc(bufSize);
    }

    // TODO: deconstruct
};

typedef void (*AddVideoSourceCallback) (const std::pair<const std::string*, const std::string*>& url2sdp, const void* client);

class VideoManagerService {
public:
    void InitializeRpc(const std::string& rpcFromHost, const uint16_t& rpcFromPort);

    void addVideoSource(const std::string* url, AddVideoSourceCallback callback, const void* client);

    int getVideoFps(const std::string& url, std::pair<uint32_t, uint32_t>& fps);

    int getFrame(const std::string& url);

private:
    void finishingWork(const std::string& url); // 取流结束后的处理
    bool parseAndCheckUrl(const std::string& url, VideoSourceParams& params);
    std::mutex mParseUrlMutex;
    std::map<std::string, VideoSourceParams> mUrl2VideoSourceParamsMap;
    std::mutex mVideoContextMutex;
    std::map<std::string, VideoContext*> mUrl2VideoContextMap;


private:
    static std::map<void*, std::string> opaque2urlMap;
    static int writeCb (void* opaque, uint8_t* buf, int bufsize);
};

#endif