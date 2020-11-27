#ifndef RTSP_PARSER_H
#define RTSP_PARSER_H

#include <map>
#include <memory>
#include <utility>
#include "http-parser/http_parser.h"
#include "common.h"

struct BaseCommand {
    size_t cseq;
    std::string url;
    VideoRequest videoRequest;
    std::string session;
    size_t streamid;
    StreamingMode streamingMode;
    uint16_t rtpPort;
    std::string lastHeader;

    std::string toString() const;
};

class RTSPParser {
public:
    static size_t execteParse(const char *data, size_t len, std::pair<RTSPCommand, BaseCommand>& parsedCommand);

private:
    static int message_begin_cb(http_parser* p);
    static int url_cb(http_parser* p, const char* data, size_t len);
    static int status_cb(http_parser* p, const char * data, size_t len);
    static int header_field_cb(http_parser* p, const char * data, size_t len);
    static int header_value_cb(http_parser* p, const char * data, size_t len);
    static int headers_complete_cb(http_parser* p);
    static int body_cb(http_parser* p, const char * data, size_t len);
    static int message_complete_cb(http_parser* p);
    static int chunk_header_cb(http_parser* p);
    static int chunk_complete_cb(http_parser* p);
    static http_parser_settings settings;
};

#endif
