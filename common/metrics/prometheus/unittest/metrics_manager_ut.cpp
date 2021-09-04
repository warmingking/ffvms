#include "metrics_manager.h"
#include <algorithm>
#include <fmt/core.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <random>
#include <thread>

using namespace common::metrics::prometheus;
using namespace google;
using namespace ::testing;

DEFINE_int32(exposer_port, 23456, "");
DEFINE_string(family_prefix, "mm_test", "");
DEFINE_string(endpoint_uri, "", "endpoint uri, if empty, default metrics");

class MetricsManagerTest : public Test
{
    void SetUp() override
    {
        MetricsManager::Options options;
        options.exposer_port = FLAGS_exposer_port;
        options.family_prefix = FLAGS_family_prefix;
        options.endpoint_uri = FLAGS_endpoint_uri;
        MetricsManager::Init(options);
    }

    void TearDown() override {}
};

TEST_F(MetricsManagerTest, METRICS_COMMON)
{
    std::map<std::string, std::string> labels;
    labels["wahaha"] = "yixixi";
    auto pCounter = MMAddCounter("mm_test_counter", labels);
    pCounter->Increment();

    auto pSummary = MMAddSummary(
        "mm_test_latency", labels,
        prometheus::Summary::Quantiles{{0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}});
    {
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> d(0, 100);
    for (int i = 0; i < 20; ++i)
    {
        LatencyMeasureMsScoped<prometheus::Summary> _(*pSummary);
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(d(gen))));
    }

    std::string uri = FLAGS_endpoint_uri;
    if (uri.empty())
    {
        uri = "metrics";
    }

    // sleep to check
    LOG(INFO) << "it's time to check "
              << fmt::format("http://127.0.0.1:{}/{}", FLAGS_exposer_port, uri);
    // std::this_thread::sleep_for(std::chrono::hours(1));
}

int main(int argc, char **argv)
{
    FLAGS_logtostderr = 1;
    InitGoogleLogging(argv[0]);
    InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}