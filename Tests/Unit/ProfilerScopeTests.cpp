#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"

namespace
{
struct RecordedScopeEvent
{
    std::string phase;
    std::string name;
    uint32_t depth = 0u;
};

class RecordingProfilerDestination final : public NLS::Base::Profiling::IProfilerDestination
{
public:
    void BeginScope(const NLS::Base::Profiling::ProfilerScopeEvent& event) override
    {
        std::lock_guard lock(mutex);
        events.push_back({ "begin", event.name, event.depth });
    }

    void EndScope(const NLS::Base::Profiling::ProfilerScopeEvent& event) override
    {
        std::lock_guard lock(mutex);
        events.push_back({ "end", event.name, event.depth });
    }

    void BeginGpuScope(const NLS::Base::Profiling::ProfilerGpuScopeEvent& event) override
    {
        std::lock_guard lock(mutex);
        events.push_back({ "gpu-begin", event.name, event.depth });
    }

    void EndGpuScope(const NLS::Base::Profiling::ProfilerGpuScopeEvent& event) override
    {
        std::lock_guard lock(mutex);
        events.push_back({ "gpu-end", event.name, event.depth });
    }

    NLS::Base::Profiling::ProfilerDestinationState GetState() const override
    {
        return {
            NLS::Base::Profiling::ProfilerDestinationId::Timeline,
            true,
            NLS::Base::Profiling::ProfilerAvailability::Available,
            NLS::Base::Profiling::ProfilerCapability_CPUScopes | NLS::Base::Profiling::ProfilerCapability_GPUScopes,
            ""
        };
    }

    std::mutex mutex;
    std::vector<RecordedScopeEvent> events;
};

class ProfilerScopeTest : public testing::Test
{
protected:
    void SetUp() override
    {
        NLS::Base::Profiling::Profiler::ResetForTesting();
    }

    void TearDown() override
    {
        NLS::Base::Profiling::Profiler::ResetForTesting();
    }
};
}

TEST_F(ProfilerScopeTest, ScopedObjectEndsScopeWhenLeavingBlock)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    {
        ProfilerScope scope("Scoped Block", __FUNCTION__);
        ASSERT_EQ(destination.events.size(), 1u);
        EXPECT_EQ(destination.events[0].phase, "begin");
        EXPECT_EQ(destination.events[0].name, "Scoped Block");
    }

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_EQ(destination.events[1].phase, "end");
    EXPECT_EQ(destination.events[1].name, "Scoped Block");
}

TEST_F(ProfilerScopeTest, DefaultScopeMacroUsesCallingFunctionName)
{
    using namespace NLS::Base::Profiling;

#if defined(NLS_ENABLE_PROFILING)
    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    {
        NLS_PROFILE_SCOPE();
    }

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_FALSE(destination.events[0].name.empty());
    EXPECT_EQ(destination.events[1].name, destination.events[0].name);
#else
    NLS_PROFILE_SCOPE();
    SUCCEED() << "Profiling macros compile to no-ops when NLS_ENABLE_PROFILING is disabled.";
#endif
}

TEST_F(ProfilerScopeTest, DefaultScopeMacroCanBeUsedTwiceInOneScope)
{
    using namespace NLS::Base::Profiling;

#if defined(NLS_ENABLE_PROFILING)
    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    NLS_PROFILE_SCOPE();
    NLS_PROFILE_SCOPE();

    EXPECT_EQ(destination.events.size(), 2u);
#else
    NLS_PROFILE_SCOPE();
    NLS_PROFILE_SCOPE();
    SUCCEED() << "Profiling macros compile to no-ops when NLS_ENABLE_PROFILING is disabled.";
#endif
}

TEST_F(ProfilerScopeTest, EmptyExplicitNameFallsBackToCallingFunctionName)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    {
        ProfilerScope scope("", __FUNCTION__);
    }

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_FALSE(destination.events[0].name.empty());
    EXPECT_EQ(destination.events[1].name, destination.events[0].name);
}

TEST_F(ProfilerScopeTest, NestedScopesTrackDepthPerThread)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    {
        ProfilerScope outer("Outer", __FUNCTION__);
        ProfilerScope inner("Inner", __FUNCTION__);
    }

    ASSERT_EQ(destination.events.size(), 4u);
    EXPECT_EQ(destination.events[0].phase, "begin");
    EXPECT_EQ(destination.events[0].name, "Outer");
    EXPECT_EQ(destination.events[0].depth, 0u);
    EXPECT_EQ(destination.events[1].phase, "begin");
    EXPECT_EQ(destination.events[1].name, "Inner");
    EXPECT_EQ(destination.events[1].depth, 1u);
    EXPECT_EQ(destination.events[2].phase, "end");
    EXPECT_EQ(destination.events[2].name, "Inner");
    EXPECT_EQ(destination.events[2].depth, 1u);
    EXPECT_EQ(destination.events[3].phase, "end");
    EXPECT_EQ(destination.events[3].name, "Outer");
    EXPECT_EQ(destination.events[3].depth, 0u);
}

TEST_F(ProfilerScopeTest, ScopesFromMultipleThreadsAreAccepted)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    auto emitScope = []
    {
        ProfilerScope scope("Worker Scope", __FUNCTION__);
    };

    std::thread first(emitScope);
    std::thread second(emitScope);
    first.join();
    second.join();

    size_t beginCount = 0u;
    for (const auto& event : destination.events)
    {
        if (event.phase == "begin")
            ++beginCount;
    }
    EXPECT_EQ(beginCount, 2u);
}

TEST_F(ProfilerScopeTest, GpuScopedObjectEndsScopeWhenLeavingBlock)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    {
        ProfilerGpuScope scope(nullptr, "GPU Block", __FUNCTION__);
        ASSERT_EQ(destination.events.size(), 1u);
        EXPECT_EQ(destination.events[0].phase, "gpu-begin");
        EXPECT_EQ(destination.events[0].name, "GPU Block");
    }

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_EQ(destination.events[1].phase, "gpu-end");
    EXPECT_EQ(destination.events[1].name, "GPU Block");
}

TEST_F(ProfilerScopeTest, DefaultGpuScopeMacroUsesCallingFunctionName)
{
    using namespace NLS::Base::Profiling;

#if defined(NLS_ENABLE_PROFILING)
    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(true);

    {
        NLS_GPU_PROFILE_SCOPE(nullptr);
    }

    ASSERT_EQ(destination.events.size(), 2u);
    EXPECT_FALSE(destination.events[0].name.empty());
    EXPECT_EQ(destination.events[1].name, destination.events[0].name);
#else
    NLS_GPU_PROFILE_SCOPE(nullptr);
    SUCCEED() << "Profiling macros compile to no-ops when NLS_ENABLE_PROFILING is disabled.";
#endif
}

TEST_F(ProfilerScopeTest, DisabledGpuScopeDoesNotCallDestination)
{
    using namespace NLS::Base::Profiling;

    RecordingProfilerDestination destination;
    Profiler::RegisterDestination(destination);
    Profiler::SetEnabled(false);

    {
        ProfilerGpuScope scope(nullptr, "Disabled GPU", __FUNCTION__);
    }

    EXPECT_TRUE(destination.events.empty());
    EXPECT_EQ(Profiler::GetSessionStats().acceptedEventCount, 0u);
    EXPECT_EQ(Profiler::GetSessionStats().droppedEventCount, 1u);
}
