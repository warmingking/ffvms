#include "rtsp_service.h"
#include <filesystem>
#include <fstream>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <memory>
#include <nlohmann/json.hpp>

using namespace ffvms::core;
using nlohmann::json;
namespace fs = std::filesystem;

DEFINE_string(config_file, "config.json", "config file path");

int main(int argc, char *argv[])
{
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (!fs::exists(FLAGS_config_file))
    {
        LOG(FATAL) << "config file " << FLAGS_config_file
                   << " does not exist, quit now";
    }

    std::ifstream ifs(FLAGS_config_file);
    LOG(INFO) << "start ffvms with config file " << FLAGS_config_file
              << ", config content\n"
              << ifs.rdbuf();
    ifs.seekg(0);
    json config = json::parse(ifs);

    std::shared_ptr<NetworkServer> networkServer =
        std::make_shared<NetworkServer>();
    networkServer->initUDPServer(config["network_server"]);

    std::shared_ptr<RtpProducer> rtpProducer = std::make_shared<RtpProducer>();
    rtpProducer->Init(config["rtp_producer"], networkServer);

    std::unique_ptr<RtspService> rtspService = std::make_unique<RtspService>();
    rtspService->Init(config["rtsp_service"], rtpProducer);
    rtspService->Dispatch();

    return 0;
}
