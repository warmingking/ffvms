#include <gflags/gflags.h>
#include <glog/logging.h>
#include "rtsp_server.h"

DEFINE_int32(rtsp_port, 8554, "rtsp listenning port");

int main (int argc, char *argv[])
{
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    LOG(INFO) << "start rtsp server on port " << FLAGS_rtsp_port;
    RTSPServer& rtspServer = RTSPServer::getInstance();
    rtspServer.start(FLAGS_rtsp_port);
    return 0;
}
