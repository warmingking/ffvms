#ifndef COMMON_
#define COMMON_

#include <string>
#include <gflags/gflags.h>
#include <glog/logging.h>

#if __INTELLISENSE__
#pragma diag_suppress 144
#endif

#define FFVMS_SUCC std::error_code{}

void funcTrace();

enum StreamingMode
{
    TCP,
    UDP
};

struct VideoRequest
{
private:
    inline std::string cmpStr() const
    {
        std::string requestStr("");
        if (_use_file)
        {
            requestStr = "file://" + filename;
        }
        else if (_use_gb)
        {
            requestStr = "gb://" + gbid;
        }
        return requestStr;
    }
    std::string toString() const;

public:
    // 文件相关的参数
    bool _use_file;
    std::string filename;
    bool repeatedly;
    // GB相关的参数
    bool _use_gb;
    std::string gbid;
    StreamingMode gbStreamingMode;

    VideoRequest();
    VideoRequest(const VideoRequest &other) = default;
    VideoRequest& operator=(const VideoRequest &other) = default;
    VideoRequest(std::string&& url);
    ~VideoRequest();

    static VideoRequest parseUrl(const std::string &url);

    friend std::ostream &operator<<(std::ostream &os, const VideoRequest &request);

    bool operator<(const VideoRequest &other) const
    {
        return this->cmpStr() < other.cmpStr();
    }

    inline bool valueAndParamsEqual(const VideoRequest &other) const
    {
        return (_use_file == other._use_file) && (filename == other.filename) && (repeatedly == other.repeatedly) && (_use_gb == other._use_gb) && (gbid == other.gbid) && (gbStreamingMode == other.gbStreamingMode);
    }
};

namespace std
{
    template <>
    struct hash<VideoRequest>
    {
        size_t operator()(const VideoRequest &r) const
        {
            // Compute hash of request string
            std::string requestStr("");
            if (r._use_file)
            {
                requestStr = "file://" + r.filename;
            }
            else if (r._use_gb)
            {
                requestStr = "gb://" + r.gbid;
            }
            return hash<std::string>{}(requestStr);
        }
    };
} // namespace std

enum RTSPCommand
{
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