#include "metrics_manager.h"
#include <fmt/core.h>
#include <glog/logging.h>

using namespace common::metrics::prometheus;
using namespace ::prometheus;

std::once_flag MetricsManager::sInstanceCreated;
std::unique_ptr<MetricsManager> MetricsManager::spInstance = nullptr;

void MetricsManager::Init(const MetricsManager::Options &options)
{
    std::call_once(sInstanceCreated, [&]() {
        spInstance = std::make_unique<MetricsManager>(options.exposer_port, options.family_prefix,
                                                      options.endpoint_uri);
        LOG(INFO) << "init metrics manager succ";
    });
}

MetricsManager &MetricsManager::GetInstance() { return *spInstance; }

MetricsManager::MetricsManager(int exposerPort, const std::string &familyPrefix,
                               const std::string &endpointUri)
    : mpRegistry(std::make_shared<Registry>())
{
    std::string address(fmt::format("0.0.0.0:{}", exposerPort));
    mpExposer = std::make_unique<Exposer>(address);
    mpCounterFamily =
        &BuildCounter().Name(fmt::format("{}_counter", familyPrefix)).Register(*mpRegistry);
    mpGaugeFamily = &BuildGauge().Name(fmt::format("{}_gauge", familyPrefix)).Register(*mpRegistry);
    mpHistogramFamily =
        &BuildHistogram().Name(fmt::format("{}_histogram", familyPrefix)).Register(*mpRegistry);
    mpSummaryFamily =
        &BuildSummary().Name(fmt::format("{}_summary", familyPrefix)).Register(*mpRegistry);
    if (endpointUri.empty())
    {
        mpExposer->RegisterCollectable(mpRegistry);
    }
    else
    {
        mpExposer->RegisterCollectable(mpRegistry, endpointUri);
    }
}

MetricsManager::UPCounter MetricsManager::AddCounter(const std::string &name,
                                                     std::map<std::string, std::string> &labels)
{
    labels["metric_name"] = name;
    auto &counter = mpCounterFamily->Add(labels);
    // for avoiding metric to be destructed, deleter do nothing
    return MetricsManager::UPCounter(&counter, [](Counter *) {});
}

MetricsManager::UPGauge MetricsManager::AddGauge(const std::string &name,
                                                 std::map<std::string, std::string> &labels)
{
    labels["metric_name"] = name;
    auto &gauge = mpGaugeFamily->Add(labels);
    // for avoiding metric to be destructed, deleter do nothing
    return MetricsManager::UPGauge(&gauge, [](Gauge *) {});
}

MetricsManager::UPHistogram MetricsManager::AddHistogram(const std::string &name,
                                                         std::map<std::string, std::string> &labels,
                                                         const Histogram::BucketBoundaries &buckets)
{
    labels["metric_name"] = name;
    auto &histogram = mpHistogramFamily->Add(labels, buckets);
    return MetricsManager::UPHistogram(&histogram, [](Histogram *) {});
}

MetricsManager::UPSummary MetricsManager::AddSummary(const std::string &name,
                                                     std::map<std::string, std::string> &labels,
                                                     const Summary::Quantiles &quantiles,
                                                     const std::chrono::milliseconds &max_age,
                                                     const int age_buckets)
{
    labels["metric_name"] = name;
    auto &summary = mpSummaryFamily->Add(labels, quantiles, max_age, age_buckets);
    return MetricsManager::UPSummary(&summary, [](Summary *) {});
}
