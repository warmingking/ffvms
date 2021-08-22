#include <chrono>
#include "thread_pool.h"
#include <glog/logging.h>
#include <gtest/gtest.h>

using namespace common;
using namespace google;
using namespace ::testing;

DEFINE_int32(thread_pool_capacity, 10, "thread pool capacity");

class ThreadPoolTest : public Test
{
    void SetUp() override
    {
        LOG(INFO) << "init thread pool with size " << FLAGS_thread_pool_capacity;
        mpThreadPool = std::make_unique<ThreadPool>(FLAGS_thread_pool_capacity);
    }

    void TearDown() override {}

protected:
    std::unique_ptr<ThreadPool> mpThreadPool;
};

TEST_F(ThreadPoolTest, SUBMIT_TASK)
{
    LOG(INFO) << "let's go...";
    for (int i = 0; i < FLAGS_thread_pool_capacity; i++)
    {
        mpThreadPool->submit([i]()
        {
            std::this_thread::sleep_for(std::chrono::seconds(FLAGS_thread_pool_capacity - i));
            LOG(INFO) << "===== " << i;
        });
    }
    LOG(INFO) << "submit all tasks...";
    LOG(INFO) << "we all done!";
}

int main(int argc, char **argv)
{
    FLAGS_logtostderr = 1;
    InitGoogleLogging(argv[0]);
    ParseCommandLineFlags(&argc, &argv, true);
    InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
