#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "Guid.h"
#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/ShaderCompiler/ShaderCacheDatabase.h"

namespace
{
    std::filesystem::path NormalizeComparablePath(const std::filesystem::path& path)
    {
        std::error_code error;
        auto normalized = std::filesystem::weakly_canonical(path, error);
        if (error)
            normalized = path.lexically_normal();
        return normalized;
    }

    void WriteTinyVertexShaderFile(const std::filesystem::path& shaderPath)
    {
        std::filesystem::create_directories(shaderPath.parent_path());
        std::ofstream shaderFile(shaderPath, std::ios::binary);
        shaderFile
            << "struct VSOutput { float4 position : SV_Position; };\n"
            << "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
            << "    VSOutput output;\n"
            << "    output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
            << "    return output;\n"
            << "}\n";
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    bool IsDxcUnavailableDiagnostic(const std::string& diagnostics)
    {
        return diagnostics.find("Unable to locate dxc.exe.") != std::string::npos ||
            diagnostics.find("Unable to locate an executable native dxc.") != std::string::npos ||
            diagnostics.find("Failed to spawn shader compiler process (") != std::string::npos ||
            diagnostics.find("[dxc-exit-code] 126") != std::string::npos ||
            diagnostics.find("[dxc-exit-code] 127") != std::string::npos;
    }

    class ScopedEnvironmentVariable final
    {
    public:
        ScopedEnvironmentVariable(std::string name, std::string value)
            : m_name(std::move(name))
        {
            if (const char* existing = std::getenv(m_name.c_str()); existing != nullptr)
                m_previousValue = existing;
            SetValue(value);
        }

        ~ScopedEnvironmentVariable()
        {
            if (m_previousValue.has_value())
                SetValue(*m_previousValue);
            else
                ClearValue();
        }

        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    private:
        void SetValue(const std::string& value) const
        {
#if defined(_WIN32)
            _putenv_s(m_name.c_str(), value.c_str());
#else
            setenv(m_name.c_str(), value.c_str(), 1);
#endif
        }

        void ClearValue() const
        {
#if defined(_WIN32)
            _putenv_s(m_name.c_str(), "");
#else
            unsetenv(m_name.c_str());
#endif
        }

        std::string m_name;
        std::optional<std::string> m_previousValue;
    };

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

    class PersistentShaderCompilerBackend final : public NLS::Render::ShaderCompiler::IShaderCompilerBackend
    {
    public:
        NLS::Render::ShaderCompiler::ShaderCompilationOutput Compile(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput& input) override
        {
            NLS::Render::ShaderCompiler::ShaderCompilationOutput output;
            output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
            output.cacheKey = "persistent-" + input.options.entryPoint;
            output.artifactPath = "Library/ShaderCache/" + output.cacheKey + ".dxil";
            output.bytecode = {7u, 8u, 9u};
            output.dependencyPaths = {input.assetPath, ":Shaders/Common.hlsl"};
            return output;
        }

        NLS::Render::Resources::ShaderReflection Reflect(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput&) override
        {
            return {};
        }

        NLS::Render::Resources::ShaderReflection Reflect(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput&,
            const NLS::Render::ShaderCompiler::ShaderCompilationOutput&) override
        {
            return {};
        }

        const char* GetBackendName() const override
        {
            return "PersistentShaderCompilerBackend";
        }
    };

    class ArtifactDirectoryEchoBackend final : public NLS::Render::ShaderCompiler::IShaderCompilerBackend
    {
    public:
        NLS::Render::ShaderCompiler::ShaderCompilationOutput Compile(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput& input) override
        {
            NLS::Render::ShaderCompiler::ShaderCompilationOutput output;
            output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
            output.cacheKey = "artifact-directory-" + input.options.entryPoint;
            output.diagnostics = input.options.artifactDirectory;
            output.bytecode = {1u};
            if (!input.options.artifactDirectory.empty())
            {
                output.artifactPath = (
                    std::filesystem::path(input.options.artifactDirectory) /
                    (input.options.entryPoint + ".dxil")).string();
            }
            return output;
        }

        NLS::Render::Resources::ShaderReflection Reflect(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput& input) override
        {
            return MakeReflection(input);
        }

        NLS::Render::Resources::ShaderReflection Reflect(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput& input,
            const NLS::Render::ShaderCompiler::ShaderCompilationOutput&) override
        {
            return MakeReflection(input);
        }

        const char* GetBackendName() const override
        {
            return "ArtifactDirectoryEchoBackend";
        }

    private:
        static NLS::Render::Resources::ShaderReflection MakeReflection(
            const NLS::Render::ShaderCompiler::ShaderCompilationInput& input)
        {
            NLS::Render::Resources::ShaderReflection reflection;
            NLS::Render::Resources::ShaderConstantBufferDesc buffer;
            buffer.name = input.options.artifactDirectory;
            buffer.stage = input.stage;
            reflection.constantBuffers.push_back(std::move(buffer));
            return reflection;
        }
    };

    class ScopedShaderLoaderProjectAssetsRoot final
    {
    public:
        explicit ScopedShaderLoaderProjectAssetsRoot(const std::string& projectAssetsPath)
        {
            NLS::Render::Resources::Loaders::ShaderLoader::SetDefaultProjectAssetsPath(projectAssetsPath);
        }

        ~ScopedShaderLoaderProjectAssetsRoot()
        {
            NLS::Render::Resources::Loaders::ShaderLoader::SetDefaultProjectAssetsPath({});
        }
    };

    class ScopedShaderManagerAssetPaths final
    {
    public:
        ScopedShaderManagerAssetPaths(
            const std::string& projectAssetsPath,
            const std::string& engineAssetsPath)
        {
            NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(
                projectAssetsPath,
                engineAssetsPath);
        }

        ~ScopedShaderManagerAssetPaths()
        {
            NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
            NLS::Render::Resources::Loaders::ShaderLoader::SetDefaultProjectAssetsPath({});
        }
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

TEST(ShaderCompilerTests, DxcExecutableLookupPrefersProjectBundledCompilerBeforeEnvironmentAndSdkFallbacks)
{
    const auto shaderCompilerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ShaderCompiler/ShaderCompiler.cpp";
    const std::string source = ReadTextFile(shaderCompilerSourcePath);

    const auto findDxcFunction = source.find("std::optional<std::filesystem::path> FindDxcExecutable()");
    ASSERT_NE(findDxcFunction, std::string::npos);
    const auto nextFunction = source.find("std::filesystem::path GetShaderCacheDirectory()", findDxcFunction);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto body = source.substr(findDxcFunction, nextFunction - findDxcFunction);

    const auto bundledProbe = body.find("if (const auto bundledPath = findBundledDxc");
    const auto sourcePathProbe = body.find("__FILE__");
    const auto currentPathProbe = body.find("std::filesystem::current_path()");
    const auto dxcPathEnv = body.find("\"DXC_PATH\"");
    const auto vulkanSdkEnv = body.find("\"VULKAN_SDK\"");
    const auto vkSdkPathEnv = body.find("\"VK_SDK_PATH\"");
    const auto windowsSdk = body.find("Windows Kits/10");
    ASSERT_NE(bundledProbe, std::string::npos);
    ASSERT_NE(sourcePathProbe, std::string::npos);
    ASSERT_NE(currentPathProbe, std::string::npos);
    ASSERT_NE(dxcPathEnv, std::string::npos);
    ASSERT_NE(vulkanSdkEnv, std::string::npos);
    ASSERT_NE(vkSdkPathEnv, std::string::npos);
    ASSERT_NE(windowsSdk, std::string::npos);

    EXPECT_LT(sourcePathProbe, currentPathProbe);
    EXPECT_LT(bundledProbe, dxcPathEnv);
    EXPECT_LT(bundledProbe, vulkanSdkEnv);
    EXPECT_LT(bundledProbe, vkSdkPathEnv);
    EXPECT_LT(bundledProbe, windowsSdk);
}

TEST(ShaderCompilerTests, CompileBatchPersistsShaderCacheWithSingleDatabaseFlush)
{
    const auto shaderCompilerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ShaderCompiler/ShaderCompiler.cpp";
    const std::string source = ReadTextFile(shaderCompilerSourcePath);

    const auto compileBatch = source.find("std::vector<ShaderCompilationOutput> ShaderCompiler::CompileBatch");
    ASSERT_NE(compileBatch, std::string::npos);
    const auto nextFunction = source.find("ShaderCompilationOutput ShaderCompiler::Compile(ShaderAsset&", compileBatch);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto body = source.substr(compileBatch, nextFunction - compileBatch);

    EXPECT_EQ(body.find("return Compile(input);"), std::string::npos);
    const auto preparedInputs = body.find("std::vector<ShaderCompilationInput> preparedInputs");
    const auto futureJoin = body.find("outputs[index] = futures[index].get()");
    const auto cacheFlush = body.find("PersistCacheRecords(preparedInputs, outputs)");
    ASSERT_NE(preparedInputs, std::string::npos);
    ASSERT_NE(futureJoin, std::string::npos);
    ASSERT_NE(cacheFlush, std::string::npos);
    EXPECT_LT(preparedInputs, futureJoin);
    EXPECT_LT(futureJoin, cacheFlush);
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

    auto changedArtifactDirectory = input;
    changedArtifactDirectory.options.artifactDirectory = "D:/Project/Library/ShaderCache";
    const auto artifactDirectoryKey = NLS::Render::ShaderCompiler::BuildShaderCompilationCacheKey(
        changedArtifactDirectory,
        "source-and-include-fingerprint",
        toolchain);

    EXPECT_NE(baseKey, pathKey);
    EXPECT_NE(baseKey, versionKey);
    EXPECT_NE(baseKey, argumentsKey);
    EXPECT_EQ(baseKey, artifactDirectoryKey);
}

TEST(ShaderCompilerTests, ReflectDxcStructuredBuffersAsStructuredBuffersWithElementStride)
{
    const auto directory = std::filesystem::temp_directory_path() / "NullusShaderCompilerTests";
    std::filesystem::create_directories(directory);
    const auto sourcePath = directory / ("structured-buffer-reflection-" + NLS::Guid::New().ToString() + ".hlsl");
    {
        std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
        source <<
            "StructuredBuffer<float4x4> ObjectData : register(t0, space3);\n"
            "RWStructuredBuffer<uint> OutputData : register(u1, space2);\n"
            "[numthreads(1, 1, 1)]\n"
            "void Main(uint3 id : SV_DispatchThreadID) {\n"
            "    OutputData[id.x] = asuint(ObjectData[id.x][0][0]);\n"
            "}\n";
    }

    NLS::Render::ShaderCompiler::ShaderCompilationInput input;
    input.assetPath = sourcePath.string();
    input.stage = NLS::Render::ShaderCompiler::ShaderStage::Compute;
    input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    input.options.targetProfile = "cs_6_0";
    input.options.entryPoint = "Main";

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    const auto output = compiler.Compile(input);
    if (output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
    {
        std::filesystem::remove(sourcePath);
        GTEST_SKIP() << "DXC is unavailable for shader reflection regression test: " << output.diagnostics;
    }

    const auto reflection = compiler.Reflect(input, output);
    const auto objectData = std::find_if(
        reflection.properties.begin(),
        reflection.properties.end(),
        [](const NLS::Render::Resources::ShaderPropertyDesc& property)
        {
            return property.name == "ObjectData";
        });
    ASSERT_NE(objectData, reflection.properties.end());
    EXPECT_EQ(objectData->kind, NLS::Render::Resources::ShaderResourceKind::StructuredBuffer);
    EXPECT_EQ(objectData->bindingIndex, 0u);
    EXPECT_EQ(objectData->bindingSpace, 3u);
    EXPECT_EQ(objectData->byteSize, 64u);

    const auto outputData = std::find_if(
        reflection.properties.begin(),
        reflection.properties.end(),
        [](const NLS::Render::Resources::ShaderPropertyDesc& property)
        {
            return property.name == "OutputData";
        });
    ASSERT_NE(outputData, reflection.properties.end());
    EXPECT_EQ(outputData->kind, NLS::Render::Resources::ShaderResourceKind::StorageBuffer);
    EXPECT_EQ(outputData->bindingIndex, 1u);
    EXPECT_EQ(outputData->bindingSpace, 2u);
    EXPECT_EQ(outputData->byteSize, 4u);

    std::filesystem::remove(sourcePath);
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
    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 2000u;

#if defined(_WIN32)
    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "powershell.exe",
        { "-NoProfile", "-NonInteractive", "-Command", "Write-Output shader-process-ready" },
        options);
#else
    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "/bin/sh",
        { "-c", "printf shader-process-ready" },
        options);
#endif

    EXPECT_EQ(result.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::Succeeded)
        << "exit=" << result.exitCode << "\noutput=" << result.output << "\ndiagnostics=" << result.diagnostics
        << "\ncommand=" << result.commandLine;
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("shader-process-ready"), std::string::npos);
}

TEST(ShaderCompilerTests, ShaderCompilerProcessTimeoutReturnsDiagnostics)
{
    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 100u;

#if defined(_WIN32)
    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "powershell.exe",
        { "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 3" },
        options);
#else
    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "/bin/sh",
        { "-c", "sleep 3" },
        options);
#endif

    EXPECT_EQ(result.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::TimedOut);
    EXPECT_NE(result.diagnostics.find("timed out"), std::string::npos);
}

TEST(ShaderCompilerTests, ShaderCompilerProcessTimeoutTerminatesChildProcessTree)
{
    const auto markerPath = std::filesystem::temp_directory_path() / "NullusShaderCompilerTests" / "shader-child-survived.txt";
    std::filesystem::create_directories(markerPath.parent_path());
    std::filesystem::remove(markerPath);

    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 150u;

#if defined(_WIN32)
    const std::string childCommand =
        "Start-Process powershell.exe -ArgumentList '-NoProfile','-NonInteractive','-Command','Start-Sleep -Milliseconds 700; Set-Content -LiteralPath \"" +
        markerPath.string() +
        "\" -Value child-survived'; Start-Sleep -Seconds 5";

    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "powershell.exe",
        { "-NoProfile", "-NonInteractive", "-Command", childCommand },
        options);
#else
    const std::string childCommand =
        "(sleep 1; printf child-survived > \"$1\") & sleep 5";

    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "/bin/sh",
        { "-c", childCommand, "shader-child-timeout-test", markerPath.string() },
        options);
#endif

    EXPECT_EQ(result.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::TimedOut)
        << result.diagnostics << "\n" << result.output;

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(std::filesystem::exists(markerPath));
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
    EXPECT_NE(source.find("CloseHandle(outputReadPipe);"), std::string::npos);
    EXPECT_NE(source.find("if (readPipe != nullptr)"), std::string::npos);
    EXPECT_NE(source.find("CloseHandle(readPipe);"), std::string::npos);
}

TEST(ShaderCompilerTests, ShaderCacheDatabasePersistsCompiledArtifactsAndFailureDiagnostics)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_cache_db_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Library" / "ShaderCache" / "ShaderCache.tsv";

    auto vertexInput = MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain");
    vertexInput.assetPath = ":Shaders/Deferred.hlsl";
    vertexInput.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;

    NLS::Render::ShaderCompiler::ShaderCompilationOutput vertexOutput;
    vertexOutput.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
    vertexOutput.cacheKey = "cache-key-vs";
    vertexOutput.artifactPath = "Library/ShaderCache/cache-key-vs.dxil";
    vertexOutput.bytecode = {1u, 2u, 3u};
    vertexOutput.dependencyPaths = {":Shaders/Common.hlsl"};

    auto pixelInput = MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Pixel, "PSMain");
    pixelInput.assetPath = ":Shaders/Deferred.hlsl";
    pixelInput.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;

    NLS::Render::ShaderCompiler::ShaderCompilationOutput pixelOutput;
    pixelOutput.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Failed;
    pixelOutput.cacheKey = "cache-key-ps";
    pixelOutput.diagnostics = "missing texture binding";

    NLS::Render::ShaderCompiler::ShaderCacheDatabase cache;
    cache.Upsert(vertexInput, vertexOutput, "sha256:shader-source", "dxc-1.9");
    cache.Upsert(pixelInput, pixelOutput, "sha256:shader-source", "dxc-1.9");
    ASSERT_TRUE(cache.Save(databasePath));

    NLS::Render::ShaderCompiler::ShaderCacheDatabase loaded;
    ASSERT_TRUE(loaded.Load(databasePath));

    const auto* vertex = loaded.Find(
        "cache-key-vs",
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    ASSERT_NE(vertex, nullptr);
    EXPECT_EQ(vertex->assetPath, ":Shaders/Deferred.hlsl");
    EXPECT_EQ(vertex->entryPoint, "VSMain");
    EXPECT_EQ(vertex->artifactPath, "Library/ShaderCache/cache-key-vs.dxil");
    EXPECT_EQ(vertex->status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded);
    EXPECT_EQ(vertex->dependencyCount, 1u);

    const auto* failed = loaded.Find(
        "cache-key-ps",
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Failed);
    EXPECT_EQ(failed->diagnostics, "missing texture binding");
    EXPECT_TRUE(failed->artifactPath.empty());
    EXPECT_EQ(loaded.GetStats().failedRecords, 1u);
    EXPECT_EQ(loaded.GetStats().succeededRecords, 1u);

    loaded.RemoveByAssetPath(":Shaders/Deferred.hlsl");
    EXPECT_EQ(loaded.GetStats().totalRecords, 0u);

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerPersistsConfiguredShaderCacheDatabase)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_cache_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Library" / "ShaderCache" / "ShaderCache.tsv";

    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<PersistentShaderCompilerBackend>());
    compiler.SetCacheDatabasePath(databasePath.string());

    auto input = MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain");
    input.assetPath = ":Shaders/Deferred.hlsl";
    input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;

    const auto output = compiler.Compile(input);
    ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded);

    NLS::Render::ShaderCompiler::ShaderCacheDatabase loaded;
    ASSERT_TRUE(loaded.Load(databasePath));
    const auto* record = loaded.Find(
        output.cacheKey,
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->assetPath, ":Shaders/Deferred.hlsl");
    EXPECT_EQ(record->artifactPath, "Library/ShaderCache/persistent-VSMain.dxil");
    EXPECT_EQ(record->toolchainIdentity, "PersistentShaderCompilerBackend");
    EXPECT_EQ(record->dependencyCount, 2u);

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerConfiguredCacheDatabasePassesArtifactDirectoryToBackend)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_artifact_option_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv";
    const auto expectedArtifactDirectory = databasePath.parent_path();

    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ArtifactDirectoryEchoBackend>());
    compiler.SetCacheDatabasePath(databasePath.string());

    const auto output = compiler.Compile(
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"));

    ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded);
    EXPECT_EQ(
        NormalizeComparablePath(std::filesystem::path(output.diagnostics)),
        NormalizeComparablePath(expectedArtifactDirectory));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerBareCacheDatabasePathUsesCurrentDirectoryForArtifacts)
{
    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ArtifactDirectoryEchoBackend>());
    compiler.SetCacheDatabasePath("ShaderCache.tsv");

    const auto output = compiler.Compile(
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"));

    ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded);
    EXPECT_EQ(std::filesystem::path(output.diagnostics), std::filesystem::path("."));
}

TEST(ShaderCompilerTests, ShaderCompilerWithoutCacheDatabaseLeavesBackendArtifactDirectoryUnset)
{
    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ArtifactDirectoryEchoBackend>());

    const auto output = compiler.Compile(
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"));

    ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded);
    EXPECT_TRUE(output.diagnostics.empty());
}

TEST(ShaderCompilerTests, ShaderCompilerCompileBatchPassesConfiguredArtifactDirectoryToBackend)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_batch_artifact_option_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv";
    const auto expectedArtifactDirectory = databasePath.parent_path();

    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ArtifactDirectoryEchoBackend>());
    compiler.SetCacheDatabasePath(databasePath.string());

    const std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput> inputs = {
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"),
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Pixel, "PSMain")
    };

    const auto outputs = compiler.CompileBatch(inputs);

    ASSERT_EQ(outputs.size(), inputs.size());
    for (const auto& output : outputs)
    {
        ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded);
        EXPECT_EQ(
            NormalizeComparablePath(std::filesystem::path(output.diagnostics)),
            NormalizeComparablePath(expectedArtifactDirectory));
    }

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerReflectPassesConfiguredArtifactDirectoryToBackend)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_reflect_artifact_option_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv";
    const auto expectedArtifactDirectory = databasePath.parent_path();

    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ArtifactDirectoryEchoBackend>());
    compiler.SetCacheDatabasePath(databasePath.string());

    const auto reflection = compiler.Reflect(
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"));

    ASSERT_EQ(reflection.constantBuffers.size(), 1u);
    EXPECT_EQ(
        NormalizeComparablePath(std::filesystem::path(reflection.constantBuffers[0].name)),
        NormalizeComparablePath(expectedArtifactDirectory));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerReflectWithCompiledOutputPassesConfiguredArtifactDirectoryToBackend)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_reflect_output_artifact_option_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv";
    const auto expectedArtifactDirectory = databasePath.parent_path();

    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ArtifactDirectoryEchoBackend>());
    compiler.SetCacheDatabasePath(databasePath.string());

    NLS::Render::ShaderCompiler::ShaderCompilationOutput compiledOutput;
    compiledOutput.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
    compiledOutput.artifactPath = (expectedArtifactDirectory / "VSMain.dxil").string();

    const auto reflection = compiler.Reflect(
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"),
        compiledOutput);

    ASSERT_EQ(reflection.constantBuffers.size(), 1u);
    EXPECT_EQ(
        NormalizeComparablePath(std::filesystem::path(reflection.constantBuffers[0].name)),
        NormalizeComparablePath(expectedArtifactDirectory));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerReflectBatchPassesConfiguredArtifactDirectoryToBackend)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_reflect_batch_artifact_option_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv";
    const auto expectedArtifactDirectory = databasePath.parent_path();

    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<ArtifactDirectoryEchoBackend>());
    compiler.SetCacheDatabasePath(databasePath.string());

    std::vector<NLS::Render::ShaderCompiler::ShaderReflectionInput> inputs;
    for (const auto& input : {
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"),
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Pixel, "PSMain") })
    {
        NLS::Render::ShaderCompiler::ShaderCompilationOutput output;
        output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
        output.artifactPath = (expectedArtifactDirectory / (input.options.entryPoint + ".dxil")).string();
        inputs.push_back({ input, output });
    }

    const auto reflections = compiler.ReflectBatch(inputs);

    ASSERT_EQ(reflections.size(), inputs.size());
    for (const auto& reflection : reflections)
    {
        ASSERT_EQ(reflection.constantBuffers.size(), 1u);
        EXPECT_EQ(
            NormalizeComparablePath(std::filesystem::path(reflection.constantBuffers[0].name)),
            NormalizeComparablePath(expectedArtifactDirectory));
    }

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerConfiguredCacheDatabasePlacesDxilArtifactInProjectLibrary)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_artifact_library_" + NLS::Guid::New().ToString());
    const auto shaderPath = root / "App" / "Assets" / "Engine" / "Shaders" / "LibraryCacheTest.hlsl";
    const auto databasePath = root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv";
    const auto expectedArtifactDirectory = databasePath.parent_path();
    WriteTinyVertexShaderFile(shaderPath);

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    compiler.SetCacheDatabasePath(databasePath.string());

    auto input = MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain");
    input.assetPath = shaderPath.string();
    input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    input.options.targetProfile = "vs_6_0";

    const auto output = compiler.Compile(input);
    if (output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
        IsDxcUnavailableDiagnostic(output.diagnostics))
    {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "DXC is unavailable for configured shader artifact path coverage.";
    }

    ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
        << output.diagnostics;
    ASSERT_FALSE(output.artifactPath.empty());
    EXPECT_EQ(
        NormalizeComparablePath(std::filesystem::path(output.artifactPath).parent_path()),
        NormalizeComparablePath(expectedArtifactDirectory));
    EXPECT_TRUE(std::filesystem::exists(output.artifactPath));
    EXPECT_TRUE(std::filesystem::exists(databasePath));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerConfiguredCacheDatabasePlacesSpirvArtifactInProjectLibrary)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_spirv_artifact_library_" + NLS::Guid::New().ToString());
    const auto shaderPath = root / "App" / "Assets" / "Engine" / "Shaders" / "LibraryCacheSpirvTest.hlsl";
    const auto databasePath = root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv";
    const auto expectedArtifactDirectory = databasePath.parent_path();
    WriteTinyVertexShaderFile(shaderPath);

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    compiler.SetCacheDatabasePath(databasePath.string());

    auto input = MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain");
    input.assetPath = shaderPath.string();
    input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV;
    input.options.targetProfile = "vs_6_0";

    const auto output = compiler.Compile(input);
    if (output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
        IsDxcUnavailableDiagnostic(output.diagnostics))
    {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "DXC is unavailable for configured SPIR-V artifact path coverage.";
    }

    ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
        << output.diagnostics;
    ASSERT_FALSE(output.artifactPath.empty());
    EXPECT_EQ(
        NormalizeComparablePath(std::filesystem::path(output.artifactPath).parent_path()),
        NormalizeComparablePath(expectedArtifactDirectory));
    EXPECT_EQ(std::filesystem::path(output.artifactPath).extension(), ".spv");
    EXPECT_TRUE(std::filesystem::exists(output.artifactPath));
    EXPECT_TRUE(std::filesystem::exists(databasePath));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerConfiguredArtifactDirectoryConflictReturnsFailureDiagnostic)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_artifact_dir_conflict_" + NLS::Guid::New().ToString());
    const auto shaderPath = root / "App" / "Assets" / "Engine" / "Shaders" / "LibraryCacheConflictTest.hlsl";
    const auto artifactDirectory = root / "Project" / "Library" / "ShaderCache";
    const auto databasePath = artifactDirectory / "ShaderCache.tsv";
#if defined(_WIN32)
    const auto fakeDxcPath = root / "Tools" / "FakeDXC" / "dxc.exe";
#else
    const auto fakeDxcPath = root / "Tools" / "FakeDXC" / "dxc";
#endif
    WriteTinyVertexShaderFile(shaderPath);
    std::filesystem::create_directories(fakeDxcPath.parent_path());
    {
        std::ofstream fakeDxc(fakeDxcPath, std::ios::binary);
        fakeDxc << "fake dxc";
    }
#if !defined(_WIN32)
    std::error_code permissionError;
    std::filesystem::permissions(
        fakeDxcPath,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add,
        permissionError);
    ASSERT_FALSE(permissionError) << permissionError.message();
#endif
    std::filesystem::create_directories(artifactDirectory.parent_path());
    {
        std::ofstream conflict(artifactDirectory, std::ios::binary);
        conflict << "not a directory";
    }
    const ScopedEnvironmentVariable scopedDxcPath("DXC_PATH", fakeDxcPath.string());

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    compiler.SetCacheDatabasePath(databasePath.string());

    auto input = MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain");
    input.assetPath = shaderPath.string();
    input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    input.options.targetProfile = "vs_6_0";

    NLS::Render::ShaderCompiler::ShaderCompilationOutput output;
    EXPECT_NO_THROW(output = compiler.Compile(input));

    EXPECT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Failed);
    EXPECT_NE(output.diagnostics.find("Failed to create shader artifact directory"), std::string::npos)
        << output.diagnostics;
    EXPECT_TRUE(output.artifactPath.empty());

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerCacheDatabaseKeepsConcurrentStageRecords)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_compiler_cache_concurrent_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Library" / "ShaderCache" / "ShaderCache.tsv";

    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<PersistentShaderCompilerBackend>());
    compiler.SetCacheDatabasePath(databasePath.string());

    std::vector<NLS::Render::ShaderCompiler::ShaderCompilationInput> inputs = {
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain"),
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Pixel, "PSMain"),
        MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Compute, "CSMain")
    };
    for (auto& input : inputs)
    {
        input.assetPath = ":Shaders/Deferred.hlsl";
        input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    }

    const auto outputs = compiler.CompileBatch(inputs);
    ASSERT_EQ(outputs.size(), inputs.size());

    NLS::Render::ShaderCompiler::ShaderCacheDatabase loaded;
    ASSERT_TRUE(loaded.Load(databasePath));
    EXPECT_EQ(loaded.GetStats().succeededRecords, 3u);
    EXPECT_NE(loaded.Find(
        "persistent-VSMain",
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL), nullptr);
    EXPECT_NE(loaded.Find(
        "persistent-PSMain",
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL), nullptr);
    EXPECT_NE(loaded.Find(
        "persistent-CSMain",
        NLS::Render::ShaderCompiler::ShaderStage::Compute,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL), nullptr);

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderLoaderCacheDatabasePathPrefersConfiguredProjectLibrary)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_loader_cache_root_" + NLS::Guid::New().ToString());
    const auto projectAssets = (root / "Project" / "Assets").string() + std::string(1, std::filesystem::path::preferred_separator);
    const auto engineShader = root / "App" / "Assets" / "Engine" / "Shaders" / "Standard.hlsl";

    const auto databasePath = NLS::Render::Resources::Loaders::ShaderLoader::GetCacheDatabasePath(
        engineShader.string(),
        projectAssets);

    EXPECT_EQ(
        NormalizeComparablePath(databasePath),
        NormalizeComparablePath(root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv"));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderLoaderCacheDatabasePathDoesNotInferAppLibraryForDirectEngineShader)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_loader_app_cache_root_" + NLS::Guid::New().ToString());
    const auto engineShader = root / "App" / "Assets" / "Engine" / "Shaders" / "Standard.hlsl";

    const auto databasePath = NLS::Render::Resources::Loaders::ShaderLoader::GetCacheDatabasePath(
        engineShader.string());

    EXPECT_TRUE(databasePath.empty());

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderLoaderCacheDatabasePathDoesNotInferAppLibraryCaseInsensitively)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_loader_app_cache_case_root_" + NLS::Guid::New().ToString());
    const auto engineShader = root / "app" / "Assets" / "Engine" / "Shaders" / "Standard.hlsl";

    const auto databasePath = NLS::Render::Resources::Loaders::ShaderLoader::GetCacheDatabasePath(
        engineShader.string());

    EXPECT_TRUE(databasePath.empty());

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderLoaderCacheDatabasePathKeepsDirectNonAppAssetFallback)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_loader_non_app_cache_root_" + NLS::Guid::New().ToString());
    const auto projectShader = root / "StandaloneProject" / "Assets" / "Shaders" / "Standard.hlsl";

    const auto databasePath = NLS::Render::Resources::Loaders::ShaderLoader::GetCacheDatabasePath(
        projectShader.string());

    EXPECT_EQ(
        NormalizeComparablePath(databasePath),
        NormalizeComparablePath(root / "StandaloneProject" / "Library" / "ShaderCache" / "ShaderCache.tsv"));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderLoaderCacheDatabasePathKeepsProjectNamedAppFallback)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_loader_project_named_app_cache_root_" + NLS::Guid::New().ToString());
    const auto projectShader = root / "App" / "Assets" / "Shaders" / "Standard.hlsl";

    const auto databasePath = NLS::Render::Resources::Loaders::ShaderLoader::GetCacheDatabasePath(
        projectShader.string());

    EXPECT_EQ(
        NormalizeComparablePath(databasePath),
        NormalizeComparablePath(root / "App" / "Library" / "ShaderCache" / "ShaderCache.tsv"));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderLoaderDefaultProjectAssetsRootRoutesDirectEngineShaderToProjectLibrary)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_loader_default_project_root_" + NLS::Guid::New().ToString());
    const auto projectAssets = (root / "Project" / "Assets").string() + std::string(1, std::filesystem::path::preferred_separator);
    const auto engineShader = root / "App" / "Assets" / "Engine" / "Shaders" / "DebugPrimitive.hlsl";
    const ScopedShaderLoaderProjectAssetsRoot scopedRoot(projectAssets);

    const auto databasePath = NLS::Render::Resources::Loaders::ShaderLoader::GetCacheDatabasePath(
        engineShader.string());

    EXPECT_EQ(
        NormalizeComparablePath(databasePath),
        NormalizeComparablePath(root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv"));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderManagerProvideAssetPathsConfiguresShaderLoaderDefaultProjectLibrary)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_manager_default_project_root_" + NLS::Guid::New().ToString());
    const auto projectAssets = (root / "Project" / "Assets").string() + std::string(1, std::filesystem::path::preferred_separator);
    const auto engineAssets = (root / "App" / "Assets" / "Engine").string() + std::string(1, std::filesystem::path::preferred_separator);
    const auto engineShader = root / "App" / "Assets" / "Engine" / "Shaders" / "DebugPrimitive.hlsl";
    const ScopedShaderManagerAssetPaths scopedPaths(projectAssets, engineAssets);

    const auto databasePath = NLS::Render::Resources::Loaders::ShaderLoader::GetCacheDatabasePath(
        engineShader.string());

    EXPECT_EQ(
        NormalizeComparablePath(databasePath),
        NormalizeComparablePath(root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv"));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderLoaderCreateRoutesConfiguredEngineShaderCacheToProjectLibrary)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_loader_create_no_app_library_" + NLS::Guid::New().ToString());
    const auto projectAssets = (root / "Project" / "Assets").string() + std::string(1, std::filesystem::path::preferred_separator);
    const auto engineShader = root / "App" / "Assets" / "Engine" / "Shaders" / "DebugPrimitive.hlsl";
    std::filesystem::create_directories(engineShader.parent_path());
    std::filesystem::create_directories(root / "Project" / "Assets");
    std::ofstream shaderFile(engineShader, std::ios::binary);
    shaderFile
        << "struct VSOutput { float4 position : SV_Position; };\n"
        << "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
        << "    VSOutput output;\n"
        << "    output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
        << "    return output;\n"
        << "}\n"
        << "float4 PSMain(VSOutput input) : SV_Target0 {\n"
        << "    return float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
        << "}\n";
    shaderFile.close();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        engineShader.string(),
        projectAssets);

    ASSERT_NE(shader, nullptr);
    EXPECT_TRUE(std::filesystem::exists(root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv"));
    EXPECT_FALSE(std::filesystem::exists(root / "App" / "Library" / "ShaderCache" / "ShaderCache.tsv"));
    EXPECT_FALSE(std::filesystem::exists(root / "App" / "Library"));

    NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader);
    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderLoaderCreateWithoutActiveDriverCompilesCrossBackendArtifacts)
{
    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_loader_no_driver_artifacts_" + NLS::Guid::New().ToString());
    const auto projectAssets = (root / "Project" / "Assets").string() + std::string(1, std::filesystem::path::preferred_separator);
    const auto shaderPath = root / "Project" / "Assets" / "Shaders" / "NoDriverPortable.hlsl";
    std::filesystem::create_directories(shaderPath.parent_path());
    std::ofstream shaderFile(shaderPath, std::ios::binary);
    shaderFile
        << "struct VSOutput { float4 position : SV_Position; };\n"
        << "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
        << "    VSOutput output;\n"
        << "    output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
        << "    return output;\n"
        << "}\n"
        << "float4 PSMain(VSOutput input) : SV_Target0 {\n"
        << "    return float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
        << "}\n";
    shaderFile.close();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        shaderPath.string(),
        projectAssets);
    ASSERT_NE(shader, nullptr);

    const auto* dxilVertex = shader->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    if (dxilVertex == nullptr)
    {
        NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader);
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "DXC is unavailable for no-driver shader loader artifact coverage.";
    }

    EXPECT_NE(shader->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV), nullptr);
    EXPECT_NE(shader->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL), nullptr);
    EXPECT_NE(shader->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV), nullptr);
    EXPECT_NE(shader->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL), nullptr);

    NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader);
    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderManagerConfiguredCachePathDoesNotCreateAppLibraryDatabase)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_manager_no_app_library_" + NLS::Guid::New().ToString());
    const auto projectAssets = (root / "Project" / "Assets").string() + std::string(1, std::filesystem::path::preferred_separator);
    const auto engineAssets = (root / "App" / "Assets" / "Engine").string() + std::string(1, std::filesystem::path::preferred_separator);
    const auto engineShader = root / "App" / "Assets" / "Engine" / "Shaders" / "DebugPrimitive.hlsl";
    const ScopedShaderManagerAssetPaths scopedPaths(projectAssets, engineAssets);

    NLS::Render::ShaderCompiler::ShaderCompiler compiler(
        std::make_unique<PersistentShaderCompilerBackend>());
    compiler.SetCacheDatabasePath(
        NLS::Render::Resources::Loaders::ShaderLoader::GetCacheDatabasePath(engineShader.string()));

    auto input = MakeShaderInput(NLS::Render::ShaderCompiler::ShaderStage::Vertex, "VSMain");
    input.assetPath = engineShader.string();
    input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    const auto output = compiler.Compile(input);

    ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded);
    EXPECT_TRUE(std::filesystem::exists(root / "Project" / "Library" / "ShaderCache" / "ShaderCache.tsv"));
    EXPECT_FALSE(std::filesystem::exists(root / "App" / "Library" / "ShaderCache" / "ShaderCache.tsv"));
    EXPECT_FALSE(std::filesystem::exists(root / "App" / "Library"));

    std::filesystem::remove_all(root);
}

TEST(ShaderCompilerTests, ShaderCompilerProcessCancellationReturnsDiagnostics)
{
    std::atomic_bool cancelled{ true };
    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 2000u;
    options.cancellationFlag = &cancelled;

#if defined(_WIN32)
    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "cmd.exe",
        { "/C", "echo should-not-run" },
        options);
#else
    const auto result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        "/bin/sh",
        { "-c", "printf should-not-run" },
        options);
#endif

    EXPECT_EQ(result.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::Cancelled);
    EXPECT_NE(result.diagnostics.find("cancelled"), std::string::npos);
    EXPECT_TRUE(result.output.empty());
}
