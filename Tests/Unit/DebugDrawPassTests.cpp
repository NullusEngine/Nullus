#include <gtest/gtest.h>

#include <any>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Guid.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Debug/DebugDrawPass.h"
#include "Rendering/Debug/DebugDrawService.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Utils/Conversions.h"

namespace
{
    class PassRecordingRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit PassRecordingRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
        }

        void DrawEntity(
            NLS::Render::Data::PipelineState pipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            std::string_view lightMode) override
        {
            (void) lightMode;
            recordedDraws.push_back({
                pipelineState,
                drawable.primitiveMode,
                drawable.mesh != nullptr ? drawable.mesh->GetVertexCount() : 0u,
                drawable.mesh != nullptr ? drawable.mesh->GetIndexCount() : 0u,
                drawable.vertexStart,
                drawable.vertexCount,
                drawable.material != nullptr
                    ? std::any_cast<int>(drawable.material->GetUniformsData().at("u_UseVertexPosition"))
                    : -1
            });
        }

        struct RecordedDraw
        {
            NLS::Render::Data::PipelineState pipelineState;
            NLS::Render::Settings::EPrimitiveMode primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
            uint32_t vertexCount = 0u;
            uint32_t indexCount = 0u;
            uint32_t vertexStart = 0u;
            uint32_t vertexRangeCount = 0u;
            int useVertexPosition = -1;
        };

        std::vector<RecordedDraw> recordedDraws;
    };

    class RecordingDebugDrawPass final : public NLS::Render::Debug::DebugDrawPass
    {
    public:
        explicit RecordingDebugDrawPass(NLS::Render::Core::CompositeRenderer& renderer)
            : DebugDrawPass(renderer)
        {
        }

        void ExecuteForTest(NLS::Render::Data::PipelineState pipelineState)
        {
            Draw(pipelineState);
        }

        std::vector<NLS::Render::Debug::DebugDrawPrimitive> recordedPrimitives;
        std::vector<NLS::Render::Debug::DebugDrawPass::LineBatch> recordedLineBatches;
        std::vector<NLS::Render::Debug::DebugDrawPrimitiveType> recordedOrder;

    protected:
        void RenderPrimitive(const NLS::Render::Debug::DebugDrawPrimitive& primitive, NLS::Render::Data::PipelineState) override
        {
            recordedPrimitives.push_back(primitive);
            recordedOrder.push_back(primitive.type);
        }

        void RenderLineBatch(const NLS::Render::Debug::DebugDrawPass::LineBatch& batch, NLS::Render::Data::PipelineState) override
        {
            recordedLineBatches.push_back(batch);
            recordedOrder.push_back(NLS::Render::Debug::DebugDrawPrimitiveType::Line);
        }
    };

    class ExecutableDebugDrawPass final : public NLS::Render::Debug::DebugDrawPass
    {
    public:
        explicit ExecutableDebugDrawPass(NLS::Render::Core::CompositeRenderer& renderer)
            : DebugDrawPass(renderer)
        {
        }

        void ExecuteForTest(NLS::Render::Data::PipelineState pipelineState)
        {
            Draw(pipelineState);
        }
    };

    void WriteDxcProbeShaderFile(const std::filesystem::path& shaderPath)
    {
        std::filesystem::create_directories(shaderPath.parent_path());
        std::ofstream shaderFile(shaderPath, std::ios::binary);
        shaderFile
            << "struct VSOutput { float4 position : SV_Position; };\n"
            << "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
            << "    VSOutput output;\n"
            << "    output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
            << "    return output;\n"
            << "}\n"
            << "float4 PSMain(VSOutput input) : SV_Target {\n"
            << "    return float4(input.position.x, 0.0f, 0.0f, 1.0f);\n"
            << "}\n";
    }

    NLS::Render::ShaderCompiler::ShaderCompilationInput MakeDxcProbeInput(
        const std::filesystem::path& shaderPath,
        const NLS::Render::ShaderCompiler::ShaderStage stage,
        const NLS::Render::ShaderCompiler::ShaderTargetPlatform targetPlatform)
    {
        NLS::Render::ShaderCompiler::ShaderCompilationInput input;
        input.assetPath = shaderPath.string();
        input.stage = stage;
        input.options.entryPoint =
            stage == NLS::Render::ShaderCompiler::ShaderStage::Vertex ? "VSMain" : "PSMain";
        input.options.targetProfile =
            stage == NLS::Render::ShaderCompiler::ShaderStage::Vertex ? "vs_6_0" : "ps_6_0";
        input.options.targetPlatform = targetPlatform;
        return input;
    }

    bool IsDxcUnavailableDiagnostic(const std::string& diagnostics)
    {
        return diagnostics.find("Unable to locate dxc.exe.") != std::string::npos ||
            diagnostics.find("Unable to locate an executable native dxc.") != std::string::npos ||
            diagnostics.find("Failed to spawn shader compiler process (") != std::string::npos ||
            diagnostics.find("[dxc-exit-code] 126") != std::string::npos ||
            diagnostics.find("[dxc-exit-code] 127") != std::string::npos;
    }

    const std::optional<std::string>& RealDxcProcessExecutionSkipReason()
    {
        static const std::optional<std::string> result = []() -> std::optional<std::string>
        {
            const auto root = std::filesystem::temp_directory_path() /
                ("nullus_debugdraw_dxc_probe_" + NLS::Guid::New().ToString());
            const auto shaderPath = root / "DebugDrawDxcProbe.hlsl";
            WriteDxcProbeShaderFile(shaderPath);

            const std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput> inputs = {
                MakeDxcProbeInput(
                    shaderPath,
                    NLS::Render::ShaderCompiler::ShaderStage::Vertex,
                    NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL),
                MakeDxcProbeInput(
                    shaderPath,
                    NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                    NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL),
                MakeDxcProbeInput(
                    shaderPath,
                    NLS::Render::ShaderCompiler::ShaderStage::Vertex,
                    NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV),
                MakeDxcProbeInput(
                    shaderPath,
                    NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                    NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV)
            };

            const NLS::Render::ShaderCompiler::ShaderCompiler compiler;
            const auto outputs = compiler.CompileBatch(inputs);
            std::filesystem::remove_all(root);

            for (const auto& output : outputs)
            {
                if (output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
                    continue;

                if (IsDxcUnavailableDiagnostic(output.diagnostics))
                    return "DebugDraw line mesh draw coverage requires an executable DXC toolchain: " +
                        output.diagnostics;

                return std::nullopt;
            }

            return std::nullopt;
        }();

        return result;
    }
}

TEST(DebugDrawPassTests, ExplicitPassConsumesOnlyVisiblePrimitivesOncePerExecution)
{
    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    DebugDrawSubmitOptions hiddenOptions;
    hiddenOptions.category = DebugDrawCategory::Lighting;
    service->SetCategoryEnabled(DebugDrawCategory::Lighting, false);

    ASSERT_TRUE(service->SubmitPoint({ 0.0f, 0.0f, 0.0f }));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }));
    ASSERT_TRUE(service->SubmitTriangle(
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f }));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, hiddenOptions));

    RecordingDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});

    ASSERT_EQ(pass.recordedPrimitives.size(), 2u);
    EXPECT_EQ(pass.recordedPrimitives[0].type, DebugDrawPrimitiveType::Point);
    EXPECT_EQ(pass.recordedPrimitives[1].type, DebugDrawPrimitiveType::Triangle);
    ASSERT_EQ(pass.recordedOrder.size(), 3u);
    EXPECT_EQ(pass.recordedOrder[0], DebugDrawPrimitiveType::Point);
    EXPECT_EQ(pass.recordedOrder[1], DebugDrawPrimitiveType::Line);
    EXPECT_EQ(pass.recordedOrder[2], DebugDrawPrimitiveType::Triangle);

    ASSERT_EQ(pass.recordedLineBatches.size(), 1u);
    ASSERT_EQ(pass.recordedLineBatches[0].segments.size(), 1u);
    EXPECT_EQ(pass.recordedLineBatches[0].segments[0].start, (NLS::Maths::Vector3{ 0.0f, 0.0f, 0.0f }));
    EXPECT_EQ(pass.recordedLineBatches[0].segments[0].end, (NLS::Maths::Vector3{ 1.0f, 0.0f, 0.0f }));
}

TEST(DebugDrawPassTests, CompatibleLinesRenderAsSingleBatch)
{
    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    DebugDrawSubmitOptions options;
    options.style.color = { 0.25f, 0.5f, 0.75f };
    options.style.lineWidth = 2.0f;
    options.style.depthMode = DebugDrawDepthMode::DepthTest;

    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, options));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, options));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 2.0f, 0.0f }, { 1.0f, 2.0f, 0.0f }, options));

    RecordingDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});

    EXPECT_TRUE(pass.recordedPrimitives.empty());
    ASSERT_EQ(pass.recordedLineBatches.size(), 1u);

    const auto& batch = pass.recordedLineBatches[0];
    EXPECT_EQ(batch.style.color, (NLS::Maths::Vector3{ 0.25f, 0.5f, 0.75f }));
    EXPECT_EQ(batch.style.lineWidth, 2.0f);
    EXPECT_EQ(batch.style.depthMode, DebugDrawDepthMode::DepthTest);
    ASSERT_EQ(batch.segments.size(), 3u);
    EXPECT_EQ(batch.segments[0].start, (NLS::Maths::Vector3{ 0.0f, 0.0f, 0.0f }));
    EXPECT_EQ(batch.segments[0].end, (NLS::Maths::Vector3{ 1.0f, 0.0f, 0.0f }));
    EXPECT_EQ(batch.segments[2].start, (NLS::Maths::Vector3{ 0.0f, 2.0f, 0.0f }));
    EXPECT_EQ(batch.segments[2].end, (NLS::Maths::Vector3{ 1.0f, 2.0f, 0.0f }));
}

TEST(DebugDrawPassTests, LinesWithDifferentRenderStateSplitIntoSeparateBatches)
{
    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    DebugDrawSubmitOptions baseOptions;
    baseOptions.style.color = { 1.0f, 0.0f, 0.0f };
    baseOptions.style.lineWidth = 1.0f;
    baseOptions.style.depthMode = DebugDrawDepthMode::DepthTest;

    auto colorOptions = baseOptions;
    colorOptions.style.color = { 0.0f, 1.0f, 0.0f };

    auto depthOptions = baseOptions;
    depthOptions.style.depthMode = DebugDrawDepthMode::AlwaysOnTop;

    auto widthOptions = baseOptions;
    widthOptions.style.lineWidth = 4.0f;

    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, baseOptions));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, colorOptions));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 2.0f, 0.0f }, { 1.0f, 2.0f, 0.0f }, depthOptions));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 3.0f, 0.0f }, { 1.0f, 3.0f, 0.0f }, widthOptions));

    RecordingDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});

    EXPECT_TRUE(pass.recordedPrimitives.empty());
    ASSERT_EQ(pass.recordedLineBatches.size(), 4u);
    ASSERT_EQ(pass.recordedOrder.size(), 4u);

    EXPECT_EQ(pass.recordedLineBatches[0].style.color, (NLS::Maths::Vector3{ 1.0f, 0.0f, 0.0f }));
    EXPECT_EQ(pass.recordedLineBatches[1].style.color, (NLS::Maths::Vector3{ 0.0f, 1.0f, 0.0f }));
    EXPECT_EQ(pass.recordedLineBatches[2].style.depthMode, DebugDrawDepthMode::AlwaysOnTop);
    EXPECT_EQ(pass.recordedLineBatches[3].style.lineWidth, 4.0f);
    for (const auto& batch : pass.recordedLineBatches)
        EXPECT_EQ(batch.segments.size(), 1u);
}

TEST(DebugDrawPassTests, NonAdjacentCompatibleLinesDoNotReorderAcrossDifferentLineState)
{
    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    DebugDrawSubmitOptions redOptions;
    redOptions.style.color = { 1.0f, 0.0f, 0.0f };

    DebugDrawSubmitOptions greenOptions;
    greenOptions.style.color = { 0.0f, 1.0f, 0.0f };

    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, redOptions));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, greenOptions));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 2.0f, 0.0f }, { 1.0f, 2.0f, 0.0f }, redOptions));

    RecordingDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});

    ASSERT_EQ(pass.recordedLineBatches.size(), 3u);
    EXPECT_EQ(pass.recordedLineBatches[0].style.color, (NLS::Maths::Vector3{ 1.0f, 0.0f, 0.0f }));
    EXPECT_EQ(pass.recordedLineBatches[1].style.color, (NLS::Maths::Vector3{ 0.0f, 1.0f, 0.0f }));
    EXPECT_EQ(pass.recordedLineBatches[2].style.color, (NLS::Maths::Vector3{ 1.0f, 0.0f, 0.0f }));
}

TEST(DebugDrawPassTests, ThreadedPassClearsOneFrameHelpersAfterFrameEndWithoutDuplicatingPresentation)
{
    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    DebugDrawSubmitOptions oneFrameOptions;
    oneFrameOptions.lifetime.mode = DebugDrawLifetimeMode::OneFrame;
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, oneFrameOptions));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    renderer.BeginFrame(frameDescriptor);
    RecordingDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});
    renderer.EndFrame();

    ASSERT_TRUE(pass.recordedPrimitives.empty());
    ASSERT_EQ(pass.recordedLineBatches.size(), 1u);
    pass.recordedPrimitives.clear();
    pass.recordedLineBatches.clear();
    pass.recordedOrder.clear();
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});
    EXPECT_TRUE(pass.recordedPrimitives.empty());
    EXPECT_TRUE(pass.recordedLineBatches.empty());
}

TEST(DebugDrawPassTests, LineBatchDrawUsesVertexPositionsAndNonIndexedLineList)
{
    if (const auto& skipReason = RealDxcProcessExecutionSkipReason(); skipReason.has_value())
        GTEST_SKIP() << *skipReason;

    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    DebugDrawSubmitOptions options;
    options.style.lineWidth = 4.0f;
    options.style.depthMode = DebugDrawDepthMode::AlwaysOnTop;

    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, options));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, options));

    ExecutableDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});

    ASSERT_EQ(renderer.recordedDraws.size(), 1u);
    const auto& draw = renderer.recordedDraws[0];
    EXPECT_EQ(draw.primitiveMode, NLS::Render::Settings::EPrimitiveMode::LINES);
    EXPECT_EQ(draw.vertexCount, 4u);
    EXPECT_EQ(draw.indexCount, 0u);
    EXPECT_EQ(draw.vertexStart, 0u);
    EXPECT_EQ(draw.vertexRangeCount, 4u);
    EXPECT_EQ(draw.useVertexPosition, 1);
    EXPECT_FALSE(draw.pipelineState.depthWriting);
    EXPECT_FALSE(draw.pipelineState.depthTest);
    EXPECT_EQ(draw.pipelineState.rasterizationMode, NLS::Render::Settings::ERasterizationMode::LINE);
    EXPECT_EQ(draw.pipelineState.lineWidthPow2, NLS::Render::Utils::Conversions::FloatToPow2(4.0f));
}

TEST(DebugDrawPassTests, CompositeRendererExecutePassDrawsDebugLinesDuringActiveFrame)
{
    if (const auto& skipReason = RealDxcProcessExecutionSkipReason(); skipReason.has_value())
        GTEST_SKIP() << *skipReason;

    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    DebugDrawPass pass(renderer);
    renderer.BeginFrame(frameDescriptor);
    renderer.ExecutePass(pass, NLS::Render::Data::PipelineState{});
    renderer.EndFrame();

    ASSERT_EQ(renderer.recordedDraws.size(), 1u);
    EXPECT_EQ(renderer.recordedDraws[0].primitiveMode, NLS::Render::Settings::EPrimitiveMode::LINES);
    EXPECT_EQ(renderer.recordedDraws[0].vertexCount, 2u);
}

TEST(DebugDrawPassTests, SeparateLineStateBatchesShareUploadedMeshAndUseDrawRanges)
{
    if (const auto& skipReason = RealDxcProcessExecutionSkipReason(); skipReason.has_value())
        GTEST_SKIP() << *skipReason;

    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    DebugDrawSubmitOptions redOptions;
    redOptions.style.color = { 1.0f, 0.0f, 0.0f };

    DebugDrawSubmitOptions greenOptions;
    greenOptions.style.color = { 0.0f, 1.0f, 0.0f };

    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, redOptions));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, greenOptions));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 2.0f, 0.0f }, { 1.0f, 2.0f, 0.0f }, redOptions));

    ExecutableDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});

    ASSERT_EQ(renderer.recordedDraws.size(), 3u);
    EXPECT_EQ(renderer.recordedDraws[0].vertexCount, 6u);
    EXPECT_EQ(renderer.recordedDraws[0].vertexStart, 0u);
    EXPECT_EQ(renderer.recordedDraws[0].vertexRangeCount, 2u);
    EXPECT_EQ(renderer.recordedDraws[1].vertexCount, 6u);
    EXPECT_EQ(renderer.recordedDraws[1].vertexStart, 2u);
    EXPECT_EQ(renderer.recordedDraws[1].vertexRangeCount, 2u);
    EXPECT_EQ(renderer.recordedDraws[2].vertexCount, 6u);
    EXPECT_EQ(renderer.recordedDraws[2].vertexStart, 4u);
    EXPECT_EQ(renderer.recordedDraws[2].vertexRangeCount, 2u);
}

TEST(DebugDrawPassTests, LineMeshSlotReusesCapacityWhenLaterFramesNeedFewerVertices)
{
    if (const auto& skipReason = RealDxcProcessExecutionSkipReason(); skipReason.has_value())
        GTEST_SKIP() << *skipReason;

#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(16u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    ExecutableDebugDrawPass pass(renderer);
    const auto executeWithLines = [&](const uint32_t lineCount)
    {
        for (uint32_t lineIndex = 0u; lineIndex < lineCount; ++lineIndex)
        {
            const auto y = static_cast<float>(lineIndex);
            ASSERT_TRUE(service->SubmitLine({ 0.0f, y, 0.0f }, { 1.0f, y, 0.0f }));
        }
        pass.ExecuteForTest(NLS::Render::Data::PipelineState{});
        service->EndFrame();
        renderer.recordedDraws.clear();
    };

    executeWithLines(3u);
    const auto* firstSlotMesh = DebugDrawPassTestAccess::GetLineMeshSlotMesh(pass, 0u);
    ASSERT_NE(firstSlotMesh, nullptr);
    EXPECT_EQ(DebugDrawPassTestAccess::GetLineMeshSlotCapacity(pass, 0u), 6u);
    EXPECT_EQ(firstSlotMesh->GetVertexCount(), 6u);

    executeWithLines(1u);
    executeWithLines(1u);
    executeWithLines(1u);

    EXPECT_EQ(DebugDrawPassTestAccess::GetLineMeshSlotMesh(pass, 0u), firstSlotMesh);
    EXPECT_EQ(DebugDrawPassTestAccess::GetLineMeshSlotCapacity(pass, 0u), 6u);
    EXPECT_EQ(DebugDrawPassTestAccess::GetLineMeshSlotMesh(pass, 0u)->GetVertexCount(), 6u);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect DebugDrawPass line mesh slots.";
#endif
}

TEST(DebugDrawPassTests, PersistentLineGroupReusesUploadedMeshUntilContentChanges)
{
    if (const auto& skipReason = RealDxcProcessExecutionSkipReason(); skipReason.has_value())
        GTEST_SKIP() << *skipReason;

#if defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(16u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    const auto makePersistentLine = [](const float y)
    {
        DebugDrawPrimitive primitive;
        primitive.type = DebugDrawPrimitiveType::Line;
        primitive.points[0] = { 0.0f, y, 0.0f };
        primitive.points[1] = { 1.0f, y, 0.0f };
        primitive.options.lifetime = DebugDrawLifetime::Persistent();
        return primitive;
    };

    constexpr uint64_t groupId = 17u;
    ASSERT_TRUE(service->SetPersistentPrimitiveGroup(groupId, { makePersistentLine(0.0f) }));
    const auto stableRevision = service->GetContentRevision();

    ExecutableDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});
    ASSERT_EQ(renderer.recordedDraws.size(), 1u);
    EXPECT_EQ(DebugDrawPassTestAccess::GetCommandBuildCount(pass), 1u);
    EXPECT_EQ(DebugDrawPassTestAccess::GetLineMeshUploadCount(pass), 1u);
    ASSERT_EQ(DebugDrawPassTestAccess::GetCachedLineMaterialCount(pass), 1u);
    const auto stableMaterialRevision =
        DebugDrawPassTestAccess::GetCachedLineMaterialParameterRevision(pass, 0u);
    ASSERT_NE(stableMaterialRevision, 0u);

    renderer.recordedDraws.clear();
    service->EndFrame();
    EXPECT_EQ(service->GetContentRevision(), stableRevision);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});
    ASSERT_EQ(renderer.recordedDraws.size(), 1u);
    EXPECT_EQ(DebugDrawPassTestAccess::GetCommandBuildCount(pass), 1u);
    EXPECT_EQ(DebugDrawPassTestAccess::GetLineMeshUploadCount(pass), 1u);
    EXPECT_EQ(
        DebugDrawPassTestAccess::GetCachedLineMaterialParameterRevision(pass, 0u),
        stableMaterialRevision);

    renderer.recordedDraws.clear();
    ASSERT_TRUE(service->SetPersistentPrimitiveGroup(groupId, { makePersistentLine(2.0f) }));
    EXPECT_GT(service->GetContentRevision(), stableRevision);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});
    ASSERT_EQ(renderer.recordedDraws.size(), 1u);
    EXPECT_EQ(DebugDrawPassTestAccess::GetCommandBuildCount(pass), 2u);
    EXPECT_EQ(DebugDrawPassTestAccess::GetLineMeshUploadCount(pass), 2u);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect DebugDrawPass uploads.";
#endif
}
