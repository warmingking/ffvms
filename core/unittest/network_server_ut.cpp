#include "network_server.h"
#include <fmt/core.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

using namespace ffvms::core;
using namespace google;
using namespace testing;

DEFINE_int32(port, 8086, "udp receive port");
DEFINE_int32(send_port, 12345, "port to send data");
DEFINE_int32(event_thread_num, 1, "event thread number");

class NetworkServerTest : public Test
{
public:
    std::unique_ptr<NetworkServer> pServer;

private:
    void SetUp() override
    {
        NetworkServer::Config config;
        config.port = FLAGS_port;
        config.event_thread_num = FLAGS_event_thread_num;
        pServer = std::make_unique<NetworkServer>();
        pServer->initUdpServer(config);
    }

    void TearDown() override {}
};

TEST_F(NetworkServerTest, REGISTER)
{
    bool succReceive(false);
    pServer->registerPeer(
        fmt::format("127.0.0.1:{}", FLAGS_send_port),
        [&succReceive](const char *data, const size_t len) {
            LOG(INFO) << "receive data " << data;
            succReceive = true;
        });

    std::string request = "hello wahaha";
    std::string cmd = fmt::format("echo \"{}\" | nc -p {} -uw0 127.1 {}", request, FLAGS_send_port, FLAGS_port);
    system(cmd.c_str());

    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_TRUE(succReceive);
}

int main(int argc, char **argv)
{
    FLAGS_logtostderr = 1;
    InitGoogleLogging(argv[0]);
    ParseCommandLineFlags(&argc, &argv, true);
    InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
