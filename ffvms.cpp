#include <gflags/gflags.h>
#include <glog/logging.h>
#include "rtsp_server.h"

int main (int argc, char *argv[])
{
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    int port = 8554;
    LOG(INFO) << "start rtsp server on port " << port;
    RTSPServer& rtspServer = RTSPServer::getInstance();
    rtspServer.start(port);
    return 0;
}
