#ifndef VIDEO_MANAGER_SERVICE_H
#define VIDEO_MANAGER_SERVICE_H

#include <map>
#include <string>
extern "C" {
    #include <libavformat/avformat.h>
}

enum CodecType {
    H264,
    HEVC
};

struct VideoMeta {
    std::pair<uint32_t, uint32_t> fps;
    CodecType codec;
    double duration;
    void* data; // custom data

    std::string toString() const {
        return "fps: " + std::to_string(fps.first)
                + "/" + std::to_string(fps.second)
                + ",\tcodec: " + std::to_string(codec)
                + ",\tduration: " + std::to_string(duration);
    }
};

struct VideoContext {
    static const size_t bufSize = 4*1024*1024;
    int videoStreamId;
    VideoMeta meta;
    AVFormatContext* iFmtCtx;
    AVFormatContext* oFmtCtx;
    AVStream* iVideoStream;
    AVStream* oVideoStream;
    AVPacket* pkt;
    unsigned char* buf;

    VideoContext(int videoStreamId, VideoMeta meta, AVFormatContext* iFmtCtx, AVStream* iVideoStream)
        : videoStreamId(videoStreamId)
          , meta(meta)
          , iFmtCtx(iFmtCtx)
          , oFmtCtx(NULL)
          , iVideoStream(iVideoStream)
          , oVideoStream(NULL) {
        pkt = new AVPacket;
        av_init_packet(pkt);
        pkt->data = NULL;
        pkt->size = 0;
        buf = (unsigned char*) av_malloc(bufSize);
    }

    // TODO: deconstruct
};

typedef void (*AddVideoSourceCallback) (const VideoMeta& meta, const void* client);

class VideoManagerService {
public:
    void InitializeRpc(const std::string& rpcFromHost, const uint16_t& rpcFromPort);

    void addVideoSource(const std::string& url, AddVideoSourceCallback callback, const void* client);

    int getFrame(const std::string& url, uint8_t* data, size_t& len);
private:
    std::map<std::string, VideoContext*> mUrl2VideoContextMap;

private:
    static int writeCb (void* opaque, uint8_t* buf, int bufsize);
};

#endif