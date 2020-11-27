#ifndef COMMON_
#define COMMON_

#include <string>
#include <glog/logging.h>

void funcTrace();

enum StreamingMode {
    TCP,
    UDP
};

struct VideoRequest {
    bool _use_file;
    std::string filename;
    bool repeatedly;
    bool _use_gb;
    std::string gbid;
    StreamingMode gbStreamingMode;

    VideoRequest();
    ~VideoRequest();

    static VideoRequest parseUrl(const std::string& url);

    friend std::ostream& operator<<(std::ostream& os, const VideoRequest& request);

    inline bool operator==(const VideoRequest& other) const {
        bool f = (_use_file == other._use_file)
               && (filename == other.filename)
               && (_use_gb == other._use_gb)
               && (gbid == other.gbid);
        if (!f) {
            LOG(INFO) << _use_file << "-" << other._use_file;
            LOG(INFO) << filename << "-" << other.filename;
            LOG(INFO) << _use_gb << "-" << other._use_gb;
        }
        return f;
    }

    inline bool valueAndParamsEqual(const VideoRequest& other) const {
        return (_use_file == other._use_file)
               && (filename == other.filename)
               && (repeatedly == other.repeatedly)
               && (_use_gb == other._use_gb)
               && (gbid == other.gbid)
               && (gbStreamingMode == other.gbStreamingMode);
    }

private:
    std::string toString() const;
};

namespace std
{
    template <>
    struct hash<VideoRequest>
    {
        size_t operator()(const VideoRequest& r) const
        {
            // Compute hash of request string
            std::string requestStr("");
            if (r._use_file) {
                requestStr = "file://" + r.filename;
            } else if (r._use_gb) {
                requestStr = "gb://" + r.gbid;
            }
            return hash<std::string>{}(requestStr);
        }
    };
}

enum RTSPCommand {
    UNKNOWN = 0,
    OPTIONS,
    DESCRIBE,
    SETUP,
    PLAY,
    PAUSE,
    RECORD,
    ANNOUNCE,
    TEAEWORN,
    GET_PARAMETER,
    SET_PARAMETER,
    REDIRECT
};

#endif