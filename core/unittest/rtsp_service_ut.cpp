#include "rtsp_service.h"
#include <fmt/core.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

using namespace ffvms::core;
using namespace google;
using namespace ::testing;

DEFINE_int32(rtsp_port, 8554, "rtsp port");
DEFINE_int32(event_thread_num, 1, "event thread number");

class RtspServiceTest : public Test
{
    void SetUp() override
    {
        RtspService::Config config;
        config.rtsp_port = FLAGS_rtsp_port;
        config.event_thread_num = FLAGS_event_thread_num;
        pService = std::make_unique<RtspService>();
        pService->Init(config, pMockRtpProducer);
    }

    void TearDown() override {}

public:
    std::shared_ptr<RtpProducer> pMockRtpProducer;
    std::unique_ptr<RtspService> pService;
};

TEST_F(RtspServiceTest, RTSP_PARSER_OPTIONS)
{
    std::string request =
        "OPTIONS rtsp://example.com/media.mp4?repeated=true RTSP/1.0\n"
        "CSeq: 1\n"
        "Require: implicit-play\n"
        "Proxy-Require: gzipped-messages\n";

    // std::string request = "DESCRIBE rtsp://example.com/media.mp4 RTSP/1.0\n"
    //                       "CSeq: 2\n";

    std::string cmd = fmt::format("echo \"{}\" | nc -w0 127.1 8554", request);

    system(cmd.c_str());
    EXPECT_TRUE(true);
}

int main(int argc, char **argv)
{
    FLAGS_logtostderr = 1;
    InitGoogleLogging(argv[0]);
    ParseCommandLineFlags(&argc, &argv, true);
    InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
