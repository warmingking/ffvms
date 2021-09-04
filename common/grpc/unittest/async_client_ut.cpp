#include "async_client.h"
#include "greeter.grpc.pb.h"
#include <fmt/core.h>
#include <future>
#include <glog/logging.h>
#include <gtest/gtest.h>

using namespace common::grpc;
using namespace ffvms;
using namespace google;
using namespace grpc;
using namespace ::testing;

DEFINE_int32(mock_port, 50000, "greeter server port");
DEFINE_string(name, "wahaha", "name send to greeter server");
DEFINE_string(prefix, "hello", "greet prefix");

class GreeterServiceImpl final : public Greeter::Service
{
    Status SayHello(ServerContext *context, const HelloRequest *request,
                    HelloResponse *response) override
    {
        response->set_message(
            fmt::format("{} {}", FLAGS_prefix, request->name()));
        return Status::OK;
    }
};

class AsyncClientTest : public Test
{
    void SetUp() override
    {
        // mock server
        mpService = std::make_unique<GreeterServiceImpl>();
        ServerBuilder builder;
        builder.AddListeningPort(fmt::format("0.0.0.0:{}", FLAGS_mock_port),
                                 InsecureServerCredentials());
        builder.RegisterService(mpService.get());
        mpServer = builder.BuildAndStart();
        ASSERT_TRUE(mpServer);
        LOG(INFO) << "start greeter server on port " << FLAGS_mock_port;

        // 构造 client
        pClient = std::make_unique<AsyncClient>(1);
        mpStub = std::move(Greeter::NewStub(
            grpc::CreateChannel(fmt::format("127.0.0.1:{}", FLAGS_mock_port),
                                grpc::InsecureChannelCredentials())));
    }

    void TearDown() override { mpServer->Shutdown(); }

protected:
    std::unique_ptr<AsyncClient> pClient;
    std::unique_ptr<Greeter::Stub> mpStub;
    std::unique_ptr<Greeter::Service> mpService;
    std::unique_ptr<grpc::Server> mpServer;
};

TEST_F(AsyncClientTest, ASYNC_HELLO_GREET)
{
    std::promise<std::string> promise;
    auto future = promise.get_future();
    HelloRequest request;
    request.set_name(FLAGS_name);
    pClient->Call<HelloRequest, HelloResponse>(
        [this](ClientContext *context, const HelloRequest &request,
               CompletionQueue *cq) {
            return mpStub->PrepareAsyncSayHello(context, request, cq);
        },
        request, 0,
        [&promise](std::unique_ptr<HelloResponse> &&response,
                   std::unique_ptr<::grpc::Status> &&error) {
            if (error && !error->ok())
            {
                LOG(ERROR) << "grpc call failed"
                           << ", error code: " << error->error_code()
                           << ", error message: " << error->error_message();
                promise.set_value(""); // TODO: set exception
                return;
            }
            LOG(INFO) << "greeter server response \"" << response->message()
                      << "\"";
            promise.set_value(response->message());
        });
    EXPECT_EQ(future.get(), fmt::format("{} {}", FLAGS_prefix, FLAGS_name));
}

int main(int argc, char **argv)
{
    FLAGS_logtostderr = 1;
    InitGoogleLogging(argv[0]);
    InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}