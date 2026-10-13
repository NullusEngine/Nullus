#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::filesystem::path RepoPath(std::string_view relativePath)
    {
        return std::filesystem::path(NLS_ROOT_DIR) / std::filesystem::path(relativePath);
    }

    std::string ReadSourceText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input.is_open()) << "Failed to open source file: " << path.string();
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    size_t CountOccurrences(const std::string& source, const std::string_view needle)
    {
        size_t count = 0u;
        size_t cursor = 0u;
        while (true)
        {
            const auto found = source.find(needle, cursor);
            if (found == std::string::npos)
                return count;
            ++count;
            cursor = found + needle.size();
        }
    }

    /**
     * Removes submit lane calls so the remaining text can be
     * asserted against direct backend queue calls.
     */
    std::string StripSubmitLaneCallStatements(const std::string& source)
    {
        constexpr std::string_view kCalls[] = {"RunOnSubmitLane(", "RunBackendOperation("};
        std::string remaining;
        remaining.reserve(source.size());

        size_t cursor = 0u;
        while (true)
        {
            size_t call = std::string::npos;
            std::string_view matchedCall;
            for (const auto candidate : kCalls)
            {
                const auto found = source.find(candidate, cursor);
                if (found < call)
                {
                    call = found;
                    matchedCall = candidate;
                }
            }
            if (call == std::string::npos)
            {
                remaining.append(source, cursor, std::string::npos);
                return remaining;
            }

            remaining.append(source, cursor, call - cursor);

            // Skip the balanced argument list and the terminating semicolon so
            // lambdas and commas nested inside the call are handled correctly.
            size_t depth = 0u;
            size_t index = call + matchedCall.size() - 1u;
            for (; index < source.size(); ++index)
            {
                const char character = source[index];
                if (character == '(')
                {
                    ++depth;
                }
                else if (character == ')')
                {
                    --depth;
                    if (depth == 0u)
                        break;
                }
            }

            const auto terminator = source.find(';', index);
            if (terminator == std::string::npos)
            {
                remaining.append(source, call, std::string::npos);
                return remaining;
            }

            cursor = terminator + 1u;
        }
    }

    std::vector<std::filesystem::path> CollectRuntimeSources(const std::filesystem::path& root)
    {
        std::vector<std::filesystem::path> sources;
        std::error_code error;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error))
        {
            if (!entry.is_regular_file())
                continue;

            const auto extension = entry.path().extension().string();
            if (extension != ".cpp" && extension != ".h" && extension != ".hpp" &&
                extension != ".mm" && extension != ".inl")
            {
                continue;
            }

            sources.push_back(entry.path());
        }
        return sources;
    }

    bool OwnsQueueCallsByDesign(const std::filesystem::path& path)
    {
        const std::string text = path.generic_string();
        return text.find("/RHI/Backends/") != std::string::npos ||
            text.find("/RHI/Core/RHISwapchain.h") != std::string::npos ||
            text.find("/Context/RhiCommandListExecutor.cpp") != std::string::npos;
    }
}

TEST(RhiSubmitLaneSourceGuardTests, RhiCommandListExecutorFunnelsQueueOperationsThroughTheSubmitLane)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/RhiCommandListExecutorFrame.cpp"));
    ASSERT_FALSE(source.empty());

    const auto stripped = StripSubmitLaneCallStatements(source);
    EXPECT_EQ(stripped.find("SubmitChecked("), std::string::npos)
        << "Every queue submit must run inside RhiCommandListExecutor::RunOnSubmitLane.";
    EXPECT_EQ(stripped.find("PresentChecked("), std::string::npos)
        << "Every present must funnel through the submit lane.";
    EXPECT_EQ(stripped.find("->BeginReadPixels("), std::string::npos)
        << "Texture readback must be ordered with earlier queue submissions.";
    EXPECT_EQ(stripped.find("->BeginReadBuffer("), std::string::npos)
        << "Buffer readback must be ordered with earlier queue submissions.";

    EXPECT_GT(CountOccurrences(source, "RunOnSubmitLane("), 0u)
        << "RhiCommandListExecutor must route all of its queue operations through the submit lane.";
}

TEST(RhiSubmitLaneSourceGuardTests, NoOtherRuntimeSourceCallsBackendQueuesDirectly)
{
    const auto sources = CollectRuntimeSources(RepoPath("Runtime"));
    ASSERT_FALSE(sources.empty());

    std::vector<std::string> offenders;
    for (const auto& path : sources)
    {
        if (OwnsQueueCallsByDesign(path))
            continue;
        if (path.filename() == "RhiCommandListExecutorFrame.cpp")
            continue;

        const auto source = ReadSourceText(path);
        if (source.find("SubmitChecked(") != std::string::npos ||
            source.find("PresentChecked(") != std::string::npos)
        {
            offenders.push_back(path.generic_string());
        }
    }

    for (const auto& offender : offenders)
    {
        ADD_FAILURE() << offender
                      << " calls a backend queue directly; route it through "
                         "RhiCommandListExecutor::RunOnSubmitLane.";
    }
}

TEST(RhiSubmitLaneSourceGuardTests, DetectsDirectQueueCallsButAcceptsLaneRoutedCalls)
{
    // Negative control: the stripping helper must keep a bare queue call visible,
    // otherwise the guards above could pass while a bypass exists.
    const std::string bypassing =
        "void Frame(RHIQueue& queue)\n"
        "{\n"
        "    (void)queue.SubmitChecked(desc);\n"
        "}\n";
    EXPECT_NE(StripSubmitLaneCallStatements(bypassing).find("SubmitChecked("), std::string::npos);

    const std::string routed =
        "void Frame(RHIQueue& queue)\n"
        "{\n"
        "    (void)RhiCommandListExecutor::Get().RunOnSubmitLane(\n"
        "        \"Frame\", [&queue, &desc] { return queue.SubmitChecked(desc); });\n"
        "}\n";
    EXPECT_EQ(StripSubmitLaneCallStatements(routed).find("SubmitChecked("), std::string::npos);
    EXPECT_EQ(CountOccurrences(routed, "RunOnSubmitLane("), 1u);
}

TEST(RhiSubmitLaneSourceGuardTests, MetalPreviewAndUiBridgeCommitOnTheSubmitLane)
{
    for (const auto relativePath : {
        "Project/Editor/Rendering/MetalViewportRenderer.mm",
        "Runtime/Rendering/RHI/Backends/Metal/MetalUIBridge.mm",
        "Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"})
    {
        SCOPED_TRACE(relativePath);
        const auto source = ReadSourceText(RepoPath(relativePath));
        ASSERT_FALSE(source.empty());
        EXPECT_GT(CountOccurrences(source, "commit]"), 0u);
        EXPECT_EQ(StripSubmitLaneCallStatements(source).find("commit]"), std::string::npos)
            << "Native Metal commits must be serialized with other submits and presents.";
    }
    const std::string bypassing = "[commands commit];";
    EXPECT_NE(StripSubmitLaneCallStatements(bypassing).find("commit]"), std::string::npos);
    const std::string routed =
        "executor.RunOnSubmitLane(\"Metal\", [commands] { [commands commit]; return result; });";
    EXPECT_EQ(StripSubmitLaneCallStatements(routed).find("commit]"), std::string::npos);
}

TEST(RhiSubmitLaneSourceGuardTests, Dx12UploadsReadbacksAndProfilerDoNotBypassTheExecutor)
{
    for (const auto relativePath : {
        "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp",
        "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp",
        "Runtime/Rendering/RHI/Backends/DX12/DX12ReadbackUtils.cpp",
        "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp"})
    {
        SCOPED_TRACE(relativePath);
        const auto source = ReadSourceText(RepoPath(relativePath));
        ASSERT_FALSE(source.empty());
        EXPECT_GT(CountOccurrences(source, "SubmitDX12CommandLists("), 0u);
        const auto stripped = StripSubmitLaneCallStatements(source);
        EXPECT_EQ(stripped.find("->ExecuteCommandLists("), std::string::npos);
        EXPECT_EQ(stripped.find("->Signal("), std::string::npos);
        EXPECT_EQ(stripped.find("graphicsQueue->Wait("), std::string::npos);
    }

    const auto nativeSubmit = ReadSourceText(RepoPath(
        "Runtime/Rendering/RHI/Backends/DX12/DX12QueueSynchronization.cpp"));
    EXPECT_GT(CountOccurrences(nativeSubmit, "->ExecuteCommandLists("), 0u);
    EXPECT_EQ(StripSubmitLaneCallStatements(nativeSubmit).find("->ExecuteCommandLists("), std::string::npos);
    EXPECT_EQ(StripSubmitLaneCallStatements(nativeSubmit).find("->Signal("), std::string::npos);

    const std::string routed =
        "executor.RunBackendOperation(\"upload\", [&] { queue->ExecuteCommandLists(1, lists); return result; });"
        "queue->ExecuteCommandLists(1, bypass);";
    const auto stripped = StripSubmitLaneCallStatements(routed);
    EXPECT_EQ(CountOccurrences(stripped, "->ExecuteCommandLists("), 1u);
    EXPECT_NE(stripped.find("bypass"), std::string::npos);
}

TEST(RhiSubmitLaneSourceGuardTests, FrameProductionGateStaysScopedAndNarrow)
{
    // The RHI producer lane used to be a stored unique_lock so the owning thread
    // could release it on a later tick. That made the field shared mutable state: if
    // two threads installed a lock object into it concurrently, the assignment
    // destroyed the other thread's owning lock - silently unlocking a mutex that
    // thread still believed it held.
    //
    // That was not hypothetical: a game-thread drain running while the render thread
    // scheduled submissions wedged the pipeline at RenderReady permanently, with
    // every later drain burning its full watchdog. The lane is gone; production is
    // gated by a lock object local to one production step. These guards keep the
    // stored lane and the coarse lifetime from coming back.
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/Context/RhiCommandListExecutorFrame.cpp"));
    ASSERT_FALSE(source.empty());

    const auto extractBody = [&source](const std::string_view signature)
    {
        const auto begin = source.find(signature);
        EXPECT_NE(begin, std::string::npos) << "Missing function: " << signature;
        if (begin == std::string::npos)
            return std::string {};

        const auto bodyBegin = source.find('{', begin);
        EXPECT_NE(bodyBegin, std::string::npos);
        if (bodyBegin == std::string::npos)
            return std::string {};

        size_t depth = 0u;
        for (size_t offset = bodyBegin; offset < source.size(); ++offset)
        {
            if (source[offset] == '{')
            {
                ++depth;
            }
            else if (source[offset] == '}')
            {
                --depth;
                if (depth == 0u)
                    return source.substr(bodyBegin, offset - bodyBegin + 1u);
            }
        }
        return std::string {};
    };

    const std::string gateBody = extractBody(
        "ScopedThreadedProductionGate(DriverImpl& impl");
    ASSERT_FALSE(gateBody.empty());

    // The gate must be a local object that the calling thread owns for one
    // production step. Storing the lock in the driver is what made the previous
    // producer lane a shared mutable field: two threads could install a lock object
    // into it concurrently, and the assignment destroyed the other thread's owning
    // lock, silently unlocking a mutex that thread still believed it held. That
    // wedged the lane at RenderReady permanently.
    EXPECT_NE(
        source.find("std::unique_lock<std::timed_mutex> m_lock"),
        std::string::npos)
        << "The gate must own its lock as a member of a local scoped object.";
    EXPECT_EQ(
        gateBody.find("threadedRhiSubmissionMutex ="),
        std::string::npos)
        << "The gate must lock the driver mutex, never reassign it.";
    EXPECT_EQ(
        source.find("threadedRhiProducerLane"),
        std::string::npos)
        << "The stored producer lane must not come back; it silently unlocked a "
           "mutex another thread still owned.";
    EXPECT_EQ(
        source.find("ProducerLaneOwnedByCurrentThread"),
        std::string::npos)
        << "Thread-id lane ownership tracking is unnecessary once the lock is local.";

    // Frame overlap depends on the gate covering only claim/prepare/hand-off.
    for (const auto* signature : {
             "bool RhiCommandListExecutor::DispatchRenderFrame(" })
    {
        const std::string body = extractBody(signature);
        ASSERT_FALSE(body.empty()) << "Missing function: " << signature;
        EXPECT_NE(body.find("ScopedThreadedProductionGate"), std::string::npos)
            << "Frame production must still serialize its hand-off window: " << signature;
        EXPECT_EQ(body.find("DrainProducerOwnedThreadedSubmission"), std::string::npos)
            << "A producer must never wait for earlier frames: " << signature;
    }

    // The idle mesh upload only moves parsed CPU payloads; gating it would put
    // frame preparation behind upload bookkeeping for no safety benefit.
    const std::string idleUploadBody = extractBody(
        "bool RhiCommandListExecutor::TryScheduleIdleMeshRuntimeUploads(");
    ASSERT_FALSE(idleUploadBody.empty());
    EXPECT_EQ(idleUploadBody.find("ScopedThreadedProductionGate"), std::string::npos)
        << "The CPU-only idle upload must not take the frame production gate.";
}
