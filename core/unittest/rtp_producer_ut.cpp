#include "rtp_producer.h"
#include "video_greeter.grpc.pb.h"
#include <atomic>
#include <fmt/core.h>
#include <fmt/format.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

using namespace ffvms;
using namespace ffvms::core;
using namespace google;
using namespace grpc;
using namespace ::testing;

DEFINE_int32(push_local_port, 12300, "push rtp local port begin with");
DEFINE_int32(video_greeter_port, 8899, "video greeter server port");
DEFINE_int32(network_port, 8086, "udp receive port");
DEFINE_int32(network_event_thread_num, 1, "network event thread number");
DEFINE_int32(file_event_thread_num, 1, "file event thread number");
DEFINE_int32(rtp_jitter_in_ms, 200, "rtp jitter in ms");
DEFINE_int32(max_bandwidth_in_mb, 4, "max bandwidth in MB");

// custom popen which can return pid of subprocess
#define READ 0
#define WRITE 1

pid_t popen2(const char *command, char *const argv[], int *infp, int *outfp)
{
    int p_stdin[2], p_stdout[2];
    pid_t pid;

    if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0)
        return -1;

    pid = fork();

    if (pid < 0)
        return pid;
    else if (pid == 0)
    {
        close(p_stdin[WRITE]);
        dup2(p_stdin[READ], STDIN_FILENO);
        close(p_stdout[READ]);
        dup2(p_stdout[WRITE], STDOUT_FILENO);

        execvp(command, argv);
        perror("execvp failed");
        exit(1);
    }

    if (infp == NULL)
        close(p_stdin[WRITE]);
    else
        *infp = p_stdin[WRITE];

    if (outfp == NULL)
        close(p_stdout[READ]);
    else
        *outfp = p_stdout[READ];

    return pid;
}

class VideoGreeterServiceImpl final : public VideoGreeter::Service
{
public:
    Status InviteVideo(ServerContext *context,
                       const InviteVideoRequest *request,
                       InviteVideoResponse *response) override
    {
        std::string inviteId = fmt::format("{}", fmt::ptr(request));
        mPushSessions[inviteId] = RtpSession{portToUse.fetch_add(2), 0};
        response->set_inviteid(inviteId);
        auto *gbInviteRequest = response->mutable_gbinviteresponse();
        gbInviteRequest->set_peerinfo(
            fmt::format("localhost:{}", mPushSessions[inviteId].pushPort));
        gbInviteRequest->set_frequency(90000);
        gbInviteRequest->set_payload(33);
        gbInviteRequest->set_encoding("MP2T");
        return Status::OK;
    }

    Status SendAck(ServerContext *context, const SendAckRequest *request,
                   SendAckResponse *response) override
    {
        auto it = mPushSessions.find(request->inviteid());
        if (it != mPushSessions.end())
        {
            std::string command = "/workspaces/ffvms/push_rtp.sh";
            char *const argv[] = {
                const_cast<char *>(command.c_str()),
                const_cast<char *>(
                    fmt::format("{}", it->second.pushPort).c_str()),
                NULL};
            it->second.pushPid = popen2(command.c_str(), argv, NULL, NULL);
            LOG(INFO) << "request " << request->inviteid() << " push pid "
                      << mPushSessions[request->inviteid()].pushPid;
            return Status::OK;
        }
        else
        {
            // TODO: grpc error
            return Status::OK;
        }
    }

    Status TearDown(ServerContext *context, const TearDownRequest *request,
                    TearDownResponse *response) override
    {
        auto it = mPushSessions.find(request->inviteid());
        if (it != mPushSessions.end() && it->second.pushPid != 0)
        {
            kill(it->second.pushPid, SIGTERM);
            mPushSessions.erase(it);
        }
        return Status::OK;
    }

    VideoGreeterServiceImpl(int local_port_begin_with)
        : portToUse(local_port_begin_with)
    {
    }

private:
    struct RtpSession
    {
        int pushPort;
        pid_t pushPid;
    };
    std::atomic_int portToUse;
    std::map<std::string, RtpSession> mPushSessions;
};

class RtpProducerTest : public Test
{
private:
    void SetUp() override
    {
        // mock server
        mpService =
            std::make_unique<VideoGreeterServiceImpl>(FLAGS_push_local_port);
        ServerBuilder builder;
        builder.AddListeningPort(
            fmt::format("0.0.0.0:{}", FLAGS_video_greeter_port),
            InsecureServerCredentials());
        builder.RegisterService(mpService.get());
        mpServer = builder.BuildAndStart();
        ASSERT_TRUE(mpServer);
        LOG(INFO) << "start greeter server on port "
                  << FLAGS_video_greeter_port;

        NetworkServer::Config network_config;
        network_config.port = FLAGS_network_port;
        network_config.event_thread_num = FLAGS_network_event_thread_num;
        pNetworkServer = std::make_shared<NetworkServer>();
        pNetworkServer->initUdpServer(network_config);

        RtpProducer::Config rtp_producer_config;
        rtp_producer_config.file_event_thread_num = FLAGS_file_event_thread_num;
        rtp_producer_config.network_receive_host =
            fmt::format("127.0.0.1:{}", FLAGS_network_port);
        rtp_producer_config.sipper_host =
            fmt::format("127.0.0.1:{}", FLAGS_video_greeter_port);
        rtp_producer_config.rtp_jitter_in_ms = FLAGS_rtp_jitter_in_ms;
        rtp_producer_config.max_bandwidth_in_mb = FLAGS_max_bandwidth_in_mb;
        pProducer = std::make_unique<RtpProducer>();
        pProducer->Init(rtp_producer_config, pNetworkServer);
    }

    void TearDown() override { mpServer->Shutdown(); }

protected:
    std::unique_ptr<VideoGreeter::Service> mpService;
    std::unique_ptr<grpc::Server> mpServer;

    std::shared_ptr<NetworkServer> pNetworkServer;
    std::unique_ptr<RtpProducer> pProducer;
};

// TEST_F(RtpProducerTest, TEST_FILE_EXIST)
// {
//     bool gotSdp(false), gotPkt(false), gotError(false);
//     VideoRequest videoRequest;
//     videoRequest._use_file = true;
//     videoRequest.filename = "/workspaces/ffvms/tbut.mp4";
//     pProducer->RegisterVideo(
//         videoRequest,
//         [&gotSdp, &videoRequest](std::string &&sdp) {
//             gotSdp = true;
//             LOG(INFO) << "video " << videoRequest << " got sdp: \n" << sdp;
//         },
//         [&gotPkt](const char *data, const size_t len) { gotPkt = true; },
//         [&gotError](std::error_code &&ec) { gotError = true; });

//     std::this_thread::sleep_for(std::chrono::seconds(1));
//     EXPECT_TRUE(gotSdp);
//     EXPECT_TRUE(gotPkt);
//     EXPECT_FALSE(gotError);

//     // UnregisterVideo 后就不应该收到新的 pkt ( 不能严格保证, 因为 event_del
//     // 的同时可能正好在处理 )
//     gotPkt = false;
//     pProducer->UnregisterVideo(videoRequest);
//     std::this_thread::sleep_for(std::chrono::seconds(1));
//     EXPECT_FALSE(gotPkt);
// }

// TEST_F(RtpProducerTest, REGISTER_FILE_NOT_EXIST)
// {
//     bool gotSdp(false), gotPkt(false), gotError(false);
//     VideoRequest videoRequest;
//     videoRequest._use_file = true;
//     videoRequest.filename = "/workspaces/ffvms/no_such_file";
//     pProducer->RegisterVideo(
//         videoRequest,
//         [&gotSdp, &videoRequest](std::string &&sdp) {
//             gotSdp = true;
//             LOG(INFO) << "video " << videoRequest << " got sdp: \n" << sdp;
//         },
//         [&gotPkt](const char *data, const size_t len) { gotPkt = true; },
//         [&gotError](std::error_code &&ec) { gotError = true; });

//     std::this_thread::sleep_for(std::chrono::seconds(1));
//     EXPECT_FALSE(gotSdp);
//     EXPECT_FALSE(gotPkt);
//     EXPECT_TRUE(gotError);

//     // 不需要, 只是验证是否会出错
//     pProducer->UnregisterVideo(videoRequest);
// }

// TEST_F(RtpProducerTest, REGISTER_FILE_NOT_VIDEO)
// {
//     bool gotSdp(false), gotPkt(false), gotError(false);
//     VideoRequest videoRequest;
//     videoRequest._use_file = true;
//     videoRequest.filename = "/workspaces/ffvms/Dockerfile";
//     pProducer->RegisterVideo(
//         videoRequest,
//         [&gotSdp, &videoRequest](std::string &&sdp) {
//             gotSdp = true;
//             LOG(INFO) << "video " << videoRequest << " got sdp: \n" << sdp;
//         },
//         [&gotPkt](const char *data, const size_t len) { gotPkt = true; },
//         [&gotError](std::error_code &&ec) { gotError = true; });

//     std::this_thread::sleep_for(std::chrono::seconds(1));
//     EXPECT_FALSE(gotSdp);
//     EXPECT_FALSE(gotPkt);
//     EXPECT_TRUE(gotError);

//     // 不需要, 只是验证是否会出错
//     pProducer->UnregisterVideo(videoRequest);
// }

TEST_F(RtpProducerTest, TEST_GB)
{
    std::this_thread::sleep_for(std::chrono::seconds(10000000));
    bool gotSdp(false), gotPkt(false), gotError(false);
    VideoRequest videoRequest;
    videoRequest._use_gb = true;
    videoRequest.gbid = "237246";
    pProducer->RegisterVideo(
        videoRequest,
        [&gotSdp, &videoRequest](std::string &&sdp) {
            gotSdp = true;
            LOG(INFO) << "video " << videoRequest << " got sdp: \n" << sdp;
        },
        [&gotPkt](const char *data, const size_t len) { gotPkt = true; },
        [&gotError](std::error_code &&ec) { gotError = true; });

    for (int i = 0; i < 10; i++)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    EXPECT_TRUE(gotSdp);
    EXPECT_TRUE(gotPkt);
    EXPECT_FALSE(gotError);

    // UnregisterVideo 后就不应该收到新的 pkt ( 不能严格保证, 因为 event_del
    // 的同时可能正好在处理 )
    gotPkt = false;
    pProducer->UnregisterVideo(videoRequest);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_FALSE(gotPkt);
}

int main(int argc, char **argv)
{
    FLAGS_logtostderr = 1;
    InitGoogleLogging(argv[0]);
    ParseCommandLineFlags(&argc, &argv, true);
    InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
