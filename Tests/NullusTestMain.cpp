#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#if defined(_MSC_VER)
#include <eh.h>
#include <windows.h>
#endif

#include "Debug/FileHandler.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "AssemblyEngine.h"
#include "Math/ExternalReflection.h"

namespace
{
#if defined(_MSC_VER)
    /**
     * Structured exception translated into a C++ exception.
     *
     * Modules that enable SEH translation can otherwise lose an access violation
     * in catch-all diagnostics. This type keeps the code and faulting address.
     * First-party C++ code uses /EHsc: access violations there remain native faults
     * for the debugger, while ordinary C++ failures unwind resource guards.
     */
    class StructuredException final : public std::runtime_error
    {
    public:
        StructuredException(const unsigned long code, void* const address)
            : std::runtime_error(BuildMessage(code, address))
            , m_code(code)
        {
        }

        [[nodiscard]] unsigned long Code() const noexcept { return m_code; }

    private:
        /**
         * Captures the call chain of the faulting thread.
         *
         * The translator runs at the fault point, before any unwind, so the stack
         * still holds the frames that led to the fault. Without this the only
         * evidence is a single address, which says where the CPU stopped but not
         * which operation reached it.
         */
        static std::string CaptureCallChain()
        {
            // Skip the capture and the exception-dispatch frames so the first
            // reported entry is inside the faulting operation.
            void* frames[64] {};
            constexpr USHORT kSkip = 4u;
            const USHORT captured =
                RtlCaptureStackBackTrace(kSkip, 64u, frames, nullptr);

            std::string chain;
            for (USHORT index = 0u; index < captured; ++index)
            {
                char entry[384] {};
                char location[320] {};
                DescribeAddress(frames[index], location, sizeof(location));
                std::snprintf(entry, sizeof(entry), "\n    #%u %s", index, location);
                chain += entry;
            }
            if (chain.empty())
                chain = "\n    <no frames captured>";
            return chain;
        }

        /** Names the faulting module and offset so the address can be symbolised. */
        static void DescribeAddress(void* const address, char* const buffer, const size_t size)
        {
            if (address == nullptr)
            {
                std::snprintf(buffer, size, "<no address>");
                return;
            }

            HMODULE module = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCSTR>(address),
                    &module) &&
                module != nullptr)
            {
                char modulePath[MAX_PATH] {};
                GetModuleFileNameA(module, modulePath, MAX_PATH);
                const char* const name = std::strrchr(modulePath, '\\');
                const auto offset = reinterpret_cast<uintptr_t>(address) -
                    reinterpret_cast<uintptr_t>(module);
                std::snprintf(
                    buffer,
                    size,
                    "%s+0x%llX",
                    name != nullptr ? name + 1 : modulePath,
                    static_cast<unsigned long long>(offset));
                return;
            }

            std::snprintf(buffer, size, "%p", address);
        }

        static std::string BuildMessage(const unsigned long code, void* const address)
        {
            char location[512] {};
            DescribeAddress(address, location, sizeof(location));
            char buffer[256] {};
            std::snprintf(
                buffer,
                sizeof(buffer),
                "structured exception 0x%08lX at %s",
                code,
                location);
            return std::string { buffer } + CaptureCallChain();
        }

        unsigned long m_code = 0u;
    };

    void TranslateStructuredException(
        const unsigned int code,
        _EXCEPTION_POINTERS* const pointers)
    {
        void* address = nullptr;
        if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
            address = pointers->ExceptionRecord->ExceptionAddress;
        throw StructuredException { code, address };
    }
#endif
}

namespace
{
    /**
     * Prints the live render-thread count after every test that leaves one behind.
     *
     * The counter is process-wide by design, so a fixture driver that survives its
     * test makes every later FlushRenderingCommands wait for a drain nobody can run.
     * The listener only reports the transition, so a clean run stays silent and a
     * leaking run names the responsible test without needing a debugger attach.
     */
    class RenderThreadLeakListener final : public ::testing::EmptyTestEventListener
    {
    public:
        void OnTestEnd(const ::testing::TestInfo& testInfo) override
        {
            const uint32_t count =
                NLS::Render::Context::RenderingThreadSystem::GetActiveRenderThreadCountForTesting();
            if (count == m_lastCount)
                return;
            m_lastCount = count;
            std::fprintf(stderr, "[render-thread-count] %u after %s.%s\n", count,
                testInfo.test_suite_name(), testInfo.name());
            std::fflush(stderr);
        }

    private:
        uint32_t m_lastCount = 0u;
    };
}

int main(int argc, char** argv)
{
    const auto logsDirectory = std::filesystem::path(NLS_BUILD_DIR) / "Logs";
    std::error_code errorCode;
    std::filesystem::create_directories(logsDirectory, errorCode);
    NLS::Debug::FileHandler::SetLogFilePath(logsDirectory.generic_string());
    NLS::Render::Resources::Loaders::ShaderLoader::SetTrustedBuiltInShaderEngineAssetsPath(
        (std::filesystem::path(NLS_ROOT_DIR) / "App" / "Assets" / "Engine").string());
    // Keep generated and external Engine/Math reflection modules reachable in
    // the focused static-library test target before object graph loading.
    NLS::Engine::AssemblyEngine::Initialize();

#if defined(_MSC_VER)
    // Applies only to this thread and to modules compiled for SEH translation.
    _set_se_translator(TranslateStructuredException);
#endif

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::UnitTest::GetInstance()->listeners().Append(new RenderThreadLeakListener());
    return RUN_ALL_TESTS();
}
