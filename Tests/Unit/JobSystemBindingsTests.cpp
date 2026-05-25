#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iterator>
#include <type_traits>
#include <vector>

#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobBindings.h"
#include "Jobs/JobSystem.h"

namespace
{
    constexpr uint32_t kBindingVersion = NLS_JOB_BINDING_VERSION;

    class JobSystemBindingsTests : public ::testing::Test
    {
    protected:
        void TearDown() override
        {
            NLS::Base::Jobs::ShutdownJobSystem();
#if defined(NLS_ENABLE_TEST_HOOKS)
            NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
        }
    };

    struct CounterData
    {
        std::atomic<int>* counter = nullptr;
    };

    void IncrementCounter(void* userData)
    {
        auto* data = static_cast<CounterData*>(userData);
        data->counter->fetch_add(1, std::memory_order_acq_rel);
    }

    void ThrowingBindingJob(void*)
    {
        throw std::runtime_error("expected binding job failure");
    }

    struct ForEachData
    {
        std::vector<int>* visits = nullptr;
        std::atomic<int>* combineCount = nullptr;
    };

    void VisitIndex(void* userData, const uint32_t index)
    {
        auto* data = static_cast<ForEachData*>(userData);
        (*data->visits)[index] += 1;
    }

    void Combine(void* userData)
    {
        auto* data = static_cast<ForEachData*>(userData);
        data->combineCount->fetch_add(1, std::memory_order_acq_rel);
    }

    NLS_BindingJobHandle DefaultBindingHandle()
    {
        NLS_BindingJobHandle handle{};
        handle.structSize = sizeof(NLS_BindingJobHandle);
        handle.version = kBindingVersion;
        return handle;
    }

    NLS_BindingJobHandle BindingHandleFromNative(const NLS::Base::Jobs::JobHandle nativeHandle)
    {
        auto handle = DefaultBindingHandle();
        handle.id = nativeHandle.id;
        handle.generation = nativeHandle.generation;
        return handle;
    }

    struct CompleteOwnHandleData
    {
        NLS_BindingJobHandle* handle = nullptr;
        NLS_JobStatusCode status = NLS_JOB_STATUS_OK;
        bool handleRemainedSet = false;
    };

    void CompleteOwnBindingHandle(void* userData)
    {
        auto* data = static_cast<CompleteOwnHandleData*>(userData);
        data->status = NLS_Jobs_Complete(data->handle);
        data->handleRemainedSet = data->handle->id != 0u;
    }
}

TEST_F(JobSystemBindingsTests, BindingSchedulesAndCompletesNativeJob)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    uint32_t workerCount = 99u;
    EXPECT_EQ(NLS_Jobs_GetWorkerCount(&workerCount), NLS_JOB_STATUS_OK);
    EXPECT_EQ(workerCount, 0u);

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = IncrementCounter;
    desc.userData = &data;
    desc.dependency = DefaultBindingHandle();

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_OK);
    EXPECT_NE(handle.id, 0u);

    uint32_t completed = 1u;
    EXPECT_EQ(NLS_Jobs_IsCompleted(&handle, &completed), NLS_JOB_STATUS_OK);
    EXPECT_EQ(completed, 0u);

    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(handle.id, 0u);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBindingsTests, BindingForEachVisitsEveryIndexAndCombines)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits(11u, 0);
    std::atomic<int> combineCount = 0;
    ForEachData data{&visits, &combineCount};

    NLS_BindingForEachScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingForEachScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = VisitIndex;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(visits.size());
    desc.combineCallback = Combine;
    desc.dependency = DefaultBindingHandle();

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&desc, &handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_OK);

    for (const int visit : visits)
        EXPECT_EQ(visit, 1);
    EXPECT_EQ(combineCount.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBindingsTests, BindingForEachZeroIterationsReturnsCompletedHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits;
    std::atomic<int> combineCount = 0;
    ForEachData data{&visits, &combineCount};

    NLS_BindingForEachScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingForEachScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = VisitIndex;
    desc.userData = &data;
    desc.iterationCount = 0u;
    desc.combineCallback = Combine;
    desc.dependency = DefaultBindingHandle();

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&desc, &handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(handle.id, 0u);
    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(combineCount.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemBindingsTests, BindingForEachReportsRejectedNativeSchedule)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::vector<int> visits;
    std::atomic<int> combineCount = 0;
    ForEachData data{&visits, &combineCount};

    NLS_BindingForEachScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingForEachScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = VisitIndex;
    desc.userData = &data;
    desc.iterationCount = static_cast<uint32_t>(std::numeric_limits<int>::max()) + 1u;
    desc.dependency = DefaultBindingHandle();

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&desc, &handle), NLS_JOB_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(handle.id, 0u);
    EXPECT_EQ(combineCount.load(std::memory_order_acquire), 0);
}

TEST_F(JobSystemBindingsTests, BindingRejectsInvalidAndDefaultHandlesDeterministically)
{
    EXPECT_EQ(NLS_Jobs_GetWorkerCount(nullptr), NLS_JOB_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(NLS_Jobs_Schedule(nullptr, nullptr), NLS_JOB_STATUS_INVALID_ARGUMENT);

    auto defaultHandle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_Complete(&defaultHandle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(defaultHandle.id, 0u);

    NLS::Base::Jobs::JobSystemConfig config;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    auto stale = DefaultBindingHandle();
    stale.id = 0x1234u;
    stale.generation = 77u;
    EXPECT_EQ(NLS_Jobs_Complete(&stale), NLS_JOB_STATUS_INVALID_HANDLE);
}

TEST_F(JobSystemBindingsTests, BindingHandleApisReturnNotInitializedForNonDefaultHandlesWhenRuntimeStopped)
{
    auto handle = DefaultBindingHandle();
    handle.id = 0x1234u;
    handle.generation = 77u;

    uint32_t completed = 99u;
    EXPECT_EQ(NLS_Jobs_IsCompleted(&handle, &completed), NLS_JOB_STATUS_NOT_INITIALIZED);
    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_NOT_INITIALIZED);
    EXPECT_EQ(NLS_Jobs_ClearWithoutSync(&handle), NLS_JOB_STATUS_NOT_INITIALIZED);

    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = IncrementCounter;
    desc.userData = &data;
    desc.dependency = DefaultBindingHandle();

    auto scheduled = DefaultBindingHandle();
    ASSERT_EQ(NLS_Jobs_Schedule(&desc, &scheduled), NLS_JOB_STATUS_OK);
    ASSERT_NE(scheduled.id, 0u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    completed = 99u;
    EXPECT_EQ(NLS_Jobs_IsCompleted(&scheduled, &completed), NLS_JOB_STATUS_NOT_INITIALIZED);
    EXPECT_EQ(NLS_Jobs_Complete(&scheduled), NLS_JOB_STATUS_NOT_INITIALIZED);
    EXPECT_EQ(NLS_Jobs_ClearWithoutSync(&scheduled), NLS_JOB_STATUS_NOT_INITIALIZED);
}

TEST_F(JobSystemBindingsTests, BindingIsCompletedReturnsNotInitializedForRetiredBackgroundHandleAfterShutdown)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = IncrementCounter;
    desc.userData = &data;
    auto nativeHandle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    ASSERT_NE(nativeHandle.id, 0u);
    NLS::Base::Jobs::CompleteNoClear(nativeHandle);
    ASSERT_EQ(counter.load(std::memory_order_acquire), 1);

    auto bindingHandle = BindingHandleFromNative(nativeHandle);
    uint32_t completed = 0u;
    ASSERT_EQ(NLS_Jobs_IsCompleted(&bindingHandle, &completed), NLS_JOB_STATUS_OK);
    ASSERT_EQ(completed, 1u);

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);

    completed = 99u;
    EXPECT_EQ(NLS_Jobs_IsCompleted(&bindingHandle, &completed), NLS_JOB_STATUS_NOT_INITIALIZED);
}

TEST_F(JobSystemBindingsTests, AllZeroBindingHandleBehavesLikeDefaultCompleteHandle)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS_BindingJobHandle zeroHandle{};
    uint32_t completed = 0u;
    EXPECT_EQ(NLS_Jobs_IsCompleted(&zeroHandle, &completed), NLS_JOB_STATUS_OK);
    EXPECT_EQ(completed, 1u);
    EXPECT_EQ(NLS_Jobs_Complete(&zeroHandle), NLS_JOB_STATUS_OK);

    std::atomic<int> counter = 0;
    CounterData data{&counter};
    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = IncrementCounter;
    desc.userData = &data;
    desc.dependency = zeroHandle;

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBindingsTests, BindingRejectsInvalidPriorityAndSafetyPolicy)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = IncrementCounter;
    desc.userData = &data;
    desc.dependency = DefaultBindingHandle();

    auto handle = DefaultBindingHandle();
    desc.priority = 99u;
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_INVALID_ARGUMENT);

    desc.priority = 0u;
    desc.safetyPolicy = 99u;
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_INVALID_ARGUMENT);

    NLS_BindingForEachScheduleDesc forEachDesc{};
    forEachDesc.structSize = sizeof(NLS_BindingForEachScheduleDesc);
    forEachDesc.version = kBindingVersion;
    forEachDesc.callback = VisitIndex;
    forEachDesc.userData = nullptr;
    forEachDesc.iterationCount = 1u;
    forEachDesc.dependency = DefaultBindingHandle();

    forEachDesc.priority = 99u;
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&forEachDesc, &handle), NLS_JOB_STATUS_INVALID_ARGUMENT);

    forEachDesc.priority = 0u;
    forEachDesc.safetyPolicy = 99u;
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&forEachDesc, &handle), NLS_JOB_STATUS_INVALID_ARGUMENT);
}

TEST_F(JobSystemBindingsTests, BindingAcceptsNamedPriorityAndSafetyConstants)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = IncrementCounter;
    desc.userData = &data;
    desc.dependency = DefaultBindingHandle();
    desc.priority = NLS_JOB_PRIORITY_HIGH;
    desc.safetyPolicy = NLS_JOB_SAFETY_GUARANTEED_NO_SYNC_WAIT;

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemBindingsTests, BindingCompleteReportsFailedJobs)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = ThrowingBindingJob;
    desc.dependency = DefaultBindingHandle();

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_FAILED);
    EXPECT_EQ(handle.id, 0u);
}

TEST_F(JobSystemBindingsTests, BindingCompleteReportsCancelledJobs)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS::Base::Jobs::JobScheduleDesc failedDesc;
    failedDesc.function = ThrowingBindingJob;
    auto failedDependency = NLS::Base::Jobs::ScheduleJob(failedDesc);
    ASSERT_NE(failedDependency.id, 0u);

    NLS::Base::Jobs::CompleteNoClear(failedDependency);
    ASSERT_EQ(
        NLS::Base::Jobs::Internal::GetJobCompletionStatus(failedDependency),
        NLS::Base::Jobs::JobCompletionStatus::Failed);

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = IncrementCounter;
    desc.userData = &data;
    desc.dependency = BindingHandleFromNative(failedDependency);

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_CANCELLED);
    EXPECT_EQ(handle.id, 0u);
    EXPECT_EQ(counter.load(std::memory_order_acquire), 0);

    NLS::Base::Jobs::ClearWithoutSync(failedDependency);
}

TEST_F(JobSystemBindingsTests, BindingCompleteDoesNotClearPendingSelfWait)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    auto handle = DefaultBindingHandle();
    CompleteOwnHandleData data{&handle};
    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion;
    desc.callback = CompleteOwnBindingHandle;
    desc.userData = &data;

    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_OK);
    EXPECT_NE(handle.id, 0u);

    EXPECT_EQ(NLS_Jobs_Complete(&handle), NLS_JOB_STATUS_OK);
    EXPECT_EQ(data.status, NLS_JOB_STATUS_FAILED);
    EXPECT_TRUE(data.handleRemainedSet);
    EXPECT_EQ(handle.id, 0u);
}

TEST_F(JobSystemBindingsTests, BindingValidatesStructSizeAndVersion)
{
    NLS::Base::Jobs::JobSystemConfig config;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    CounterData data{&counter};

    NLS_BindingJobScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingJobScheduleDesc) - 1u;
    desc.version = kBindingVersion;
    desc.callback = IncrementCounter;
    desc.userData = &data;
    desc.dependency = DefaultBindingHandle();

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_VERSION_MISMATCH);

    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = kBindingVersion + 1u;
    EXPECT_EQ(NLS_Jobs_Schedule(&desc, &handle), NLS_JOB_STATUS_VERSION_MISMATCH);
}

TEST_F(JobSystemBindingsTests, BindingHandleApisValidateStructSizeAndVersion)
{
    auto badSize = DefaultBindingHandle();
    badSize.structSize = sizeof(NLS_BindingJobHandle) - 1u;

    uint32_t completed = 0u;
    EXPECT_EQ(NLS_Jobs_IsCompleted(&badSize, &completed), NLS_JOB_STATUS_VERSION_MISMATCH);
    EXPECT_EQ(NLS_Jobs_Complete(&badSize), NLS_JOB_STATUS_VERSION_MISMATCH);
    EXPECT_EQ(NLS_Jobs_ClearWithoutSync(&badSize), NLS_JOB_STATUS_VERSION_MISMATCH);

    auto badVersion = DefaultBindingHandle();
    badVersion.version = kBindingVersion + 1u;
    EXPECT_EQ(NLS_Jobs_IsCompleted(&badVersion, &completed), NLS_JOB_STATUS_VERSION_MISMATCH);
    EXPECT_EQ(NLS_Jobs_Complete(&badVersion), NLS_JOB_STATUS_VERSION_MISMATCH);
    EXPECT_EQ(NLS_Jobs_ClearWithoutSync(&badVersion), NLS_JOB_STATUS_VERSION_MISMATCH);
}

TEST_F(JobSystemBindingsTests, BindingStructsArePlainAbiAndDoNotOwnStlObjects)
{
    EXPECT_EQ(NLS_JOB_BINDING_VERSION, 1u);
    EXPECT_EQ(NLS_JOB_STATUS_OK, 0);
    EXPECT_EQ(NLS_JOB_STATUS_NOT_INITIALIZED, 1);
    EXPECT_EQ(NLS_JOB_STATUS_INVALID_ARGUMENT, 2);
    EXPECT_EQ(NLS_JOB_STATUS_INVALID_HANDLE, 3);
    EXPECT_EQ(NLS_JOB_STATUS_VERSION_MISMATCH, 4);
    EXPECT_EQ(NLS_JOB_STATUS_CANCELLED, 5);
    EXPECT_EQ(NLS_JOB_STATUS_FAILED, 6);

    EXPECT_TRUE(std::is_standard_layout_v<NLS_BindingJobHandle>);
    EXPECT_TRUE(std::is_trivially_copyable_v<NLS_BindingJobHandle>);
    EXPECT_TRUE(std::is_standard_layout_v<NLS_BindingJobScheduleDesc>);
    EXPECT_TRUE(std::is_trivially_copyable_v<NLS_BindingJobScheduleDesc>);
    EXPECT_TRUE(std::is_standard_layout_v<NLS_BindingForEachScheduleDesc>);
    EXPECT_TRUE(std::is_trivially_copyable_v<NLS_BindingForEachScheduleDesc>);

    EXPECT_EQ(offsetof(NLS_BindingJobHandle, structSize), 0u);
    EXPECT_EQ(offsetof(NLS_BindingJobHandle, id), 8u);
    EXPECT_EQ(offsetof(NLS_BindingJobScheduleDesc, callback), 8u);
    EXPECT_EQ(offsetof(NLS_BindingForEachScheduleDesc, callback), 8u);
    if constexpr (sizeof(void*) == 8u)
    {
        EXPECT_EQ(sizeof(NLS_BindingJobHandle), 24u);
        EXPECT_EQ(sizeof(NLS_BindingJobScheduleDesc), 64u);
        EXPECT_EQ(sizeof(NLS_BindingForEachScheduleDesc), 80u);
        EXPECT_EQ(offsetof(NLS_BindingJobScheduleDesc, dependency), 24u);
        EXPECT_EQ(offsetof(NLS_BindingForEachScheduleDesc, dependency), 40u);
    }
    else
    {
        EXPECT_EQ(sizeof(void*), 4u);
        EXPECT_EQ(sizeof(NLS_BindingJobHandle), 24u);
        EXPECT_EQ(sizeof(NLS_BindingJobScheduleDesc), 48u);
        EXPECT_EQ(sizeof(NLS_BindingForEachScheduleDesc), 64u);
        EXPECT_EQ(offsetof(NLS_BindingJobScheduleDesc, dependency), 16u);
        EXPECT_EQ(offsetof(NLS_BindingForEachScheduleDesc, dependency), 32u);
    }
}

TEST_F(JobSystemBindingsTests, BindingForEachValidatesStructSizeVersionAndArguments)
{
    NLS::Base::Jobs::JobSystemConfig config;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    NLS_BindingForEachScheduleDesc desc{};
    desc.structSize = sizeof(NLS_BindingForEachScheduleDesc) - 1u;
    desc.version = kBindingVersion;
    desc.callback = VisitIndex;
    desc.iterationCount = 1u;
    desc.batchSize = 1u;
    desc.dependency = DefaultBindingHandle();

    auto handle = DefaultBindingHandle();
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&desc, &handle), NLS_JOB_STATUS_VERSION_MISMATCH);

    desc.structSize = sizeof(NLS_BindingForEachScheduleDesc);
    desc.version = kBindingVersion + 1u;
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&desc, &handle), NLS_JOB_STATUS_VERSION_MISMATCH);

    desc.version = kBindingVersion;
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&desc, nullptr), NLS_JOB_STATUS_INVALID_ARGUMENT);

    desc.callback = nullptr;
    EXPECT_EQ(NLS_Jobs_ScheduleForEach(&desc, &handle), NLS_JOB_STATUS_INVALID_ARGUMENT);
}

TEST(JobSystemBindingSourceContractTests, BindingHeaderUsesCCompatibleDeclarations)
{
    const auto sourcePath = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Jobs/JobBindings.h";
    std::ifstream input(sourcePath, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    EXPECT_EQ(source.find("using NLS_BindingJobCallback"), std::string::npos);
    EXPECT_EQ(source.find("enum NLS_JobStatusCode :"), std::string::npos);
    EXPECT_EQ(source.find("#include \"BaseDef.h\""), std::string::npos);
    EXPECT_NE(source.find("#ifndef NLS_BASE_API"), std::string::npos);
    EXPECT_NE(source.find("typedef enum NLS_JobStatusCode"), std::string::npos);
    EXPECT_NE(source.find("typedef enum NLS_JobBindingPriority"), std::string::npos);
    EXPECT_NE(source.find("typedef enum NLS_JobBindingSafetyPolicy"), std::string::npos);
    EXPECT_NE(source.find("NLS_JOB_BINDING_VERSION"), std::string::npos);
    EXPECT_NE(source.find("typedef struct NLS_BindingJobHandle"), std::string::npos);
    EXPECT_NE(source.find("typedef void (*NLS_BindingJobCallback)"), std::string::npos);
}
