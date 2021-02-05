#include <execinfo.h>
#include "common.h"

const std::string FILE_PREFIX = "file/";
const std::string GB_PREFIX = "gb/";
const std::string REPEATEDLY_FLAG = "?repeatedly=";

std::ostream &operator<<(std::ostream &os, const VideoRequest &request)
{
    return os << request.toString();
};

VideoRequest::VideoRequest()
    : _use_file(false), filename(""), repeatedly(false), _use_gb(false), gbid(""), gbStreamingMode(StreamingMode::UDP) {}

VideoRequest::~VideoRequest() {}

VideoRequest VideoRequest::parseUrl(const std::string &url)
{
    VideoRequest params;
    size_t found;
    if ((found = url.find(FILE_PREFIX)) != std::string::npos)
    {
        params._use_file = true;
        params.repeatedly = false;
        std::string filename = url.substr(found + FILE_PREFIX.size());
        if ((found = filename.find(REPEATEDLY_FLAG)) != std::string::npos)
        {
            std::string repeatedly = filename.substr(found + REPEATEDLY_FLAG.size());
            if (repeatedly == "true")
            {
                params.repeatedly = true;
            }
            filename = filename.substr(0, found);
        }
        params.filename = filename;
    } else if ((found = url.find(GB_PREFIX)) != std::string::npos) {
        params._use_gb = true;
        std::string gbid = url.substr(found + GB_PREFIX.size());
        params.gbid = gbid;
    }
    return params;
}

std::string VideoRequest::toString() const
{
    if (_use_file)
    {
        return "file://" + filename + "?repeatedly=" + (repeatedly ? "true" : "false");
    }
    else if (_use_gb)
    {
        std::string prefix = "gb://" + gbid + "&gbStreamingMode=";
        if (gbStreamingMode == StreamingMode::TCP)
        {
            return prefix + "TCP";
        }
        else
        {
            return prefix + "UDP";
        }
    }
    return "";
}

void funcTrace()
{
    int nptrs;
    void *buffer[100];
    char **strings;

    nptrs = backtrace(buffer, 100);
    printf("backtrace() returned %d addresses\n", nptrs);

    /* The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
        would produce similar output to the following: */

    strings = backtrace_symbols(buffer, nptrs);
    if (strings == NULL)
    {
        LOG(ERROR) << "backtrace_symbols";
        free(strings);
        return;
    }

    for (int j = 0; j < nptrs; j++)
    {
        LOG(INFO) << strings[j];
    }

    free(strings);
}