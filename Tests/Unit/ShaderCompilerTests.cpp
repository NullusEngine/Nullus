#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Rendering/ShaderCompiler/ShaderCompiler.h"

namespace
{
    class ConcurrentShaderCompilerBackend final : public NLS::Render::ShaderCompiler::IShaderCompilerBackend
    {
    public:
        explicit ConcurrentShaderCompilerBackend(std::atomic<int>& maxCompileConcurrency)
            : m_maxCompileConcurrency(maxCompileConcurrency)
        {
        }

        NLS::Render::ShaderCompiler::ShaderCompilationOutput Compile(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput& input) override
        {
            const int active = ++m_activeCompiles;
            int observed = m_maxCompileConcurrency.load();
            while (active > observed &&
                !m_maxCompileConcurrency.compare_exchange_weak(observed, active))
            {
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(30));

            NLS::Render::ShaderCompiler::ShaderCompilationOutput output;
            output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
            output.diagnostics = input.options.entryPoint;
            output.bytecode.push_back(static_cast<uint8_t>(input.stage));
            --m_activeCompiles;
            return output;
        }

        NLS::Render::Resources::ShaderReflection Reflect(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput&) override
        {
            return {};
        }

        NLS::Render::Resources::ShaderReflection Reflect(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput& input,
            const NLS::Render::ShaderCompiler::ShaderCompilationOutput&) override
        {
            NLS::Render::Resources::ShaderReflection reflection;
            NLS::Render::Resources::ShaderConstantBufferDesc buffer;
            buffer.name = input.options.entryPoint;
            buffer.stage = input.stage;
            reflection.constantBuffers.push_back(std::move(buffer));
            return reflection;
        }

        const char* GetBackendName() const override
        {
            return "ConcurrentShaderCompilerBackend";
        }

    private:
        std::atomic<int>& m_maxCompileConcurrency;
        std::atomic<int> m_activeCompiles{ 0 };
    };

    NLS::Render::ShaderCompiler::ShaderCompilationInput MakeShaderInput(
        NLS::Render::ShaderCompiler::ShaderStage stage,
        std::string entryPoint)
    {
        NLS::Render::ShaderCompiler::ShaderCompilationInput input;
        input.assetPath = "App/Assets/Test/BatchShader.hlsl";
        input.stage = stage;
        input.options.entryPoint = std::move(entryPoint);
        input.options.targetProfile = "test_6_0";
        return input;
    }
}

TEST(ShaderCompilerTests, CompileBatchRunsIndependentStagesConcurrentlyAndPreservesOrder)
{
    std::atomic<int> maxCompileConcurrency{ 0 };
    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ConcurrentShaderCompilerBackend>(maxCompileConcurrency));

    const std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput> inputs = {
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"),
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Pixel, "PSMain"),
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Compute, "CSMain")
    };

    const auto outputs = compiler.CompileBatch(inputs);

    ASSERT_EQ(outputs.size(), inputs.size());
    EXPECT_GT(maxCompileConcurrency.load(), 1);
    EXPECT_EQ(outputs[0].diagnostics, "VSMain");
    EXPECT_EQ(outputs[1].diagnostics, "PSMain");
    EXPECT_EQ(outputs[2].diagnostics, "CSMain");
}

TEST(ShaderCompilerTests, ReflectBatchPreservesInputOrderForCompiledOutputs)
{
    std::atomic<int> maxCompileConcurrency{ 0 };
    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ConcurrentShaderCompilerBackend>(maxCompileConcurrency));

    std::vector<NLS::Render::ShaderCompiler::ShaderReflectionInput> inputs;
    for (const auto& input : {
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"),
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Pixel, "PSMain") })
    {
        NLS::Render::ShaderCompiler::ShaderCompilationOutput output;
        output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
        output.artifactPath = input.options.entryPoint + ".dxil";
        inputs.push_back({ input, output });
    }

    const auto reflections = compiler.ReflectBatch(inputs);

    ASSERT_EQ(reflections.size(), inputs.size());
    ASSERT_EQ(reflections[0].constantBuffers.size(), 1u);
    ASSERT_EQ(reflections[1].constantBuffers.size(), 1u);
    EXPECT_EQ(reflections[0].constantBuffers[0].name, "VSMain");
    EXPECT_EQ(reflections[1].constantBuffers[0].name, "PSMain");
}

TEST(ShaderCompilerTests, ShaderCompilationCacheKeyIncludesDxcIdentityAndArgumentSchema)
{
    auto input = MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain");
    input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    input.options.enableDebugInfo = true;

    NLS::Render::ShaderCompiler::ShaderCompilerToolchainIdentity toolchain;
    toolchain.compilerPath = "C:/Tools/DXC/1.9.2602/bin/x64/dxc.exe";
    toolchain.compilerVersion = "dxcompiler 1.9.2602";
    toolchain.argumentSchemaVersion = "dxc-args-v1";

    const auto baseKey = NLS::Render::ShaderCompiler::BuildShaderCompilationCacheKey(
        input,
        "source-and-include-fingerprint",
        toolchain);

    auto changedPath = toolchain;
    changedPath.compilerPath = "D:/SDK/bin/x64/dxc.exe";
    const auto pathKey = NLS::Render::ShaderCompiler::BuildShaderCompilationCacheKey(
        input,
        "source-and-include-fingerprint",
        changedPath);

    auto changedVersion = toolchain;
    changedVersion.compilerVersion = "dxcompiler 1.10.0";
    const auto versionKey = NLS::Render::ShaderCompiler::BuildShaderCompilationCacheKey(
        input,
        "source-and-include-fingerprint",
        changedVersion);

    auto changedArguments = toolchain;
    changedArguments.argumentSchemaVersion = "dxc-args-v2";
    const auto argumentsKey = NLS::Render::ShaderCompiler::BuildShaderCompilationCacheKey(
        input,
        "source-and-include-fingerprint",
        changedArguments);

    EXPECT_NE(baseKey, pathKey);
    EXPECT_NE(baseKey, versionKey);
    EXPECT_NE(baseKey, argumentsKey);
}

TEST(ShaderCompilerTests, ShaderArtifactStagingPlanUsesSameDirectoryTempFileAndLock)
{
    const auto finalPath = (std::filesystem::temp_directory_path() / "NullusShaderCompilerTests" / "shader.dxil").string();

    const auto plan = NLS::Render::ShaderCompiler::BuildShaderArtifactStagingPlan(finalPath, "unit-test");

    EXPECT_EQ(std::filesystem::path(plan.finalPath), std::filesystem::path(finalPath));
    EXPECT_EQ(std::filesystem::path(plan.temporaryPath).parent_path(), std::filesystem::path(finalPath).parent_path());
    EXPECT_EQ(std::filesystem::path(plan.lockPath).parent_path(), std::filesystem::path(finalPath).parent_path());
    EXPECT_NE(plan.temporaryPath, plan.finalPath);
    EXPECT_NE(plan.lockPath, plan.finalPath);
    EXPECT_NE(plan.temporaryPath.find(".tmp"), std::string::npos);
    EXPECT_NE(plan.lockPath.find(".lock"), std::string::npos);
}

TEST(ShaderCompilerTests, ShaderArtifactTextWritesCommitThroughTemporaryFile)
{
    const auto directory = std::filesystem::temp_directory_path() / "NullusShaderCompilerTests";
    std::filesystem::create_directories(directory);
    const auto finalPath = directory / "atomic-shader-artifact.txt";
    std::filesystem::remove(finalPath);

    std::string diagnostics;
    ASSERT_TRUE(NLS::Render::ShaderCompiler::WriteShaderArtifactTextAtomic(
        finalPath.string(),
        "first artifact",
        &diagnostics)) << diagnostics;
    ASSERT_TRUE(NLS::Render::ShaderCompiler::WriteShaderArtifactTextAtomic(
        finalPath.string(),
        "second artifact",
        &diagnostics)) << diagnostics;

    std::ifstream stream(finalPath, std::ios::binary);
    ASSERT_TRUE(stream);
    const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "second artifact");

    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        const auto filename = entry.path().filename().string();
        EXPECT_EQ(filename.find(".tmp"), std::string::npos) << entry.path().string();
    }
}

TEST(ShaderCompilerTests, ShaderArtifactLockReportsOwnerAndRecoversStaleLocks)
{
    const auto directory = std::filesystem::temp_directory_path() / "NullusShaderCompilerTests";
    std::filesystem::create_directories(directory);
    const auto finalPath = directory / "artifact-lock-metadata.txt";
    const auto plan = NLS::Render::ShaderCompiler::BuildShaderArtifactStagingPlan(finalPath.string(), "text");

    std::filesystem::remove(finalPath);
    std::filesystem::remove_all(plan.lockPath);
    ASSERT_TRUE(std::filesystem::create_directory(plan.lockPath));
    {
        std::ofstream metadata(std::filesystem::path(plan.lockPath) / "owner.txt", std::ios::binary);
        metadata << "owner=stale-unit-test\ncreatedUnixMs=1\n";
    }

    NLS::Render::ShaderCompiler::ShaderArtifactLockOptions recoverOptions;
    recoverOptions.owner = "recovering-unit-test";
    recoverOptions.timeoutMilliseconds = 100u;
    recoverOptions.retryIntervalMilliseconds = 1u;
    recoverOptions.staleAfterMilliseconds = 1u;

    std::string diagnostics;
    ASSERT_TRUE(NLS::Render::ShaderCompiler::WriteShaderArtifactTextAtomic(
        finalPath.string(),
        "recovered",
        recoverOptions,
        &diagnostics)) << diagnostics;
    EXPECT_NE(diagnostics.find("stale-unit-test"), std::string::npos);
    EXPECT_NE(diagnostics.find("stale shader artifact lock"), std::string::npos);

    ASSERT_FALSE(std::filesystem::exists(plan.lockPath));
    ASSERT_TRUE(std::filesystem::create_directory(plan.lockPath));
    {
        std::ofstream metadata(std::filesystem::path(plan.lockPath) / "owner.txt", std::ios::binary);
        metadata << "owner=active-unit-test\ncreatedUnixMs=9999999999999\n";
    }

    NLS::Render::ShaderCompiler::ShaderArtifactLockOptions timeoutOptions;
    timeoutOptions.owner = "waiting-unit-test";
    timeoutOptions.timeoutMilliseconds = 2u;
    timeoutOptions.retryIntervalMilliseconds = 1u;
    timeoutOptions.staleAfterMilliseconds = 60000u;

    diagnostics.clear();
    EXPECT_FALSE(NLS::Render::ShaderCompiler::WriteShaderArtifactTextAtomic(
        finalPath.string(),
        "blocked",
        timeoutOptions,
        &diagnostics));
    EXPECT_NE(diagnostics.find("active-unit-test"), std::string::npos);
    EXPECT_NE(diagnostics.find("timed out"), std::string::npos);

    std::filesystem::remove_all(plan.lockPath);
}

TEST(ShaderCompilerTests, ShaderCompilerProcessReportsCommandLineAndOutputForSucceededProcess)
{
#if defined(_WIN32)
    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 2000u;

    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "powershell.exe",
        { "-NoProfile", "-NonInteractive", "-Command", "Write-Output shader-process-ready" },
        options);

    EXPECT_EQ(result.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::Succeeded)
        << "exit=" << result.exitCode << "\noutput=" << result.output << "\ndiagnostics=" << result.diagnostics
        << "\ncommand=" << result.commandLine;
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("shader-process-ready"), std::string::npos);
    EXPECT_NE(result.commandLine.find("powershell.exe"), std::string::npos);
#else
    GTEST_SKIP() << "Shader compiler process execution is currently Windows-only.";
#endif
}

TEST(ShaderCompilerTests, ShaderCompilerProcessTimeoutReturnsDiagnostics)
{
#if defined(_WIN32)
    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 100u;

    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "powershell.exe",
        { "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 3" },
        options);

    EXPECT_EQ(result.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::TimedOut);
    EXPECT_NE(result.diagnostics.find("timed out"), std::string::npos);
    EXPECT_NE(result.commandLine.find("powershell.exe"), std::string::npos);
#else
    GTEST_SKIP() << "Shader compiler process execution is currently Windows-only.";
#endif
}

TEST(ShaderCompilerTests, ShaderCompilerProcessTimeoutTerminatesChildProcessTree)
{
#if defined(_WIN32)
    const auto markerPath = std::filesystem::temp_directory_path() / "NullusShaderCompilerTests" / "shader-child-survived.txt";
    std::filesystem::create_directories(markerPath.parent_path());
    std::filesystem::remove(markerPath);

    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 150u;

    const std::string childCommand =
        "Start-Process powershell.exe -ArgumentList '-NoProfile','-NonInteractive','-Command','Start-Sleep -Milliseconds 700; Set-Content -LiteralPath \"" +
        markerPath.string() +
        "\" -Value child-survived'; Start-Sleep -Seconds 5";

    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "powershell.exe",
        { "-NoProfile", "-NonInteractive", "-Command", childCommand },
        options);

    EXPECT_EQ(result.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::TimedOut)
        << result.diagnostics << "\n" << result.output;

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(std::filesystem::exists(markerPath));
#else
    GTEST_SKIP() << "Shader compiler process execution is currently Windows-only.";
#endif
}

TEST(ShaderCompilerTests, ShaderCompilerProcessUsesJobObjectInsteadOfPipePolling)
{
    const auto shaderCompilerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ShaderCompiler/ShaderCompiler.cpp";
    std::ifstream stream(shaderCompilerSourcePath, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    ASSERT_FALSE(source.empty());
    EXPECT_EQ(source.find("PeekNamedPipe"), std::string::npos);
    EXPECT_EQ(source.find("WaitForSingleObject(processInfo.hProcess, 5)"), std::string::npos);
    EXPECT_NE(source.find("CreateJobObject"), std::string::npos);
    EXPECT_NE(source.find("AssignProcessToJobObject"), std::string::npos);
    EXPECT_NE(source.find("TerminateJobObject"), std::string::npos);
}

TEST(ShaderCompilerTests, ShaderCompilerProcessReadPipeHasSingleOwner)
{
    const auto shaderCompilerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ShaderCompiler/ShaderCompiler.cpp";
    std::ifstream stream(shaderCompilerSourcePath, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("const HANDLE outputReadPipe = readPipe;"), std::string::npos);
    EXPECT_NE(source.find("readPipe = nullptr;"), std::string::npos);
    EXPECT_NE(source.find("if (readPipe != nullptr)\n\t\t\tCloseHandle(readPipe);"), std::string::npos);
}

TEST(ShaderCompilerTests, ShaderCompilerProcessCancellationReturnsDiagnostics)
{
#if defined(_WIN32)
    std::atomic_bool cancelled{ true };
    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 2000u;
    options.cancellationFlag = &cancelled;

    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "cmd.exe",
        { "/C", "echo should-not-run" },
        options);

    EXPECT_EQ(result.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::Cancelled);
    EXPECT_NE(result.diagnostics.find("cancelled"), std::string::npos);
    EXPECT_TRUE(result.output.empty());
#else
    GTEST_SKIP() << "Shader compiler process execution is currently Windows-only.";
#endif
}
