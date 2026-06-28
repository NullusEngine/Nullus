#include "Profiling/PerformanceStageStats.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

namespace NLS::Base::Profiling
{
namespace
{
thread_local PerformanceStageStats* g_activePerformanceStageStats = nullptr;
thread_local std::shared_ptr<PerformanceStageStatsCaptureState> g_activePerformanceStageStatsState;

bool IsMaximumGaugeCounter(const std::string& name)
{
    return name == "queueDepth" ||
        name == "queueBacklog" ||
        name == "inFlightRequestCount" ||
        name == "cancellationLatency";
}

bool IsMinimumGaugeCounter(const std::string& name)
{
    return name == "cacheWriteBudgetRemaining" ||
        name == "previewRenderBudgetRemaining" ||
        name == "readbackBudgetRemaining";
}

void AggregateCounter(
    std::map<std::string, uint64_t>& counters,
    const std::string& name,
    const uint64_t value)
{
    if (IsMaximumGaugeCounter(name))
    {
        auto& aggregate = counters[name];
        aggregate = std::max(aggregate, value);
        return;
    }

    if (IsMinimumGaugeCounter(name))
    {
        const auto found = counters.find(name);
        if (found == counters.end())
            counters.emplace(name, value);
        else
            found->second = std::min(found->second, value);
        return;
    }

    counters[name] += value;
}
}

struct PerformanceStageStatsCaptureState
{
    explicit PerformanceStageStatsCaptureState(PerformanceStageStats& stats)
        : activeStats(&stats)
    {
    }

    PerformanceStageStats* activeStats = nullptr;
    uint32_t activeScopes = 0u;
    std::mutex mutex;
    std::condition_variable idle;
};

bool PerformanceStageStats::StageKeyLess::operator()(const StageKey& lhs, const StageKey& rhs) const
{
    if (lhs.domain != rhs.domain)
        return static_cast<uint8_t>(lhs.domain) < static_cast<uint8_t>(rhs.domain);

    return lhs.stageName < rhs.stageName;
}

bool PerformanceStageStats::IsPreferredBottleneckOrder(
    const PerformanceStageEntry& lhs,
    const PerformanceStageEntry& rhs)
{
    if (lhs.totalDuration != rhs.totalDuration)
        return lhs.totalDuration > rhs.totalDuration;

    if (lhs.domain != rhs.domain)
        return static_cast<uint8_t>(lhs.domain) < static_cast<uint8_t>(rhs.domain);

    if (lhs.stageName != rhs.stageName)
        return lhs.stageName < rhs.stageName;

    if (lhs.callCount != rhs.callCount)
        return lhs.callCount > rhs.callCount;

    return false;
}

void PerformanceStageStats::Record(const PerformanceStageSample& sample)
{
    std::scoped_lock lock(m_mutex);

    const StageKey key{sample.domain, sample.stageName};
    auto& entry = m_stages[key];
    if (entry.callCount == 0u)
    {
        entry.domain = sample.domain;
        entry.stageName = sample.stageName;
    }

    ++entry.callCount;
    entry.totalDuration += sample.duration;

    switch (sample.thread)
    {
    case PerformanceStageThread::Main:
        entry.mainThreadDuration += sample.duration;
        break;
    case PerformanceStageThread::Background:
        entry.backgroundThreadDuration += sample.duration;
        break;
    case PerformanceStageThread::Other:
    case PerformanceStageThread::Unknown:
    default:
        entry.otherThreadDuration += sample.duration;
        break;
    }

    for (const auto& counter : sample.counters)
        AggregateCounter(entry.counters, counter.first, counter.second);
}

void PerformanceStageStats::Clear()
{
    std::scoped_lock lock(m_mutex);
    m_stages.clear();
}

PerformanceStageStatsSnapshot PerformanceStageStats::Snapshot() const
{
    std::scoped_lock lock(m_mutex);

    PerformanceStageStatsSnapshot snapshot;
    snapshot.stages.reserve(m_stages.size());
    for (const auto& [key, entry] : m_stages)
    {
        (void)key;
        snapshot.stages.push_back(entry);
    }

    std::sort(snapshot.stages.begin(), snapshot.stages.end(), IsPreferredBottleneckOrder);
    return snapshot;
}

std::vector<PerformanceStageEntry> PerformanceStageStats::TopBottlenecks(
    PerformanceStageDomain domain,
    size_t limit) const
{
    if (limit == 0u)
        return {};

    const auto snapshot = Snapshot();
    std::vector<PerformanceStageEntry> bottlenecks;
    bottlenecks.reserve(std::min(limit, snapshot.stages.size()));
    for (const auto& entry : snapshot.stages)
    {
        if (entry.domain != domain)
            continue;

        bottlenecks.push_back(entry);
        if (bottlenecks.size() >= limit)
            break;
    }
    return bottlenecks;
}

PerformanceBenchmarkComparison ComparePerformanceRuns(
    const PerformanceBenchmarkRun* baseline,
    const PerformanceBenchmarkRun* optimized)
{
    PerformanceBenchmarkComparison comparison;

    if (baseline == nullptr)
    {
        comparison.diagnostic = "performance-comparison-baseline-missing";
        return comparison;
    }

    if (optimized == nullptr)
    {
        comparison.diagnostic = "performance-comparison-optimized-missing";
        return comparison;
    }

    if (baseline->runType != PerformanceBenchmarkRunType::Baseline ||
        optimized->runType != PerformanceBenchmarkRunType::Optimized)
    {
        comparison.diagnostic = "performance-comparison-run-type-mismatch";
        return comparison;
    }

    if (baseline->scenarioName != optimized->scenarioName)
    {
        comparison.diagnostic = "performance-comparison-scenario-mismatch";
        return comparison;
    }

    if (baseline->totalDuration.count() <= 0)
    {
        comparison.diagnostic = "performance-comparison-baseline-duration-invalid";
        return comparison;
    }

    comparison.valid = true;
    comparison.scenarioName = baseline->scenarioName;
    comparison.percentChange = static_cast<double>(
        (optimized->totalDuration.count() - baseline->totalDuration.count()) * 100.0 /
        baseline->totalDuration.count());
    return comparison;
}

PerformanceStageStatsCapture::PerformanceStageStatsCapture(PerformanceStageStats& stats)
    : m_previous(g_activePerformanceStageStats)
    , m_previousState(g_activePerformanceStageStatsState)
    , m_state(std::make_shared<PerformanceStageStatsCaptureState>(stats))
{
    g_activePerformanceStageStats = &stats;
    g_activePerformanceStageStatsState = m_state;
}

PerformanceStageStatsCapture::~PerformanceStageStatsCapture()
{
    if (m_state)
    {
        std::unique_lock lock(m_state->mutex);
        m_state->activeStats = nullptr;
        m_state->idle.wait(lock, [state = m_state.get()]
        {
            return state->activeScopes == 0u;
        });
    }
    g_activePerformanceStageStats = m_previous;
    g_activePerformanceStageStatsState = m_previousState;
}

PerformanceStageStatsCaptureToken::PerformanceStageStatsCaptureToken(
    std::weak_ptr<PerformanceStageStatsCaptureState> state)
    : m_state(std::move(state))
{
}

bool PerformanceStageStatsCaptureToken::IsValid() const
{
    const auto state = m_state.lock();
    if (!state)
        return false;

    std::scoped_lock lock(state->mutex);
    return state->activeStats != nullptr;
}

PerformanceStageStatsCaptureToken PerformanceStageStatsCapture::GetToken() const
{
    return PerformanceStageStatsCaptureToken(m_state);
}

PerformanceStageStatsCaptureToken PerformanceStageStatsCapture::GetActiveToken()
{
    return PerformanceStageStatsCaptureToken(g_activePerformanceStageStatsState);
}

PerformanceStageStatsCaptureScope::PerformanceStageStatsCaptureScope(
    const PerformanceStageStatsCaptureToken& token)
    : m_previous(g_activePerformanceStageStats)
    , m_previousState(g_activePerformanceStageStatsState)
    , m_state(token.m_state.lock())
{
    if (!m_state)
        return;

    m_lock = std::unique_lock<std::mutex>(m_state->mutex);
    if (m_state->activeStats == nullptr)
        return;

    g_activePerformanceStageStats = m_state->activeStats;
    g_activePerformanceStageStatsState = m_state;
    ++m_state->activeScopes;
    m_lock.unlock();
    m_active = true;
}

PerformanceStageStatsCaptureScope::~PerformanceStageStatsCaptureScope()
{
    if (!m_active)
        return;

    g_activePerformanceStageStats = m_previous;
    g_activePerformanceStageStatsState = m_previousState;
    std::unique_lock lock(m_state->mutex);
    if (m_state->activeScopes > 0u)
        --m_state->activeScopes;
    const bool idle = m_state->activeScopes == 0u;
    lock.unlock();
    if (idle)
        m_state->idle.notify_all();
}

PerformanceStageScope::PerformanceStageScope(
    const PerformanceStageDomain domain,
    const std::string_view stageName,
    const PerformanceStageThread thread)
    : m_stats(g_activePerformanceStageStats)
    , m_start(Clock::now())
    , m_active(m_stats != nullptr)
{
    m_sample.domain = domain;
    m_sample.stageName = std::string(stageName);
    m_sample.thread = thread;
}

PerformanceStageScope::~PerformanceStageScope()
{
    End();
}

PerformanceStageScope::PerformanceStageScope(PerformanceStageScope&& other) noexcept
    : m_stats(other.m_stats)
    , m_sample(std::move(other.m_sample))
    , m_start(other.m_start)
    , m_active(other.m_active)
{
    other.m_stats = nullptr;
    other.m_active = false;
}

PerformanceStageScope& PerformanceStageScope::operator=(PerformanceStageScope&& other) noexcept
{
    if (this == &other)
        return *this;

    End();
    m_stats = other.m_stats;
    m_sample = std::move(other.m_sample);
    m_start = other.m_start;
    m_active = other.m_active;
    other.m_stats = nullptr;
    other.m_active = false;
    return *this;
}

void PerformanceStageScope::AddCounter(const std::string_view name, const uint64_t value)
{
    if (!m_active || name.empty())
        return;

    m_sample.counters[std::string(name)] += value;
}

PerformanceStageStats* PerformanceStageScope::GetActiveStats()
{
    return g_activePerformanceStageStats;
}

void PerformanceStageScope::End()
{
    if (!m_active || m_stats == nullptr)
        return;

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - m_start);
    m_sample.duration = elapsed.count() > 0 ? elapsed : std::chrono::microseconds(1);
    m_stats->Record(m_sample);
    m_active = false;
}

std::vector<std::string> FindMissingPerformanceStages(
    const PerformanceStageStatsSnapshot& snapshot,
    const PerformanceStageDomain domain,
    const std::initializer_list<std::string_view> requiredStages)
{
    std::set<std::string> presentStages;
    for (const auto& stage : snapshot.stages)
    {
        if (stage.domain == domain)
            presentStages.insert(stage.stageName);
    }

    std::vector<std::string> missing;
    for (const auto requiredStage : requiredStages)
    {
        if (presentStages.find(std::string(requiredStage)) == presentStages.end())
            missing.emplace_back(requiredStage);
    }
    std::sort(missing.begin(), missing.end());
    return missing;
}

namespace
{
std::string DomainToString(const PerformanceStageDomain domain)
{
    switch (domain)
    {
    case PerformanceStageDomain::Prefab:
        return "Prefab";
    case PerformanceStageDomain::Thumbnail:
        return "Thumbnail";
    case PerformanceStageDomain::Unknown:
    default:
        return "Unknown";
    }
}

std::string RunTypeToString(const PerformanceBenchmarkRunType runType)
{
    switch (runType)
    {
    case PerformanceBenchmarkRunType::Baseline:
        return "Baseline";
    case PerformanceBenchmarkRunType::Optimized:
        return "Optimized";
    case PerformanceBenchmarkRunType::Unknown:
    default:
        return "Unknown";
    }
}

void AppendCounters(std::ostringstream& stream, const std::map<std::string, uint64_t>& counters)
{
    if (counters.empty())
        return;

    stream << " counters=";
    bool first = true;
    for (const auto& [name, value] : counters)
    {
        if (!first)
            stream << ',';
        first = false;
        stream << name << '=' << value;
    }
}

bool IsPreferredReportBottleneckOrder(
    const PerformanceStageEntry& lhs,
    const PerformanceStageEntry& rhs)
{
    if (lhs.totalDuration != rhs.totalDuration)
        return lhs.totalDuration > rhs.totalDuration;

    if (lhs.domain != rhs.domain)
        return static_cast<uint8_t>(lhs.domain) < static_cast<uint8_t>(rhs.domain);

    if (lhs.stageName != rhs.stageName)
        return lhs.stageName < rhs.stageName;

    if (lhs.callCount != rhs.callCount)
        return lhs.callCount > rhs.callCount;

    return false;
}
}

std::string FormatPerformanceBenchmarkReport(
    const PerformanceBenchmarkRun& run,
    const size_t bottleneckLimit)
{
    std::ostringstream stream;
    stream << "Scenario: " << run.scenarioName << '\n';
    stream << "Run: " << RunTypeToString(run.runType) << '\n';
    stream << "Total: " << run.totalDuration.count() << "us\n";

    const auto snapshot = run.stageStats;
    stream << "Stages:\n";
    for (const auto& stage : snapshot.stages)
    {
        stream << "- [" << DomainToString(stage.domain) << "] "
            << stage.stageName
            << " callCount=" << stage.callCount
            << " total=" << stage.totalDuration.count() << "us"
            << " main=" << stage.mainThreadDuration.count() << "us"
            << " background=" << stage.backgroundThreadDuration.count() << "us"
            << " other=" << stage.otherThreadDuration.count() << "us";
        AppendCounters(stream, stage.counters);
        stream << '\n';
    }

    stream << "TopBottlenecks:\n";
    if (bottleneckLimit > 0u)
    {
        auto top = snapshot.stages;
        std::sort(top.begin(), top.end(), IsPreferredReportBottleneckOrder);
        if (top.size() > bottleneckLimit)
            top.resize(bottleneckLimit);
        for (const auto& stage : top)
        {
            stream << "- " << stage.stageName
                << " total=" << stage.totalDuration.count() << "us"
                << " calls=" << stage.callCount << '\n';
        }
    }

    return stream.str();
}

std::string FormatPerformanceComparisonReport(
    const PerformanceBenchmarkRun* baseline,
    const PerformanceBenchmarkRun* optimized,
    const size_t bottleneckLimit)
{
    const auto comparison = ComparePerformanceRuns(baseline, optimized);
    std::ostringstream stream;
    stream << "Comparison: ";
    if (!comparison.valid)
    {
        stream << "invalid " << comparison.diagnostic << '\n';
        return stream.str();
    }

    stream << comparison.scenarioName
        << " change=" << comparison.percentChange << "%\n";
    if (baseline != nullptr)
        stream << "BaselineTotal=" << baseline->totalDuration.count() << "us\n";
    if (optimized != nullptr)
        stream << "OptimizedTotal=" << optimized->totalDuration.count() << "us\n";
    stream << "BottleneckLimit=" << bottleneckLimit << '\n';
    return stream.str();
}
}
