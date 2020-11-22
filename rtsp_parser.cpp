#include <memory>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <glog/logging.h>
#include "rtsp_parser.h"

const std::string STREAMID_PATTEN = "/streamid=";

std::string BaseCommand::toString() const {
    return "cseq: " + std::to_string(cseq)
           + ",\turl: " + url
           + ",\tsession:" + session
           + ",\tstreamid: " + std::to_string(streamid)
           + ",\tport: " + std::to_string(rtpPort);
}

int RTSPParser::message_begin_cb(http_parser* p) {
    VLOG(1) << "messge begin cb";
    return 0;
}

int RTSPParser::header_field_cb(http_parser* p, const char* buf, size_t len) {
    VLOG(1) << "header field cb";
    std::string headerOrValue(buf, len);
    std::transform(headerOrValue.begin(), headerOrValue.end(), headerOrValue.begin(), ::tolower);
    std::pair<RTSPCommand, BaseCommand>* parsedCommand = (std::pair<RTSPCommand, BaseCommand>*) p->data;
    std::string& lastHeader = parsedCommand->second.lastHeader;
    if (lastHeader == "") {
        if (headerOrValue == "cseq"
            || headerOrValue == "session"
            || headerOrValue == "transport")
            lastHeader = headerOrValue;
    } else if (lastHeader == "cseq") {
        parsedCommand->second.cseq = std::stoi(headerOrValue);
        lastHeader = "";
    } else if (lastHeader == "session") {
        parsedCommand->second.session = headerOrValue;
        lastHeader = "";
    } else if (lastHeader == "transport") {
        if (headerOrValue.find("tcp") != std::string::npos) {
            parsedCommand->second.streamingMode = StreamingMode::TCP;
        } else {
            parsedCommand->second.streamingMode = StreamingMode::UDP;
            uint16_t p1(0), p2;
            if ((sscanf(headerOrValue.c_str(), "client_port=%hu-%hu", &p1, &p2) == 2)) {
                parsedCommand->second.rtpPort = p1;
            }
        }
        lastHeader = "";
    }
    return 0;
}

int RTSPParser::header_value_cb(http_parser* p, const char* buf, size_t len) {
    VLOG(1) << "header value cb";
    return 0;
}

int RTSPParser::url_cb(http_parser* p, const char* buf, size_t len) {
    VLOG(1) << "url cb";
    std::pair<RTSPCommand, BaseCommand>* parsedCommand = (std::pair<RTSPCommand, BaseCommand>*) p->data;
    struct http_parser_url u;
    http_parser_url_init(&u);
    http_parser_parse_url(buf, len, 0, &u);
    if ((u.field_set >> 3) <= 0) {
        LOG(ERROR) << "invailed url: " << buf;
        return -1;
    }
    const size_t offset = u.field_data[3].off; // alise
    std::string url(&buf[offset], len - offset);
    size_t streamid(0);
    const size_t at = url.find(STREAMID_PATTEN);
    if (at != std::string::npos) {
        streamid = std::stoi(url.substr(at + STREAMID_PATTEN.size()));
        url = url.substr(0, at);
    }
    parsedCommand->second.url = url;
    return 0;
}

int RTSPParser::status_cb(http_parser* p, const char* buf, size_t len) {
    VLOG(1) << "status cb";
    return 0;
}

int RTSPParser::body_cb(http_parser* p, const char* buf, size_t len) {
    VLOG(1) << "body cb";
    return 0;
}

int RTSPParser::headers_complete_cb(http_parser* p) {
    VLOG(1) << "headers complete cb";
    return 0;
}

int RTSPParser::message_complete_cb(http_parser* p) {
    VLOG(1) << "messge complete cb";
    std::pair<RTSPCommand, BaseCommand>* parsedCommand = (std::pair<RTSPCommand, BaseCommand>*) p->data;
    const char* method = http_method_str(static_cast<http_method>(p->method));
    if (strcmp(method, "OPTIONS") == 0)
        parsedCommand->first = RTSPCommand::OPTIONS;
    else if (strcmp(method, "DESCRIBE") == 0)
        parsedCommand->first = RTSPCommand::DESCRIBE;
    else if (strcmp(method, "SETUP") == 0)
        parsedCommand->first = RTSPCommand::SETUP;
    else if (strcmp(method, "PLAY") == 0)
        parsedCommand->first = RTSPCommand::PLAY;
    return 0;
}

int RTSPParser::chunk_header_cb(http_parser* p) {
    VLOG(1) << "chunk header cb";
    return 0;
}

int RTSPParser::chunk_complete_cb(http_parser* p) {
    VLOG(1) << "chunk complete cb";
    return 0;
}
http_parser_settings RTSPParser::settings = {
    .on_message_begin = RTSPParser::message_begin_cb,
    .on_url = RTSPParser::url_cb,
    .on_status = RTSPParser::status_cb,
    .on_header_field = RTSPParser::header_field_cb,
    .on_header_value = RTSPParser::header_field_cb,
    .on_headers_complete = RTSPParser::headers_complete_cb,
    .on_body = RTSPParser::body_cb,
    .on_message_complete = RTSPParser::message_complete_cb,
    .on_chunk_header = RTSPParser::chunk_header_cb,
    .on_chunk_complete = RTSPParser::chunk_complete_cb
};

size_t RTSPParser::execteParse(const char *data, size_t len, std::pair<RTSPCommand, BaseCommand>& parsedcommand) {
    parsedcommand.first = RTSPCommand::UNKNOWN;
    VLOG(1) << "http_parser_execute: \n" << data;
    http_parser* parser = new http_parser;
    http_parser_init(parser, HTTP_BOTH);
    parser->data = (void*) &parsedcommand;
    size_t nparsed = http_parser_execute(parser, &settings, data, len);

    delete parser; // don't worry, will not free parsedcommand
};
