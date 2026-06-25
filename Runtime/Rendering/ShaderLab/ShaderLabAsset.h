#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Guid.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"

namespace NLS::Render::ShaderLab
{
    struct NLS_RENDER_API ShaderLabPassRuntime
    {
        std::string name;
        uint32_t subShaderIndex = 0;
        uint32_t passIndex = 0;
        ShaderLabTagSet tags;
        ShaderLabPassState state;
        std::string hlslSource;
        ShaderLabSourceLocation hlslLocation;
        std::string vertexEntry;
        std::string fragmentEntry;
        std::string computeEntry;
        std::vector<ShaderLabKeywordPragma> shaderFeatures;
        std::vector<ShaderLabKeywordPragma> multiCompiles;
    };

    class ShaderLabRuntimeSnapshot;

    struct NLS_RENDER_API ShaderLabPassHandle
    {
        uint64_t generation = 0;
        uint32_t subShaderIndex = 0;
        uint32_t passIndex = 0;

        [[nodiscard]] bool IsValid() const { return generation != 0; }
    };

    struct NLS_RENDER_API ShaderLabPassView
    {
        ShaderLabPassView() = default;
        ShaderLabPassView(
            std::shared_ptr<const void> lifetime,
            std::shared_ptr<const ShaderLabPassRuntime> pass,
            uint64_t generation,
            uint32_t subShaderIndex,
            uint32_t passIndex);

        [[nodiscard]] explicit operator bool() const;
        [[nodiscard]] std::shared_ptr<const ShaderLabPassRuntime> GetPass() const;
        [[nodiscard]] uint64_t GetGeneration() const;
        [[nodiscard]] uint32_t GetSubShaderIndex() const { return m_subShaderIndex; }
        [[nodiscard]] uint32_t GetPassIndex() const { return m_passIndex; }

    private:
        std::shared_ptr<const void> m_lifetime;
        std::shared_ptr<const ShaderLabPassRuntime> m_pass;
        uint64_t m_generation = 0;
        uint32_t m_subShaderIndex = 0;
        uint32_t m_passIndex = 0;
    };

    class NLS_RENDER_API ShaderLabAsset
    {
    public:
        ShaderLabAsset(NLS::Guid guid, ShaderLabAssetDesc desc);

        [[nodiscard]] NLS::Guid GetGuid() const;
        [[nodiscard]] std::string GetName() const;
        [[nodiscard]] uint64_t GetGeneration() const;
        [[nodiscard]] std::vector<ShaderLabPropertyDesc> GetProperties() const;
        [[nodiscard]] ShaderLabPassView FindPass(uint32_t subShaderIndex, std::string_view lightMode) const;
        [[nodiscard]] ShaderLabPassHandle GetPassHandle(uint32_t subShaderIndex, std::string_view lightMode) const;
        [[nodiscard]] ShaderLabPassView ResolvePass(const ShaderLabPassHandle& handle) const;
        [[nodiscard]] size_t GetRetainedSnapshotCountForTests() const;

        [[nodiscard]] std::vector<ShaderLabDiagnostic> GetLastDiagnostics() const;

    private:
        friend class ShaderLabMaterial;
        friend class ShaderLabHotReloadService;

        static std::shared_ptr<ShaderLabRuntimeSnapshot> BuildSnapshot(
            NLS::Guid guid,
            uint64_t generation,
            ShaderLabAssetDesc desc);
        [[nodiscard]] std::vector<ShaderLabPropertyDesc> CopyPropertiesForMaterialReload() const;
        [[nodiscard]] NLS::Guid GetGuidUnderBarrier() const;
        [[nodiscard]] uint64_t GetGenerationUnderBarrier() const;
        void SetLastDiagnostics(std::vector<ShaderLabDiagnostic> diagnostics);
        [[nodiscard]] bool TryReplaceWith(ShaderLabAssetDesc desc, std::vector<ShaderLabDiagnostic> diagnostics);
        [[nodiscard]] bool ReplaceSnapshot(
            ShaderLabAssetDesc desc,
            std::vector<ShaderLabDiagnostic> diagnostics);

        mutable std::shared_mutex m_mutex;
        std::shared_ptr<const ShaderLabRuntimeSnapshot> m_snapshot;
        std::vector<ShaderLabDiagnostic> m_lastDiagnostics;
    };
}
