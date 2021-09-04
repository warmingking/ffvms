#ifndef __METRICS_MANAGER_H__
#define __METRICS_MANAGER_H__

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/summary.h>

namespace common
{
namespace metrics
{
namespace prometheus
{

template <typename ObserveAble> class LatencyMeasureMsScoped
{
public:
    LatencyMeasureMsScoped(
        typename std::enable_if<std::is_same<ObserveAble, ::prometheus::Summary>::value
                                    || std::is_same<ObserveAble, ::prometheus::Histogram>::value,
                                ObserveAble>::type &observeAble);
    ~LatencyMeasureMsScoped();

private:
    ObserveAble &mObserveAble;
    std::chrono::time_point<std::chrono::high_resolution_clock> mStartTime;
};

class MetricsManager
{
public:
    struct Options
    {
        int exposer_port;
        std::string family_prefix;
        // if empty, use default(metrics)
        // TODO: use std::optional
        std::string endpoint_uri;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Options, exposer_port, family_prefix, endpoint_uri)
    };

public:
    static void Init(const Options &options);
    static MetricsManager &GetInstance();

    using UPCounter =
        std::unique_ptr<::prometheus::Counter, std::function<void(::prometheus::Counter *)>>;
    using UPGauge =
        std::unique_ptr<::prometheus::Gauge, std::function<void(::prometheus::Gauge *)>>;
    using UPHistogram =
        std::unique_ptr<::prometheus::Histogram, std::function<void(::prometheus::Histogram *)>>;
    using UPSummary =
        std::unique_ptr<::prometheus::Summary, std::function<void(::prometheus::Summary *)>>;

    MetricsManager(int exposerPort, const std::string &familyPrefix,
                   const std::string &endpoint_uri);
    UPCounter AddCounter(const std::string &name, std::map<std::string, std::string> &labele);
    UPGauge AddGauge(const std::string &name, std::map<std::string, std::string> &labels);
    UPHistogram AddHistogram(const std::string &name, std::map<std::string, std::string> &labels,
                               const ::prometheus::Histogram::BucketBoundaries &buckets);
    UPSummary AddSummary(const std::string &name, std::map<std::string, std::string> &labels,
                           const ::prometheus::Summary::Quantiles &quantiles,
                           const std::chrono::milliseconds &max_age = std::chrono::seconds{60},
                           const int age_buckets = 5);

private:
    MetricsManager(const MetricsManager &);
    MetricsManager(MetricsManager &&);
    MetricsManager &operator=(const MetricsManager &);
    MetricsManager &operator=(MetricsManager &&);

    static std::once_flag sInstanceCreated;
    static std::unique_ptr<MetricsManager> spInstance;
    std::unique_ptr<::prometheus::Exposer> mpExposer;
    std::shared_ptr<::prometheus::Registry> mpRegistry;
    ::prometheus::Family<::prometheus::Counter> *mpCounterFamily;
    ::prometheus::Family<::prometheus::Gauge> *mpGaugeFamily;
    ::prometheus::Family<::prometheus::Histogram> *mpHistogramFamily;
    ::prometheus::Family<::prometheus::Summary> *mpSummaryFamily;
};

template <typename ObserveAble>
LatencyMeasureMsScoped<ObserveAble>::LatencyMeasureMsScoped(
    typename std::enable_if<std::is_same<ObserveAble, ::prometheus::Summary>::value
                                || std::is_same<ObserveAble, ::prometheus::Histogram>::value,
                            ObserveAble>::type &observeAble)
    : mObserveAble(observeAble), mStartTime(std::chrono::high_resolution_clock::now())
{
}

template <typename ObserveAble> LatencyMeasureMsScoped<ObserveAble>::~LatencyMeasureMsScoped()
{
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - mStartTime);
    mObserveAble.Observe(duration.count());
}

#define MMAddCounter(...) MetricsManager::GetInstance().AddCounter(__VA_ARGS__);
#define MMAddGauge(...) MetricsManager::GetInstance().AddGauge(__VA_ARGS__);
#define MMAddGistogram(...) MetricsManager::GetInstance().AddHistogram(__VA_ARGS__);
#define MMAddSummary(...) MetricsManager::GetInstance().AddSummary(__VA_ARGS__);

} // namespace prometheus
} // namespace metrics
} // namespace common

#endif // __METRICS_MANAGER_H__