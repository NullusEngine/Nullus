#include "Rendering/ShaderLab/ShaderLabAsset.h"
#include "Rendering/ShaderLab/ShaderLabReloadBarrier.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

namespace NLS::Render::ShaderLab
{
    class ShaderLabRuntimeSnapshot
    {
    public:
        NLS::Guid guid = NLS::Guid::Empty();
        uint64_t generation = 1;
        std::string shaderName;
        std::vector<ShaderLabPropertyDesc> properties;
        std::vector<ShaderLabSubShaderDesc> importSubShaders;
        std::vector<std::vector<ShaderLabPassRuntime>> passes;
        std::string fallbackShader;
    };

    namespace
    {
        std::optional<uint32_t> FindPassIndexInSnapshot(
            const ShaderLabRuntimeSnapshot& snapshot,
            const uint32_t subShaderIndex,
            const std::string_view lightMode)
        {
            if (subShaderIndex >= snapshot.passes.size())
                return std::nullopt;

            const auto& passes = snapshot.passes[subShaderIndex];
            const auto found = std::find_if(
                passes.begin(),
                passes.end(),
                [lightMode](const ShaderLabPassRuntime& pass)
                {
                    const auto tag = pass.tags.values.find("LightMode");
                    return tag != pass.tags.values.end() && tag->second == lightMode;
                });
            if (found == passes.end())
                return std::nullopt;
            return static_cast<uint32_t>(std::distance(passes.begin(), found));
        }

        ShaderLabPassRuntime ToRuntimePass(
            ShaderLabPassDesc pass,
            const uint32_t subShaderIndex,
            const uint32_t passIndex)
        {
            ShaderLabPassRuntime runtime;
            runtime.name = std::move(pass.name);
            runtime.subShaderIndex = subShaderIndex;
            runtime.passIndex = passIndex;
            runtime.tags = std::move(pass.tags);
            runtime.state = std::move(pass.state);
            runtime.hlslSource = std::move(pass.hlslSource);
            runtime.hlslLocation = std::move(pass.hlslLocation);
            runtime.vertexEntry = std::move(pass.vertexEntry);
            runtime.fragmentEntry = std::move(pass.fragmentEntry);
            runtime.computeEntry = std::move(pass.computeEntry);
            runtime.shaderFeatures = std::move(pass.shaderFeatures);
            runtime.multiCompiles = std::move(pass.multiCompiles);
            return runtime;
        }
    }

    ShaderLabPassView::ShaderLabPassView(
        std::shared_ptr<const void> lifetime,
        std::shared_ptr<const ShaderLabPassRuntime> pass,
        const uint64_t generation,
        const uint32_t subShaderIndex,
        const uint32_t passIndex)
        : m_lifetime(std::move(lifetime))
        , m_pass(std::move(pass))
        , m_generation(generation)
        , m_subShaderIndex(subShaderIndex)
        , m_passIndex(passIndex)
    {
    }

    ShaderLabPassView::operator bool() const
    {
        return m_pass != nullptr;
    }

    std::shared_ptr<const ShaderLabPassRuntime> ShaderLabPassView::GetPass() const
    {
        return m_pass;
    }

    uint64_t ShaderLabPassView::GetGeneration() const
    {
        return m_generation;
    }

    ShaderLabAsset::ShaderLabAsset(NLS::Guid guid, ShaderLabAssetDesc desc)
        : m_snapshot(BuildSnapshot(std::move(guid), 1, std::move(desc)))
    {
    }

    NLS::Guid ShaderLabAsset::GetGuid() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_snapshot->guid;
    }

    std::string ShaderLabAsset::GetName() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_snapshot->shaderName;
    }

    uint64_t ShaderLabAsset::GetGeneration() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_snapshot->generation;
    }

    std::vector<ShaderLabPropertyDesc> ShaderLabAsset::GetProperties() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_snapshot->properties;
    }

    std::vector<ShaderLabPropertyDesc> ShaderLabAsset::CopyPropertiesForMaterialReload() const
    {
        const std::shared_lock lock(m_mutex);
        return m_snapshot != nullptr ? m_snapshot->properties : std::vector<ShaderLabPropertyDesc>{};
    }

    NLS::Guid ShaderLabAsset::GetGuidUnderBarrier() const
    {
        const std::shared_lock lock(m_mutex);
        return m_snapshot != nullptr ? m_snapshot->guid : NLS::Guid::Empty();
    }

    uint64_t ShaderLabAsset::GetGenerationUnderBarrier() const
    {
        const std::shared_lock lock(m_mutex);
        return m_snapshot != nullptr ? m_snapshot->generation : 0;
    }

    ShaderLabPassView ShaderLabAsset::FindPass(
        const uint32_t subShaderIndex,
        const std::string_view lightMode) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        auto snapshot = m_snapshot;
        const auto passIndex = FindPassIndexInSnapshot(*snapshot, subShaderIndex, lightMode);
        if (!passIndex.has_value())
            return {};
        const auto& pass = snapshot->passes[subShaderIndex][*passIndex];
        return {
            snapshot,
            std::shared_ptr<const ShaderLabPassRuntime>(snapshot, &pass),
            snapshot->generation,
            subShaderIndex,
            *passIndex
        };
    }

    ShaderLabPassHandle ShaderLabAsset::GetPassHandle(
        const uint32_t subShaderIndex,
        const std::string_view lightMode) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        if (subShaderIndex >= m_snapshot->passes.size())
            return {};

        const auto& passes = m_snapshot->passes[subShaderIndex];
        for (const auto& pass : passes)
        {
            const auto tag = pass.tags.values.find("LightMode");
            if (tag != pass.tags.values.end() && tag->second == lightMode)
                return { m_snapshot->generation, subShaderIndex, pass.passIndex };
        }
        return {};
    }

    ShaderLabPassView ShaderLabAsset::ResolvePass(const ShaderLabPassHandle& handle) const
    {
        if (!handle.IsValid())
            return {};

        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        if (handle.generation != m_snapshot->generation ||
            handle.subShaderIndex >= m_snapshot->passes.size())
        {
            return {};
        }

        auto snapshot = m_snapshot;
        const auto& passes = m_snapshot->passes[handle.subShaderIndex];
        if (handle.passIndex >= passes.size())
            return {};
        return {
            snapshot,
            std::shared_ptr<const ShaderLabPassRuntime>(snapshot, &passes[handle.passIndex]),
            snapshot->generation,
            handle.subShaderIndex,
            handle.passIndex
        };
    }

    size_t ShaderLabAsset::GetRetainedSnapshotCountForTests() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_snapshot != nullptr ? 1u : 0u;
    }

    bool ShaderLabAsset::TryReplaceWith(ShaderLabAssetDesc desc, std::vector<ShaderLabDiagnostic> diagnostics)
    {
        const std::unique_lock barrier(GetShaderLabReloadBarrier());
        return ReplaceSnapshot(std::move(desc), std::move(diagnostics));
    }

    bool ShaderLabAsset::ReplaceSnapshot(ShaderLabAssetDesc desc, std::vector<ShaderLabDiagnostic> diagnostics)
    {
        std::unique_lock lock(m_mutex);
        const uint64_t nextGeneration = m_snapshot != nullptr ? m_snapshot->generation + 1 : 1;
        auto replacement = BuildSnapshot(m_snapshot->guid, nextGeneration, std::move(desc));
        m_snapshot = std::move(replacement);
        m_lastDiagnostics = std::move(diagnostics);
        return true;
    }

    void ShaderLabAsset::SetLastDiagnostics(std::vector<ShaderLabDiagnostic> diagnostics)
    {
        const std::unique_lock lock(m_mutex);
        m_lastDiagnostics = std::move(diagnostics);
    }

    std::vector<ShaderLabDiagnostic> ShaderLabAsset::GetLastDiagnostics() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_lastDiagnostics;
    }

    std::shared_ptr<ShaderLabRuntimeSnapshot> ShaderLabAsset::BuildSnapshot(
        NLS::Guid guid,
        const uint64_t generation,
        ShaderLabAssetDesc desc)
    {
        auto snapshot = std::make_shared<ShaderLabRuntimeSnapshot>();
        snapshot->guid = std::move(guid);
        snapshot->generation = generation;
        snapshot->shaderName = std::move(desc.shaderName);
        snapshot->properties = std::move(desc.properties);
        snapshot->fallbackShader = std::move(desc.fallbackShader);
        snapshot->importSubShaders = desc.subShaders;
        snapshot->passes.resize(desc.subShaders.size());

        for (uint32_t subShaderIndex = 0; subShaderIndex < desc.subShaders.size(); ++subShaderIndex)
        {
            auto& runtimePasses = snapshot->passes[subShaderIndex];
            auto& subShader = desc.subShaders[subShaderIndex];
            runtimePasses.reserve(subShader.passes.size());
            for (uint32_t passIndex = 0; passIndex < subShader.passes.size(); ++passIndex)
                runtimePasses.push_back(ToRuntimePass(std::move(subShader.passes[passIndex]), subShaderIndex, passIndex));
        }

        return snapshot;
    }
}
