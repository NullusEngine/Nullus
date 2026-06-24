#pragma once

#include "BaseDef.h"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Base::Profiling
{
enum class PerformanceStageDomain : uint8_t
{
    Unknown = 0u,
    Prefab,
    Thumbnail
};

enum class PerformanceStageThread : uint8_t
{
    Unknown = 0u,
    Main,
    Background,
    Other
};

enum class PerformanceBenchmarkRunType : uint8_t
{
    Unknown = 0u,
    Baseline,
    Optimized
};

struct NLS_BASE_API PerformanceStageSample
{
    PerformanceStageDomain domain = PerformanceStageDomain::Unknown;
    std::string stageName;
    PerformanceStageThread thread = PerformanceStageThread::Unknown;
    std::chrono::microseconds duration{0};
    std::map<std::string, uint64_t> counters;
};

struct NLS_BASE_API PerformanceStageEntry
{
    PerformanceStageDomain domain = PerformanceStageDomain::Unknown;
    std::string stageName;
    uint64_t callCount = 0u;
    std::chrono::microseconds totalDuration{0};
    std::chrono::microseconds mainThreadDuration{0};
    std::chrono::microseconds backgroundThreadDuration{0};
    std::chrono::microseconds otherThreadDuration{0};
    std::map<std::string, uint64_t> counters;
};

struct NLS_BASE_API PerformanceStageStatsSnapshot
{
    std::vector<PerformanceStageEntry> stages;
};

struct NLS_BASE_API PerformanceBenchmarkRun
{
    std::string scenarioName;
    PerformanceBenchmarkRunType runType = PerformanceBenchmarkRunType::Unknown;
    std::chrono::microseconds totalDuration{0};
    PerformanceStageStatsSnapshot stageStats;
};

struct NLS_BASE_API PerformanceBenchmarkComparison
{
    bool valid = false;
    std::string scenarioName;
    double percentChange = 0.0;
    std::string diagnostic;
};

class NLS_BASE_API PerformanceStageStats
{
public:
    void Record(const PerformanceStageSample& sample);
    void Clear();

    PerformanceStageStatsSnapshot Snapshot() const;
    std::vector<PerformanceStageEntry> TopBottlenecks(
        PerformanceStageDomain domain,
        size_t limit) const;

private:
    struct StageKey
    {
        PerformanceStageDomain domain = PerformanceStageDomain::Unknown;
        std::string stageName;
    };

    struct StageKeyLess
    {
        bool operator()(const StageKey& lhs, const StageKey& rhs) const;
    };

    using StageMap = std::map<StageKey, PerformanceStageEntry, StageKeyLess>;

    static bool IsPreferredBottleneckOrder(
        const PerformanceStageEntry& lhs,
        const PerformanceStageEntry& rhs);

    StageMap m_stages;
    mutable std::mutex m_mutex;
};

class NLS_BASE_API PerformanceStageStatsCapture
{
public:
    explicit PerformanceStageStatsCapture(PerformanceStageStats& stats);
    ~PerformanceStageStatsCapture();

    PerformanceStageStatsCapture(const PerformanceStageStatsCapture&) = delete;
    PerformanceStageStatsCapture& operator=(const PerformanceStageStatsCapture&) = delete;

    PerformanceStageStatsCapture(PerformanceStageStatsCapture&&) = delete;
    PerformanceStageStatsCapture& operator=(PerformanceStageStatsCapture&&) = delete;

private:
    PerformanceStageStats* m_previous = nullptr;
};

class NLS_BASE_API PerformanceStageScope
{
public:
    PerformanceStageScope(
        PerformanceStageDomain domain,
        std::string_view stageName,
        PerformanceStageThread thread = PerformanceStageThread::Main);
    ~PerformanceStageScope();

    PerformanceStageScope(const PerformanceStageScope&) = delete;
    PerformanceStageScope& operator=(const PerformanceStageScope&) = delete;

    PerformanceStageScope(PerformanceStageScope&& other) noexcept;
    PerformanceStageScope& operator=(PerformanceStageScope&& other) noexcept;

    void AddCounter(std::string_view name, uint64_t value = 1u);
    static PerformanceStageStats* GetActiveStats();

private:
    using Clock = std::chrono::steady_clock;

    void End();

    PerformanceStageStats* m_stats = nullptr;
    PerformanceStageSample m_sample;
    Clock::time_point m_start;
    bool m_active = false;
};

NLS_BASE_API PerformanceBenchmarkComparison ComparePerformanceRuns(
    const PerformanceBenchmarkRun* baseline,
    const PerformanceBenchmarkRun* optimized);

NLS_BASE_API std::vector<std::string> FindMissingPerformanceStages(
    const PerformanceStageStatsSnapshot& snapshot,
    PerformanceStageDomain domain,
    std::initializer_list<std::string_view> requiredStages);

NLS_BASE_API std::string FormatPerformanceBenchmarkReport(
    const PerformanceBenchmarkRun& run,
    size_t bottleneckLimit = 5u);

NLS_BASE_API std::string FormatPerformanceComparisonReport(
    const PerformanceBenchmarkRun* baseline,
    const PerformanceBenchmarkRun* optimized,
    size_t bottleneckLimit = 5u);
}
