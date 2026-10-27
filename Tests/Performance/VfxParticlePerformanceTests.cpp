#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <Guid.h>
#include <VFX/CompiledSystem.h>
#include <VFX/CpuEmitter.h>
#include <VFX/GpuEmitter.h>
#include <VFX/ParticleBounds.h>
#include <VFX/VfxGraph.h>
#include <VFXCompiler/VfxCompiler.h>
#include <VFXCompiler/VfxShaderCompiler.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Jobs/JobSystem.h>

namespace
{
using namespace NLS::VFX;
namespace context = NLS::Render::Context;
namespace rhi = NLS::Render::RHI;
namespace settings = NLS::Render::Settings;

constexpr double kBenchmarkDelta = 1.0 / 60.0;

/**
 * Owns a Jobs worker pool for the CPU sweeps.
 *
 * CpuEmitter fans out across either a Jobs pool or a running global TaskGraph
 * and falls back to serial otherwise, so a benchmark without one silently
 * measures the serial path. The pool is started before the samples and torn
 * down after them so its startup cost is not part of any sample.
 */
class ScopedBenchmarkJobSystem final
{
public:
    ScopedBenchmarkJobSystem()
    {
        NLS::Base::Jobs::JobSystemConfig config;
        config.enableDiagnostics = false;
        // NLS_VFX_PERF_WORKERS lets the sweep show where fan-out stops paying
        // off instead of only measuring the auto-sized pool.
        if (const char* const configured = std::getenv("NLS_VFX_PERF_WORKERS");
            configured != nullptr && *configured != '\0')
        {
            const long long parsed = std::strtoll(configured,nullptr,10);
            if (parsed >= 0 && parsed <= 256)
                config.workerCount = static_cast<uint32_t>(parsed);
        }
        m_initialized = NLS::Base::Jobs::TryInitializeJobSystem(config);
    }

    ~ScopedBenchmarkJobSystem()
    {
        if (!m_initialized) return;
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
    }

    [[nodiscard]] bool Initialized() const { return m_initialized; }

private:
    bool m_initialized = false;
};

/**
 * Nearest-rank percentile over an already sorted sample vector.
 */
double Percentile(const std::vector<double>& sorted, const double fraction)
{
    if (sorted.empty())
        return 0.0;
    const auto rank = static_cast<size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[(std::min)(rank == 0 ? 0u : rank - 1u, sorted.size() - 1u)];
}

struct TimingSummary
{
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double maxMs = 0.0;
};

TimingSummary Summarize(std::vector<double> samples)
{
    TimingSummary summary;
    if (samples.empty())
        return summary;
    double total = 0.0;
    for (const auto sample : samples)
        total += sample;
    summary.meanMs = total / static_cast<double>(samples.size());
    summary.maxMs = *std::max_element(samples.begin(), samples.end());
    std::sort(samples.begin(), samples.end());
    summary.p50Ms = Percentile(samples, 0.50);
    summary.p95Ms = Percentile(samples, 0.95);
    return summary;
}

uint32_t ResolveCount(const char* name, const uint32_t fallback, const uint32_t maximum)
{
    const char* const configured = std::getenv(name);
    if (configured == nullptr || *configured == '\0')
        return fallback;
    const long long parsed = std::strtoll(configured, nullptr, 10);
    if (parsed <= 0)
        return fallback;
    return static_cast<uint32_t>((std::min)(static_cast<long long>(maximum), parsed));
}

bool OrderingReadbackEnabled()
{
    const char* const configured = std::getenv("NLS_VFX_PERF_ORDERING_READBACK");
    if (configured == nullptr || *configured == '\0')
        return true;
    const std::string text { configured };
    return !(text == "0" || text == "false" || text == "FALSE" || text == "off" || text == "OFF");
}

/**
 * Capacity list for one scaling sweep.
 *
 * NLS_VFX_PERF_CPU_CAPACITIES / NLS_VFX_PERF_GPU_CAPACITIES accept a comma
 * separated list so the acceptance matrix can request 1M GPU particles without
 * paying that cost on every routine run.
 */
std::vector<uint32_t> ResolveCapacityList(const char* name, const std::vector<uint32_t>& fallback)
{
    const char* const configured = std::getenv(name);
    if (configured == nullptr || *configured == '\0')
        return fallback;

    std::vector<uint32_t> capacities;
    const std::string text { configured };
    size_t begin = 0;
    while (begin <= text.size())
    {
        const auto separator = text.find(',', begin);
        const auto token = text.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin);
        if (!token.empty())
        {
            const long long parsed = std::strtoll(token.c_str(), nullptr, 10);
            if (parsed > 0 && parsed <= 10000000ll)
                capacities.push_back(static_cast<uint32_t>(parsed));
        }
        if (separator == std::string::npos)
            break;
        begin = separator + 1u;
    }
    return capacities.empty() ? fallback : capacities;
}

/**
 * Emitter template for the scaling sweeps.
 *
 * The spawn rate fills the population on the first simulated frame and the
 * lifetime keeps particles alive for the whole measurement, so the sampled
 * ticks are the steady-state spawn/update/compaction cost at full occupancy
 * instead of a growth ramp.
 */
SystemAsset MakeBenchmarkSystem(const uint32_t capacity, const SimulationTarget target)
{
    auto system = CreateDefaultSystem("Particle benchmark");
    auto& emitter = system.emitters.front();
    emitter.target = target;
    emitter.capacity = capacity;
    emitter.initialCapacity = 0;
    // The scheduler rejects rates above 10M/s, which bounds the fill time for
    // the 1M particle case at a small number of frames.
    emitter.spawnRate = (std::min)(static_cast<double>(capacity) * 60.0, 10000000.0);
    emitter.duration = 1.0e6;
    emitter.looping = true;
    emitter.interpolatedSpawning = false;
    emitter.bursts.clear();
    emitter.collisionEnabled = false;
    emitter.sortByViewDepth = false;
    for (auto& attribute : emitter.attributes)
        if (attribute.name == "Particles.Lifetime")
            attribute.defaultValue = {1.0e6f, 0.0f, 0.0f, 0.0f};
    return system;
}

/**
 * Simulation buffer footprint of the SoA storage at full occupancy.
 *
 * Attribute columns plus the 64-bit particle ids; this is the deterministic
 * part of the emitter memory cost, independent of allocator overhead.
 */
uint64_t SimulationBytes(const std::vector<Attribute>& attributes, const uint32_t capacity)
{
    uint64_t scalars = 0;
    for (const auto& attribute : attributes)
        scalars += ComponentCount(attribute.type);
    return scalars * capacity * sizeof(float) + uint64_t { capacity } * sizeof(uint64_t);
}

struct ScalingReport
{
    uint32_t capacity = 0;
    uint32_t particles = 0;
    uint64_t simulationBytes = 0;
    TimingSummary timing;
};

void ReportScaling(ScalingReport& report, const char* const backend)
{
    const auto& timing = report.timing;
    std::cout << "[VFX perf] backend=" << backend
              << " capacity=" << report.capacity
              << " particles=" << report.particles
              << " simulationMiB=" << (static_cast<double>(report.simulationBytes) / (1024.0 * 1024.0))
              << " meanMs=" << timing.meanMs
              << " p50Ms=" << timing.p50Ms
              << " p95Ms=" << timing.p95Ms
              << " maxMs=" << timing.maxMs
              << std::endl;

    const auto prefix = std::string("vfx_") + backend + "_" + std::to_string(report.capacity);
    ::testing::Test::RecordProperty(prefix + "_particles", std::to_string(report.particles));
    ::testing::Test::RecordProperty(prefix + "_simulation_bytes", std::to_string(report.simulationBytes));
    ::testing::Test::RecordProperty(prefix + "_mean_ms", std::to_string(timing.meanMs));
    ::testing::Test::RecordProperty(prefix + "_p95_ms", std::to_string(timing.p95Ms));
}

/**
 * Reports the sweep through the shared performance report channel when the
 * caller asked for it, so XML/console capture is not the only durable record.
 */
void WriteReportIfRequested(const std::string& scenarioName, const std::vector<ScalingReport>& reports)
{
    const char* const reportDirectory = std::getenv("NLS_PERFORMANCE_REPORT_DIR");
    if (reportDirectory == nullptr || std::string(reportDirectory).empty())
        return;

    std::error_code error;
    std::filesystem::create_directories(reportDirectory, error);
    if (error)
        return;

    std::ofstream output(std::filesystem::path(reportDirectory) / (scenarioName + ".txt"),
        std::ios::binary | std::ios::trunc);
    if (!output)
        return;

    output << "scenario: " << scenarioName << "\n";
    for (const auto& report : reports)
    {
        output << "capacity=" << report.capacity
               << " particles=" << report.particles
               << " simulation_bytes=" << report.simulationBytes
               << " mean_ms=" << report.timing.meanMs
               << " p50_ms=" << report.timing.p50Ms
               << " p95_ms=" << report.timing.p95Ms
               << " max_ms=" << report.timing.maxMs << "\n";
    }
}

/**
 * Hidden NoAPI surface for backends that refuse a device without a window.
 */
struct BenchmarkPlatformWindow
{
    GLFWwindow* handle = nullptr;

    ~BenchmarkPlatformWindow()
    {
        if (handle != nullptr)
            glfwDestroyWindow(handle);
        if (glfwInitialized)
            glfwTerminate();
    }

    bool glfwInitialized = false;

    void Ensure(const settings::EGraphicsBackend backend)
    {
        if (backend != settings::EGraphicsBackend::VULKAN)
            return;
        if (!glfwInit())
            return;
        glfwInitialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        handle = glfwCreateWindow(64, 64, "VFX performance", nullptr, nullptr);
    }
};

settings::EGraphicsBackend ResolveBackend()
{
    using settings::EGraphicsBackend;
    const char* const configured = std::getenv("NLS_VFX_PERF_BACKEND");
    if (configured == nullptr || *configured == '\0')
        return settings::DriverSettings {}.graphicsBackend;
    const std::string requested { configured };
    if (requested == "dx12")
        return EGraphicsBackend::DX12;
    if (requested == "vulkan")
        return EGraphicsBackend::VULKAN;
    if (requested == "metal")
        return EGraphicsBackend::METAL;
    return settings::DriverSettings {}.graphicsBackend;
}
}

/**
 * CPU scaling baseline: 1k / 10k / 100k full-occupancy update cost.
 *
 * Every sampled tick includes spawn scheduling, the update program over the
 * whole population, and lifetime compaction, which is the recurring per-frame
 * work a game pays for a live emitter.
 */
TEST(VfxParticlePerformanceTests, CpuEmitterScalingBaseline)
{
    const auto capacities = ResolveCapacityList("NLS_VFX_PERF_CPU_CAPACITIES", {1000u, 10000u, 100000u});
    const auto frames = ResolveCount("NLS_VFX_PERF_CPU_FRAMES", 120u, 200000u);
    std::vector<ScalingReport> reports;
    const ScopedBenchmarkJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.Initialized()) << "CPU scaling needs a live Jobs worker pool";
    std::cout << "[VFX perf] backend=cpu workers=" << NLS::Base::Jobs::GetJobWorkerCount() << std::endl;

    for (const auto capacity : capacities)
    {
        const auto system = MakeBenchmarkSystem(capacity, SimulationTarget::CPU);
        const auto& emitterAsset = system.emitters.front();
        CompiledSystem compiled;
        std::vector<Diagnostic> diagnostics;
        ASSERT_TRUE(CompileSystem(system, compiled, diagnostics))
            << (diagnostics.empty() ? std::string {} : diagnostics.front().message);

        CpuEmitter emitter;
        std::string error;
        ASSERT_TRUE(emitter.Initialize(emitterAsset, compiled.emitters[0].spawn, compiled.emitters[0].update, error)) << error;

        // Warm up until the population is full so storage growth and the initial
        // allocation are excluded from the sampled window.
        for (uint32_t frame = 0; frame < 256u && emitter.Data().Count() < capacity; ++frame)
            ASSERT_TRUE(emitter.Tick(kBenchmarkDelta)) << error;
        ASSERT_EQ(emitter.Data().Count(), capacity) << "Benchmark emitter never reached full occupancy";

        std::vector<double> samples;
        samples.reserve(frames);
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            const auto begin = std::chrono::steady_clock::now();
            const bool ticked = emitter.Tick(kBenchmarkDelta);
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            ASSERT_TRUE(ticked) << error;
            samples.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
        }

        ScalingReport report;
        report.capacity = capacity;
        report.particles = emitter.Data().Count();
        report.simulationBytes = SimulationBytes(emitterAsset.attributes, emitter.Data().Capacity());
        report.timing = Summarize(std::move(samples));
        ReportScaling(report, "cpu");
        reports.push_back(report);
    }

    WriteReportIfRequested("vfx_particle_cpu_scaling", reports);
}

/**
 * GPU scaling baseline: 10k / 100k particles on the active backend.
 *
 * The backend exposes no timestamp query yet, so each sample is one fully
 * synchronised frame: record, submit, then wait for the frame fence. The
 * queue is drained inside the sample, which makes the number an end-to-end
 * simulation frame cost rather than isolated GPU execution time.
 *
 * The acceptance matrix requests 1M particles explicitly:
 *
 *   $env:NLS_VFX_PERF_GPU_CAPACITIES = '1000000'
 */
TEST(VfxParticlePerformanceTests, GpuEmitterScalingBaseline)
{
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized())
        GTEST_SKIP() << "Another fixture owns the global RHI executor";
    executor.Initialize(context::RhiThreadMode::None, {});
    struct Shutdown
    {
        context::RhiCommandListExecutor& executor;
        ~Shutdown() { executor.Shutdown(); }
    } shutdown { executor };

    const auto backend = ResolveBackend();
    BenchmarkPlatformWindow window;
    window.Ensure(backend);
    settings::DriverSettings deviceSettings;
    deviceSettings.debugMode = false;
    deviceSettings.graphicsBackend = backend;
    deviceSettings.platformWindow = window.handle;
    auto device = NLS::Render::Backend::CreateRhiDevice(deviceSettings);
    if (!device || !device->IsBackendReady())
        GTEST_SKIP() << "Requested RHI device is unavailable for the VFX performance sweep";
    if (!device->GetCapabilities().supportsCompute)
        GTEST_SKIP() << "VFX simulation needs a compute capable device";
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));

    const auto capacities = ResolveCapacityList("NLS_VFX_PERF_GPU_CAPACITIES", {10000u, 100000u});
    const auto frames = ResolveCount("NLS_VFX_PERF_GPU_FRAMES", 60u, 200000u);
    const auto warmupFrames = ResolveCount("NLS_VFX_PERF_GPU_WARMUP_FRAMES", 8u, 4096u);
    const auto nativeBackend = device->GetAdapter()->GetBackendType();
    const bool orderingReadback = OrderingReadbackEnabled();
    auto pool = device->CreateCommandPool(rhi::QueueType::Graphics, "VfxPerformance");
    ASSERT_NE(pool, nullptr);

    const auto root = std::filesystem::temp_directory_path() / ("NullusVfxPerformance_" + NLS::Guid::New().ToString());
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup { root };

    const auto measure = [&](const uint32_t capacity) -> std::optional<ScalingReport>
    {
        const auto system = MakeBenchmarkSystem(capacity, SimulationTarget::GPU);
        const auto& emitterAsset = system.emitters.front();
        CompiledSystem compiled;
        std::vector<Diagnostic> diagnostics;
        if (!CompileSystem(system, compiled, diagnostics))
        {
            ADD_FAILURE() << (diagnostics.empty() ? std::string {} : diagnostics.front().message);
            return std::nullopt;
        }
        if (!CompilePlatformPrograms(compiled, std::span(&nativeBackend, 1), root / std::to_string(capacity), diagnostics))
        {
            ADD_FAILURE() << (diagnostics.empty() ? std::string {} : diagnostics.front().message);
            return std::nullopt;
        }

        std::string error;
        if (!DeserializeCompiledSystem(SerializeCompiledSystem(compiled, false), compiled, error))
        {
            ADD_FAILURE() << error;
            return std::nullopt;
        }
        GpuEmitter emitter;
        if (!emitter.Initialize(device, emitterAsset, compiled.emitters[0],
            compiled.FindPlatform(nativeBackend)->emitters[0].simulation, error))
        {
            ADD_FAILURE() << error;
            return std::nullopt;
        }

        uint64_t frameId = 0;
        const auto tick = [&](const double delta) -> bool
        {
            auto commands = pool->CreateCommandBuffer("VfxPerformanceFrame");
            if (!commands)
                return false;
            commands->Begin();
            auto packet = emitter.RecordFrame(*commands, ++frameId, delta, error);
            if (!packet)
                return false;
            commands->End();
            auto fence = device->CreateFence("VfxPerformanceFrameComplete");
            if (!fence)
                return false;
            fence->Reset();
            rhi::RHISubmitDesc submit;
            submit.commandBuffers = { commands };
            submit.signalFence = fence;
            const auto submitted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
            if (!submitted.Succeeded())
                return false;
            if (!emitter.CommitFrame(packet))
                return false;
            if (orderingReadback)
            {
                std::string boundsError;
                if (!emitter.BeginOrderingBoundsReadback(*packet,boundsError))
                    return false;
                if (!emitter.PollOrderingBoundsReadback(boundsError))
                    return false;
            }
            return fence->Wait(30000000000ull);
        };

        for (uint32_t frame = 0; frame < warmupFrames; ++frame)
            if (!tick(kBenchmarkDelta))
            {
                ADD_FAILURE() << error;
                return std::nullopt;
            }

        std::vector<double> samples;
        samples.reserve(frames);
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            const auto begin = std::chrono::steady_clock::now();
            const bool ticked = tick(kBenchmarkDelta);
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            if (!ticked)
            {
                ADD_FAILURE() << error;
                return std::nullopt;
            }
            samples.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
        }

        ScalingReport report;
        report.capacity = capacity;
        report.particles = capacity;
        report.simulationBytes = SimulationBytes(emitterAsset.attributes, capacity);
        report.timing = Summarize(std::move(samples));
        return report;
    };

    std::cout << "[VFX perf] backend=gpu orderingReadback=" << orderingReadback << std::endl;
    std::vector<ScalingReport> reports;
    for (const auto capacity : capacities)
    {
        auto report = measure(capacity);
        if (!report)
            return;
        ReportScaling(*report, "gpu");
        reports.push_back(*report);
    }

    // The backend exposes no timestamp query yet, so every sample includes the
    // cost of creating a command buffer, submitting, and draining the queue.
    // Measuring a one-particle frame makes that floor explicit; the difference
    // against the sweep is the part that actually scales with particle count.
    if (auto floor = measure(1u))
    {
        std::cout << "[VFX perf] backend=gpu floor meanMs=" << floor->timing.meanMs
                  << " p95Ms=" << floor->timing.p95Ms << std::endl;
        ::testing::Test::RecordProperty("vfx_gpu_floor_mean_ms", std::to_string(floor->timing.meanMs));
        if (!reports.empty() && reports.back().timing.meanMs > floor->timing.meanMs)
        {
            const auto marginal = reports.back().timing.meanMs - floor->timing.meanMs;
            std::cout << "[VFX perf] backend=gpu marginalMeanMs=" << marginal
                      << " overCapacity=" << reports.back().capacity << std::endl;
            ::testing::Test::RecordProperty("vfx_gpu_marginal_mean_ms", std::to_string(marginal));
        }
    }

    WriteReportIfRequested("vfx_particle_gpu_scaling", reports);
}

/**
 * Emitter lifecycle soak.
 *
 * Repeats create/fill/reset/destroy cycles for a bounded default window and
 * checks two bounded-resource properties that a leak or an unbounded growth
 * policy would break: storage stays within the configured hard capacity, and
 * the per-tick cost does not degrade across cycles.
 *
 * The 30 minute acceptance run is requested explicitly:
 *
 *   $env:NLS_VFX_SOAK_SECONDS = '1800'
 */
TEST(VfxParticlePerformanceTests, EmitterLifecycleStabilitySoak)
{
    const auto capacity = ResolveCount("NLS_VFX_PERF_SOAK_CAPACITY", 20000u, 10000000u);
    const auto seconds = ResolveCount("NLS_VFX_SOAK_SECONDS", 30u, 86400u);
    const auto cycles = ResolveCount("NLS_VFX_SOAK_CYCLES", 0u, 1000000u);
    const ScopedBenchmarkJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.Initialized()) << "Soak needs the same worker pool the editor uses";

    const auto system = MakeBenchmarkSystem(capacity, SimulationTarget::CPU);
    const auto& emitterAsset = system.emitters.front();
    CompiledSystem compiled;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(system, compiled, diagnostics))
        << (diagnostics.empty() ? std::string {} : diagnostics.front().message);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::vector<double> cycleMeans;
    uint32_t completedCycles = 0;
    uint32_t completedFrames = 0;
    while (std::chrono::steady_clock::now() < deadline && (cycles == 0u || completedCycles < cycles))
    {
        CpuEmitter emitter;
        std::string error;
        ASSERT_TRUE(emitter.Initialize(emitterAsset, compiled.emitters[0].spawn, compiled.emitters[0].update, error)) << error;
        for (uint32_t frame = 0; frame < 256u && emitter.Data().Count() < capacity; ++frame)
            ASSERT_TRUE(emitter.Tick(kBenchmarkDelta)) << error;
        ASSERT_EQ(emitter.Data().Count(), capacity);

        std::vector<double> samples;
        for (uint32_t frame = 0; frame < 30u; ++frame)
        {
            const auto begin = std::chrono::steady_clock::now();
            const bool ticked = emitter.Tick(kBenchmarkDelta);
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            ASSERT_TRUE(ticked) << error;
            samples.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
        }
        completedFrames += static_cast<uint32_t>(samples.size());

        // Reset must release the live particles without growing the hard limit.
        emitter.Reset();
        EXPECT_EQ(emitter.Data().Count(), 0u);
        EXPECT_LE(emitter.Data().Capacity(), capacity);

        cycleMeans.push_back(Summarize(std::move(samples)).meanMs);
        ++completedCycles;
    }

    ASSERT_GT(completedCycles, 0u);
    const auto firstMean = cycleMeans.front();
    const auto lastMean = cycleMeans.back();
    std::cout << "[VFX soak] cycles=" << completedCycles
              << " frames=" << completedFrames
              << " capacity=" << capacity
              << " firstCycleMeanMs=" << firstMean
              << " lastCycleMeanMs=" << lastMean
              << std::endl;
    ::testing::Test::RecordProperty("vfx_soak_cycles", std::to_string(completedCycles));
    ::testing::Test::RecordProperty("vfx_soak_frames", std::to_string(completedFrames));
    ::testing::Test::RecordProperty("vfx_soak_first_cycle_mean_ms", std::to_string(firstMean));
    ::testing::Test::RecordProperty("vfx_soak_last_cycle_mean_ms", std::to_string(lastMean));

    // A leak or an unbounded growth policy shows up as a steady per-tick cost
    // increase; the tolerance keeps scheduler noise from failing the soak.
    EXPECT_LE(lastMean, firstMean * 3.0 + 1.0);
}
