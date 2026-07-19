#include "Rendering/Resources/Loaders/MaterialLoader.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Debug/Logger.h>

#include "Assets/ArtifactLoadTelemetry.h"
#include "Profiling/PerformanceStageStats.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Math/Matrix4.h"
#include "Rendering/Resources/TextureCube.h"
#include "Rendering/Resources/MaterialSerialization.h"

namespace
{
    using UniformType = NLS::Render::Resources::UniformType;
    using Material = NLS::Render::Resources::Material;
    using MaterialSurfaceMode = NLS::Render::Resources::MaterialSurfaceMode;
    using UniformInfo = NLS::Render::Resources::UniformInfo;
    using Texture2D = NLS::Render::Resources::Texture2D;

    std::vector<uint8_t> ReadAllBytes(const std::string& path)
    {
        NLS::Core::Assets::ArtifactLoadTelemetryRecord telemetry;
        telemetry.stage = NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead;
        telemetry.path = path;
        NLS::Core::Assets::RecordArtifactLoadTelemetry(telemetry);

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return {};

        return {
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
    }

    std::optional<std::string> TryReadNativeMaterialArtifactPayload(const std::vector<uint8_t>& bytes)
    {
        const auto container = NLS::Core::Assets::ReadNativeArtifactContainerView(
            bytes,
            NLS::Core::Assets::ArtifactType::Material,
            1u);
        if (!container.has_value() || container->payloadSize == 0u)
            return std::nullopt;

        return std::string(
            reinterpret_cast<const char*>(container->payloadData),
            container->payloadSize);
    }

    std::string TryMakePortableContentArtifactFilePath(const std::string& path)
    {
        return NLS::Core::Assets::TryMakePortableContentArtifactPath(path);
    }

    bool IsAuthorizedMaterialArtifactPath(const std::string& path)
    {
        const auto portableArtifactPath = TryMakePortableContentArtifactFilePath(path);
        return !portableArtifactPath.empty() &&
            NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portableArtifactPath);
    }

    std::string ReadMaterialPayloadText(
        const std::string& path,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize});

        const auto bytes = ReadAllBytes(path);
        if (bytes.empty())
            return {};

        if (!options.allowSourceAssetNativeContainer && !IsAuthorizedMaterialArtifactPath(path))
            return {};

        if (auto artifactPayload = TryReadNativeMaterialArtifactPayload(bytes);
            artifactPayload.has_value())
            return *artifactPayload;

        return {};
    }

    std::string Trim(std::string value)
    {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return {};

        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    bool StartsWith(const std::string_view value, const std::string_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0u, prefix.size()) == prefix;
    }

    std::vector<std::string> SplitWhitespace(const std::string& value)
    {
        std::vector<std::string> tokens;
        std::istringstream stream(value);
        std::string token;
        while (stream >> token)
            tokens.push_back(std::move(token));
        return tokens;
    }

    std::string EscapeXml(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());

        for (const char c : value)
        {
            switch (c)
            {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            default: result += c; break;
            }
        }

        return result;
    }

    std::string UnescapeXml(std::string value)
    {
        const struct Replacement
        {
            std::string_view from;
            std::string_view to;
        } replacements[] =
        {
            { "&quot;", "\"" },
            { "&gt;", ">" },
            { "&lt;", "<" },
            { "&amp;", "&" }
        };

        for (const auto& replacement : replacements)
        {
            size_t position = 0;
            while ((position = value.find(replacement.from.data(), position)) != std::string::npos)
            {
                value.replace(position, replacement.from.size(), replacement.to.data());
                position += replacement.to.size();
            }
        }

        return value;
    }

    std::string GetTagValue(const std::string& xml, const std::string& tag)
    {
        const std::string openTag = "<" + tag + ">";
        const std::string closeTag = "</" + tag + ">";

        const auto open = xml.find(openTag);
        if (open == std::string::npos)
            return {};

        const auto valueStart = open + openTag.size();
        const auto close = xml.find(closeTag, valueStart);
        if (close == std::string::npos)
            return {};

        return UnescapeXml(Trim(xml.substr(valueStart, close - valueStart)));
    }

    std::string GetLineValue(const std::string& payload, const std::string& key)
    {
        const auto marker = key + "=";
        std::istringstream stream(payload);
        std::string line;
        while (std::getline(stream, line))
        {
            line = Trim(line);
            if (line.rfind(marker, 0u) == 0u)
                return NLS::Render::Resources::UnescapeMaterialField(line.substr(marker.size()));
        }
        return {};
    }

    std::vector<std::string> GetBlocks(const std::string& xml, const std::string& blockName)
    {
        std::vector<std::string> blocks;
        const std::string openTag = "<" + blockName;
        const std::string closeTag = "/>";

        size_t searchFrom = 0;
        while (true)
        {
            const auto open = xml.find(openTag, searchFrom);
            if (open == std::string::npos)
                break;

            const auto close = xml.find(closeTag, open);
            if (close == std::string::npos)
                break;

            blocks.emplace_back(xml.substr(open, close - open + closeTag.size()));
            searchFrom = close + closeTag.size();
        }

        return blocks;
    }

    std::string GetAttributeValue(const std::string& block, const std::string& attribute)
    {
        const std::string marker = attribute + "=\"";
        const auto begin = block.find(marker);
        if (begin == std::string::npos)
            return {};

        const auto valueStart = begin + marker.size();
        const auto valueEnd = block.find('"', valueStart);
        if (valueEnd == std::string::npos)
            return {};

        return UnescapeXml(block.substr(valueStart, valueEnd - valueStart));
    }

    bool ParseBool(const std::string& value, bool fallback = false)
    {
        if (value == "true" || value == "1")
            return true;
        if (value == "false" || value == "0")
            return false;
        return fallback;
    }

    bool TryParseInt(const std::string& value, int& output)
    {
        const auto trimmed = Trim(value);
        const auto* begin = trimmed.data();
        const auto* end = begin + trimmed.size();
        const auto result = std::from_chars(begin, end, output);
        return result.ec == std::errc{} && result.ptr == end;
    }

    bool TryParseFloat(const std::string& value, float& output)
    {
        const auto trimmed = Trim(value);
        const auto* begin = trimmed.data();
        const auto* end = begin + trimmed.size();
#if defined(__APPLE__) && defined(_LIBCPP_VERSION)
        errno = 0;
        char* parseEnd = nullptr;
        const float parsed = std::strtof(begin, &parseEnd);
        if (errno != 0 || parseEnd != end)
            return false;
        output = parsed;
        return true;
#else
        const auto result = std::from_chars(begin, end, output);
        return result.ec == std::errc{} && result.ptr == end;
#endif
    }

    std::string ToLower(std::string value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    bool IsForbiddenMaterialShaderSourceReference(const std::string& shaderPath)
    {
        const auto extension = ToLower(std::filesystem::path(shaderPath).extension().generic_string());
        return extension != ".shader";
    }

    bool IsShaderLabSourceReference(const std::string& shaderPath)
    {
        return ToLower(std::filesystem::path(shaderPath).extension().generic_string()) == ".shader";
    }

    bool ValidateShaderLabMaterialShaderReference(const std::string& shaderPath)
    {
        if (shaderPath.empty() || shaderPath == "?")
        {
            NLS_LOG_ERROR(
                "Failed to load ShaderLab material: missing authoritative .shader source reference.");
            return false;
        }

        if (IsForbiddenMaterialShaderSourceReference(shaderPath))
        {
            NLS_LOG_ERROR(
                "Failed to load ShaderLab material: shader reference '" + shaderPath +
                "' is not an authoritative ShaderLab .shader source asset.");
            return false;
        }

        return true;
    }

    std::string NormalizePortablePath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return std::filesystem::path(path).lexically_normal().generic_string();
    }

    std::optional<std::filesystem::path> TryResolveProjectRootFromMaterialArtifactPath(const std::string& materialPath)
    {
        if (materialPath.empty())
            return std::nullopt;

        std::filesystem::path normalized = std::filesystem::path(materialPath).lexically_normal();
        std::vector<std::filesystem::path> parts;
        for (const auto& part : normalized)
            parts.push_back(part);

        for (size_t index = 0u; index + 1u < parts.size(); ++index)
        {
            if (parts[index].generic_string() != "Library" ||
                parts[index + 1u].generic_string() != "Artifacts")
            {
                continue;
            }

            std::filesystem::path root;
            for (size_t rootIndex = 0u; rootIndex < index; ++rootIndex)
                root /= parts[rootIndex];
            if (!root.empty())
                return root;
            return std::filesystem::current_path();
        }

        return std::nullopt;
    }

    std::optional<std::filesystem::path> TryResolveArtifactDatabasePathFromMaterialArtifactPath(
        const std::string& materialPath)
    {
        if (materialPath.empty())
            return std::nullopt;

        std::filesystem::path normalized = std::filesystem::path(materialPath).lexically_normal();
        std::vector<std::filesystem::path> parts;
        for (const auto& part : normalized)
            parts.push_back(part);

        for (size_t index = 0u; index + 1u < parts.size(); ++index)
        {
            const auto first = parts[index].generic_string();
            const auto second = parts[index + 1u].generic_string();
            if ((first != "Library" && first != "Data") || second != "Artifacts")
                continue;

            std::filesystem::path root;
            for (size_t rootIndex = 0u; rootIndex < index; ++rootIndex)
                root /= parts[rootIndex];
            if (root.empty())
                root = std::filesystem::current_path();
            return root / parts[index] / "ArtifactDB";
        }

        return std::nullopt;
    }

    std::optional<std::filesystem::file_time_type> TryGetArtifactDatabaseWriteTime(
        const std::filesystem::path& databasePath)
    {
        std::error_code error;
        const auto dataFile = databasePath / "data.mdb";
        if (std::filesystem::exists(dataFile, error))
            return std::filesystem::last_write_time(dataFile, error);
        if (!error)
            return std::nullopt;
        return std::nullopt;
    }

    std::shared_ptr<NLS::Core::Assets::ArtifactDatabase> LoadCachedArtifactDatabase(
        const std::filesystem::path& databasePath)
    {
        struct CacheEntry
        {
            std::shared_ptr<NLS::Core::Assets::ArtifactDatabase> database;
            std::optional<std::filesystem::file_time_type> writeTime;
        };

        static std::mutex cacheMutex;
        static std::unordered_map<std::string, CacheEntry> cache;

        const auto normalizedPath = databasePath.lexically_normal();
        const auto key = normalizedPath.generic_string();
        const auto writeTime = TryGetArtifactDatabaseWriteTime(normalizedPath);

        std::lock_guard lock(cacheMutex);
        if (const auto found = cache.find(key);
            found != cache.end() &&
            found->second.database != nullptr &&
            found->second.writeTime == writeTime)
        {
            return found->second.database;
        }

        auto database = std::make_shared<NLS::Core::Assets::ArtifactDatabase>();
        if (!database->Load(normalizedPath))
            return nullptr;

        cache[key] = { database, writeTime };
        return database;
    }

    std::filesystem::path ResolveArtifactPayloadRootFromDatabasePath(const std::filesystem::path& databasePath)
    {
        const auto databaseRoot = databasePath.lexically_normal().parent_path();
        if (databaseRoot.filename().generic_string() == "Library")
            return databaseRoot.parent_path();
        return databaseRoot;
    }

    std::filesystem::path ResolveSourceAssetRootFromDatabasePath(const std::filesystem::path& databasePath)
    {
        const auto databaseRoot = databasePath.lexically_normal().parent_path();
        if (databaseRoot.filename().generic_string() == "Library")
            return databaseRoot.parent_path();
        if (databaseRoot.filename().generic_string() == "Data" && !databaseRoot.parent_path().empty())
            return databaseRoot.parent_path();
        return databaseRoot;
    }

    std::optional<std::filesystem::path> ResolveArtifactDatabasePath(
        const std::string& materialPath,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        if (!options.artifactDatabasePath.empty())
            return options.artifactDatabasePath.lexically_normal();
        return TryResolveArtifactDatabasePathFromMaterialArtifactPath(materialPath);
    }

    NLS::Core::Assets::AssetId ResolveShaderLabSourceAssetId(
        const std::filesystem::path& projectRoot,
        const std::string& shaderSourcePath)
    {
        const auto sourcePath = std::filesystem::path(shaderSourcePath).lexically_normal();
        const auto absoluteSourcePath = sourcePath.is_absolute()
            ? sourcePath
            : (projectRoot / sourcePath).lexically_normal();

        if (const auto meta = NLS::Core::Assets::AssetMeta::Load(
                NLS::Core::Assets::GetAssetMetaPath(absoluteSourcePath));
            meta.has_value() && meta->id.IsValid())
        {
            return meta->id;
        }

        return NLS::Core::Assets::AssetId(
            NLS::Guid::NewDeterministic(NormalizePortablePath(shaderSourcePath)));
    }

    std::vector<std::string> ResolveCachedShaderLabPassArtifactPaths(
        const NLS::Core::Assets::ArtifactDatabase& database,
        const std::filesystem::path& artifactDatabasePath,
        const std::string& shaderSourcePath,
        const std::string& targetPlatform)
    {
        struct CacheEntry
        {
            std::optional<std::filesystem::file_time_type> writeTime;
            std::vector<std::string> artifactPaths;
        };

        static std::mutex cacheMutex;
        static std::unordered_map<std::string, CacheEntry> cache;

        const auto normalizedDatabasePath = artifactDatabasePath.lexically_normal();
        const auto normalizedSource = NormalizePortablePath(shaderSourcePath);
        const auto writeTime = TryGetArtifactDatabaseWriteTime(normalizedDatabasePath);
        const auto key = normalizedDatabasePath.generic_string() + "|" + normalizedSource + "|" + targetPlatform;
        {
            std::lock_guard lock(cacheMutex);
            if (const auto found = cache.find(key);
                found != cache.end() && found->second.writeTime == writeTime)
            {
                return found->second.artifactPaths;
            }
        }

        const auto sourceRoot = ResolveSourceAssetRootFromDatabasePath(normalizedDatabasePath);
        const auto artifactRoot = ResolveArtifactPayloadRootFromDatabasePath(normalizedDatabasePath);
        const auto sourceAssetId = ResolveShaderLabSourceAssetId(sourceRoot, normalizedSource);
        std::vector<std::string> artifactPaths;
        auto collectRecord = [&](const NLS::Core::Assets::ArtifactDatabaseRecord* record)
        {
            if (record == nullptr ||
                record->status != NLS::Core::Assets::ArtifactRecordStatus::UpToDate ||
                (!targetPlatform.empty() && record->targetPlatform != targetPlatform) ||
                record->artifactType != NLS::Core::Assets::ArtifactType::Shader ||
                record->artifactPath.empty())
            {
                return;
            }

            artifactPaths.push_back((artifactRoot / record->artifactPath).lexically_normal().string());
        };

        size_t matchedBySourceIdCount = 0u;
        for (const auto* record : database.FindBySource(sourceAssetId))
        {
            collectRecord(record);
            if (record != nullptr)
                ++matchedBySourceIdCount;
        }
        if (matchedBySourceIdCount == 0u)
        {
            database.VisitRecords([&](const NLS::Core::Assets::ArtifactDatabaseRecord& record)
            {
                if (NormalizePortablePath(record.sourcePath) == normalizedSource)
                    collectRecord(&record);
            });
        }

        {
            std::lock_guard lock(cacheMutex);
            cache[key] = { writeTime, artifactPaths };
        }
        return artifactPaths;
    }

    void RegisterShaderLabPassArtifactsFromArtifactDatabase(
        Material& material,
        const std::string& shaderSourcePath,
        const std::string& materialPath,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        if (shaderSourcePath.empty() ||
            !options.loadMissingShaders ||
            !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
        {
            return;
        }

        const auto artifactDatabasePath = ResolveArtifactDatabasePath(materialPath, options);
        if (!artifactDatabasePath.has_value())
            return;

        const auto database = LoadCachedArtifactDatabase(*artifactDatabasePath);
        if (database == nullptr)
            return;

        const auto passResolveTelemetryBegin = std::chrono::steady_clock::now();
        const auto passArtifactPaths = ResolveCachedShaderLabPassArtifactPaths(
            *database,
            *artifactDatabasePath,
            shaderSourcePath,
            options.targetPlatform);
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialShaderPassResolve,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - passResolveTelemetryBegin),
            passArtifactPaths.size(),
            shaderSourcePath
        });

        auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
        for (const auto& artifactResourcePath : passArtifactPaths)
        {
            const auto passLoadTelemetryBegin = std::chrono::steady_clock::now();
            auto* passShader = shaderManager.GetResource(artifactResourcePath, true);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                options.shaderPassLoadTelemetryStage,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - passLoadTelemetryBegin),
                1u,
                artifactResourcePath
            });
            if (passShader != nullptr)
                material.RegisterShaderLabPassShader(passShader);
        }
    }

    size_t PreloadShaderLabPassArtifactsFromArtifactDatabase(
        const std::string& materialPath,
        const std::string& shaderSourcePath,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        if (shaderSourcePath.empty() ||
            !options.loadMissingShaders ||
            !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
        {
            return 0u;
        }

        const auto artifactDatabasePath = ResolveArtifactDatabasePath(materialPath, options);
        if (!artifactDatabasePath.has_value())
            return 0u;

        const auto database = LoadCachedArtifactDatabase(*artifactDatabasePath);
        if (database == nullptr)
            return 0u;

        const auto passResolveTelemetryBegin = std::chrono::steady_clock::now();
        const auto passArtifactPaths = ResolveCachedShaderLabPassArtifactPaths(
            *database,
            *artifactDatabasePath,
            shaderSourcePath,
            options.targetPlatform);
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialShaderPassResolve,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - passResolveTelemetryBegin),
            passArtifactPaths.size(),
            shaderSourcePath
        });

        size_t loadedCount = 0u;
        auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
        for (const auto& artifactResourcePath : passArtifactPaths)
        {
            const auto passLoadTelemetryBegin = std::chrono::steady_clock::now();
            auto* passShader = shaderManager.GetResource(artifactResourcePath, true);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                options.shaderPassLoadTelemetryStage,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - passLoadTelemetryBegin),
                1u,
                artifactResourcePath
            });
            if (passShader != nullptr)
                ++loadedCount;
        }
        return loadedCount;
    }

    std::vector<std::string> ResolveShaderLabPassArtifactPathsFromArtifactDatabase(
        const std::string& materialPath,
        const std::string& shaderSourcePath,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        if (shaderSourcePath.empty() || !options.loadMissingShaders)
            return {};

        const auto artifactDatabasePath = ResolveArtifactDatabasePath(materialPath, options);
        if (!artifactDatabasePath.has_value())
            return {};

        const auto database = LoadCachedArtifactDatabase(*artifactDatabasePath);
        if (database == nullptr)
            return {};

        const auto passResolveTelemetryBegin = std::chrono::steady_clock::now();
        auto passArtifactPaths = ResolveCachedShaderLabPassArtifactPaths(
            *database,
            *artifactDatabasePath,
            shaderSourcePath,
            options.targetPlatform);
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialShaderPassResolve,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - passResolveTelemetryBegin),
            passArtifactPaths.size(),
            shaderSourcePath
        });
        return passArtifactPaths;
    }

    std::string ResolveMaterialShaderReferenceForSave(Material& material)
    {
        if (material.HasExplicitShaderLabSourcePath())
            return material.GetShaderLabSourcePath();

        if (!material.GetShaderReferencePath().empty())
            return material.GetShaderReferencePath();

        const auto* shader = material.GetShader();
        if (shader != nullptr && !shader->path.empty() && !IsForbiddenMaterialShaderSourceReference(shader->path))
            return shader->path;

        return "?";
    }

    std::optional<NLS::Render::RHI::TextureWrap> ParseTextureWrap(const std::string& value)
    {
        const auto lowered = ToLower(value);
        if (lowered == "clamp" || lowered == "clamptoedge")
            return NLS::Render::RHI::TextureWrap::ClampToEdge;
        if (lowered == "mirror" || lowered == "mirroredrepeat" || lowered == "mirrorrepeat")
            return NLS::Render::RHI::TextureWrap::MirrorRepeat;
        if (lowered == "clamptoborder")
            return NLS::Render::RHI::TextureWrap::ClampToBorder;
        if (lowered == "repeat")
            return NLS::Render::RHI::TextureWrap::Repeat;
        return std::nullopt;
    }

    std::optional<NLS::Render::RHI::TextureFilter> ParseTextureFilter(const std::string& value)
    {
        const auto lowered = ToLower(value);
        if (lowered == "nearest" || lowered == "point")
            return NLS::Render::RHI::TextureFilter::Nearest;
        if (lowered == "linear" || lowered == "bilinear")
            return NLS::Render::RHI::TextureFilter::Linear;
        return std::nullopt;
    }

    std::string SamplerBindingNameForTextureSlot(const std::string& slotName)
    {
        if (slotName.empty())
            return {};
        if (slotName.front() == '_')
            return "sampler" + slotName;
        return "sampler_" + slotName;
    }

    void ApplyTextureSlotSamplerOverrides(Material& material, const std::string& xml)
    {
        for (const auto& block : GetBlocks(xml, "textureSlot"))
        {
            const auto samplerName = SamplerBindingNameForTextureSlot(GetAttributeValue(block, "name"));
            if (samplerName.empty())
                continue;

            NLS::Render::RHI::SamplerDesc sampler;
            if (const auto wrap = ParseTextureWrap(GetAttributeValue(block, "wrapS")); wrap.has_value())
                sampler.wrapU = *wrap;
            if (const auto wrap = ParseTextureWrap(GetAttributeValue(block, "wrapT")); wrap.has_value())
                sampler.wrapV = *wrap;
            sampler.wrapW = sampler.wrapV;
            if (const auto filter = ParseTextureFilter(GetAttributeValue(block, "minFilter")); filter.has_value())
                sampler.minFilter = *filter;
            if (const auto filter = ParseTextureFilter(GetAttributeValue(block, "magFilter")); filter.has_value())
                sampler.magFilter = *filter;

            material.SetSamplerOverride(samplerName, sampler);
        }
    }

    void ApplyShaderLabTextureSlotSamplerOverrides(Material& material, const std::string& payload)
    {
        std::istringstream stream(payload);
        std::string line;
        while (std::getline(stream, line))
        {
            line = Trim(line);
            if (!StartsWith(line, "textureSlot "))
                continue;

            const auto tokens = SplitWhitespace(line);
            if (tokens.size() < 2u)
                continue;

            const auto samplerName = SamplerBindingNameForTextureSlot(tokens[1]);
            if (samplerName.empty())
                continue;

            const auto attributes = NLS::Render::Resources::ParseMaterialKeyValueTail(
                line.substr(std::string("textureSlot ").size() + tokens[1].size()));
            NLS::Render::RHI::SamplerDesc sampler;
            if (const auto found = attributes.find("wrapS"); found != attributes.end())
            {
                if (const auto wrap = ParseTextureWrap(found->second); wrap.has_value())
                    sampler.wrapU = *wrap;
            }
            if (const auto found = attributes.find("wrapT"); found != attributes.end())
            {
                if (const auto wrap = ParseTextureWrap(found->second); wrap.has_value())
                    sampler.wrapV = *wrap;
            }
            sampler.wrapW = sampler.wrapV;
            if (const auto found = attributes.find("minFilter"); found != attributes.end())
            {
                if (const auto filter = ParseTextureFilter(found->second); filter.has_value())
                    sampler.minFilter = *filter;
            }
            if (const auto found = attributes.find("magFilter"); found != attributes.end())
            {
                if (const auto filter = ParseTextureFilter(found->second); filter.has_value())
                    sampler.magFilter = *filter;
            }

            material.SetSamplerOverride(samplerName, sampler);
        }
    }

    void ApplyShaderLabSamplerOverrides(Material& material, const std::string& payload)
    {
        std::istringstream stream(payload);
        std::string line;
        while (std::getline(stream, line))
        {
            line = Trim(line);
            if (!StartsWith(line, "samplerOverride "))
                continue;

            const auto tokens = SplitWhitespace(line);
            if (tokens.size() < 2u || tokens[1].empty())
                continue;

            const auto attributes = NLS::Render::Resources::ParseMaterialKeyValueTail(
                line.substr(std::string("samplerOverride ").size() + tokens[1].size()));
            NLS::Render::RHI::SamplerDesc sampler;
            if (const auto found = attributes.find("wrapS"); found != attributes.end())
            {
                if (const auto wrap = ParseTextureWrap(found->second); wrap.has_value())
                    sampler.wrapU = *wrap;
            }
            if (const auto found = attributes.find("wrapT"); found != attributes.end())
            {
                if (const auto wrap = ParseTextureWrap(found->second); wrap.has_value())
                    sampler.wrapV = *wrap;
            }
            sampler.wrapW = sampler.wrapV;
            if (const auto found = attributes.find("minFilter"); found != attributes.end())
            {
                if (const auto filter = ParseTextureFilter(found->second); filter.has_value())
                    sampler.minFilter = *filter;
            }
            if (const auto found = attributes.find("magFilter"); found != attributes.end())
            {
                if (const auto filter = ParseTextureFilter(found->second); filter.has_value())
                    sampler.magFilter = *filter;
            }

            material.SetSamplerOverride(tokens[1], sampler);
        }
    }

    std::string UniformTypeToString(UniformType type)
    {
        switch (type)
        {
        case UniformType::UNIFORM_BOOL: return "bool";
        case UniformType::UNIFORM_INT: return "int";
        case UniformType::UNIFORM_FLOAT: return "float";
        case UniformType::UNIFORM_FLOAT_VEC2: return "vec2";
        case UniformType::UNIFORM_FLOAT_VEC3: return "vec3";
        case UniformType::UNIFORM_FLOAT_VEC4: return "vec4";
        case UniformType::UNIFORM_FLOAT_MAT4: return "mat4";
        case UniformType::UNIFORM_SAMPLER_2D: return "sampler2D";
        case UniformType::UNIFORM_SAMPLER_CUBE: return "samplerCube";
        default: return {};
        }
    }

    std::string SerializeUniformValue(UniformType type, const std::any& value)
    {
        using NLS::Maths::Matrix4;
        using NLS::Maths::Vector2;
        using NLS::Maths::Vector3;
        using NLS::Maths::Vector4;

        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(6);

        switch (type)
        {
        case UniformType::UNIFORM_BOOL:
            return std::any_cast<bool>(value) ? "true" : "false";
        case UniformType::UNIFORM_INT:
            stream << std::any_cast<int>(value);
            return stream.str();
        case UniformType::UNIFORM_FLOAT:
            stream << std::any_cast<float>(value);
            return stream.str();
        case UniformType::UNIFORM_FLOAT_VEC2:
        {
            const auto vec = std::any_cast<Vector2>(value);
            stream << vec.x << ' ' << vec.y;
            return stream.str();
        }
        case UniformType::UNIFORM_FLOAT_VEC3:
        {
            const auto vec = std::any_cast<Vector3>(value);
            stream << vec.x << ' ' << vec.y << ' ' << vec.z;
            return stream.str();
        }
        case UniformType::UNIFORM_FLOAT_VEC4:
        {
            const auto vec = std::any_cast<Vector4>(value);
            stream << vec.x << ' ' << vec.y << ' ' << vec.z << ' ' << vec.w;
            return stream.str();
        }
        case UniformType::UNIFORM_FLOAT_MAT4:
        {
            const auto mat = std::any_cast<Matrix4>(value);
            for (size_t index = 0; index < 16; ++index)
            {
                if (index != 0)
                    stream << ' ';
                stream << mat.data[index];
            }
            return stream.str();
        }
        case UniformType::UNIFORM_SAMPLER_2D:
        {
            const auto texture = std::any_cast<Texture2D*>(value);
            if (texture)
                return texture->path;
            break;
        }
        case UniformType::UNIFORM_SAMPLER_CUBE:
            return "";
        default:
            return {};
        }
        return {};
    }

    template <size_t N>
    bool ParseFloatArray(const std::string& value, std::array<float, N>& output)
    {
        std::istringstream stream(value);
        for (size_t index = 0; index < N; ++index)
        {
            if (!(stream >> output[index]))
                return false;
        }

        return true;
    }

    template <typename T>
    void SetSerializedUniformValue(Material& material, const std::string& name, const T& value)
    {
        if (material.HasExplicitShaderLabSourcePath() && !material.HasShader())
        {
            material.SetRawParameter(name, std::any(value));
            return;
        }

        material.Set<T>(name, value);
    }

    void ApplyUniformValue(
        Material& material,
        const UniformInfo& uniform,
        const std::string& value,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        using NLS::Maths::Matrix4;
        using NLS::Maths::Vector2;
        using NLS::Maths::Vector3;
        using NLS::Maths::Vector4;

        switch (uniform.type)
        {
        case UniformType::UNIFORM_BOOL:
            SetSerializedUniformValue<bool>(material, uniform.name, ParseBool(value));
            break;
        case UniformType::UNIFORM_INT:
        {
            int parsed = 0;
            if (TryParseInt(value, parsed))
                SetSerializedUniformValue<int>(material, uniform.name, parsed);
            else
                NLS_LOG_ERROR("Failed to load material property '" + uniform.name + "': invalid int value '" + value + "'");
            break;
        }
        case UniformType::UNIFORM_FLOAT:
        {
            float parsed = 0.0f;
            if (TryParseFloat(value, parsed))
                SetSerializedUniformValue<float>(material, uniform.name, parsed);
            else
                NLS_LOG_ERROR("Failed to load material property '" + uniform.name + "': invalid float value '" + value + "'");
            break;
        }
        case UniformType::UNIFORM_FLOAT_VEC2:
        {
            std::array<float, 2> parsed{};
            if (ParseFloatArray(value, parsed))
                SetSerializedUniformValue<Vector2>(material, uniform.name, { parsed[0], parsed[1] });
            break;
        }
        case UniformType::UNIFORM_FLOAT_VEC3:
        {
            std::array<float, 3> parsed{};
            if (ParseFloatArray(value, parsed))
                SetSerializedUniformValue<Vector3>(material, uniform.name, { parsed[0], parsed[1], parsed[2] });
            break;
        }
        case UniformType::UNIFORM_FLOAT_VEC4:
        {
            std::array<float, 4> parsed{};
            if (ParseFloatArray(value, parsed))
                SetSerializedUniformValue<Vector4>(material, uniform.name, { parsed[0], parsed[1], parsed[2], parsed[3] });
            break;
        }
        case UniformType::UNIFORM_FLOAT_MAT4:
        {
            std::array<float, 16> parsed{};
            if (ParseFloatArray(value, parsed))
            {
                Matrix4 matrix;
                for (size_t index = 0; index < 16; ++index)
                    matrix.data[index] = parsed[index];
                SetSerializedUniformValue<Matrix4>(material, uniform.name, matrix);
            }
            break;
        }
        case UniformType::UNIFORM_SAMPLER_2D:
        {
            if (!value.empty())
                material.SetTextureResourcePath(uniform.name, value);
            else
                material.ClearTextureResourcePath(uniform.name);

            auto* texture = static_cast<Texture2D*>(nullptr);
            if (!value.empty() && options.loadMissingTextures)
            {
                NLS::Base::Profiling::PerformanceStageScope scope(
                    NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                    "PrewarmMaterialTextureDependency",
                    NLS::Base::Profiling::PerformanceStageThread::Main);
                texture = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).GetArtifactResource(value, true);
            }
            if (!value.empty() && options.loadMissingTextures && !texture)
                NLS_LOG_WARNING("Material texture failed to load and will use the default white texture: " + value);
            SetSerializedUniformValue<Texture2D*>(material, uniform.name, texture);
            break;
        }
        case UniformType::UNIFORM_SAMPLER_CUBE:
            break;
        default:
            break;
        }
    }

    std::optional<UniformType> ShaderLabPropertyTypeToUniformType(const std::string& type)
    {
        const auto lowered = ToLower(type);
        if (lowered == "float" || lowered == "range")
            return UniformType::UNIFORM_FLOAT;
        if (lowered == "int")
            return UniformType::UNIFORM_INT;
        if (lowered == "vector" || lowered == "color")
            return UniformType::UNIFORM_FLOAT_VEC4;
        if (lowered == "texture2d")
            return UniformType::UNIFORM_SAMPLER_2D;
        if (lowered == "texturecube")
            return UniformType::UNIFORM_SAMPLER_CUBE;
        return std::nullopt;
    }

    std::string TextureWrapName(const NLS::Render::RHI::TextureWrap value)
    {
        using NLS::Render::RHI::TextureWrap;
        switch (value)
        {
        case TextureWrap::ClampToEdge: return "ClampToEdge";
        case TextureWrap::MirrorRepeat: return "MirrorRepeat";
        case TextureWrap::ClampToBorder: return "ClampToBorder";
        case TextureWrap::Repeat: return "Repeat";
        }
        return "Repeat";
    }

    std::string TextureFilterName(const NLS::Render::RHI::TextureFilter value)
    {
        using NLS::Render::RHI::TextureFilter;
        switch (value)
        {
        case TextureFilter::Nearest: return "Nearest";
        case TextureFilter::Linear: return "Linear";
        }
        return "Linear";
    }

    std::string UniformTypeToShaderLabPropertyType(
        const UniformType type,
        const std::string& shaderLabPropertyName)
    {
        switch (type)
        {
        case UniformType::UNIFORM_BOOL: return "Int";
        case UniformType::UNIFORM_INT: return "Int";
        case UniformType::UNIFORM_FLOAT: return "Float";
        case UniformType::UNIFORM_FLOAT_VEC2:
        case UniformType::UNIFORM_FLOAT_VEC3:
            return "Vector";
        case UniformType::UNIFORM_FLOAT_VEC4:
            return shaderLabPropertyName.find("Color") != std::string::npos
                ? "Color"
                : "Vector";
        case UniformType::UNIFORM_FLOAT_MAT4: return "Matrix";
        case UniformType::UNIFORM_SAMPLER_2D: return "Texture2D";
        case UniformType::UNIFORM_SAMPLER_CUBE: return "TextureCube";
        default: return {};
        }
    }

    std::optional<UniformType> InferSerializedMaterialPropertyType(const std::string& name, const std::any& value)
    {
        using NLS::Maths::Matrix4;
        using NLS::Maths::Vector2;
        using NLS::Maths::Vector3;
        using NLS::Maths::Vector4;

        if (value.type() == typeid(bool))
            return UniformType::UNIFORM_BOOL;
        if (value.type() == typeid(int))
            return UniformType::UNIFORM_INT;
        if (value.type() == typeid(float))
            return UniformType::UNIFORM_FLOAT;
        if (value.type() == typeid(Vector2))
            return UniformType::UNIFORM_FLOAT_VEC2;
        if (value.type() == typeid(Vector3))
            return UniformType::UNIFORM_FLOAT_VEC3;
        if (value.type() == typeid(Vector4))
            return name.find("Color") != std::string::npos
                ? UniformType::UNIFORM_FLOAT_VEC4
                : UniformType::UNIFORM_FLOAT_VEC4;
        if (value.type() == typeid(Matrix4))
            return UniformType::UNIFORM_FLOAT_MAT4;
        if (value.type() == typeid(Texture2D*))
            return UniformType::UNIFORM_SAMPLER_2D;
        return std::nullopt;
    }

    void ApplyShaderLabPropertyValue(
        Material& material,
        const std::string& propertyName,
        const std::string& propertyType,
        const std::string& value,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        const auto fallbackType = ShaderLabPropertyTypeToUniformType(propertyType);
        if (!fallbackType.has_value())
            return;

        UniformInfo syntheticUniform;
        syntheticUniform.name = propertyName;
        syntheticUniform.type = *fallbackType;

        const auto* shader = material.GetShader();
        const auto reflectedUniform = shader != nullptr
            ? shader->GetUniformInfo(propertyName)
            : std::optional<UniformInfo> {};

        ApplyUniformValue(
            material,
            reflectedUniform.has_value() ? *reflectedUniform : syntheticUniform,
            value,
            options);
    }

    bool ApplyShaderLabSerializedMaterial(
        Material& material,
        const std::string& payload,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        if (GetLineValue(payload, "shaderLabMaterialVersion") != "1")
            return false;

        const auto shaderPath = GetLineValue(payload, "shader");
        if (!ValidateShaderLabMaterialShaderReference(shaderPath))
            return false;

        const auto surfaceMode = GetLineValue(payload, "surfaceMode");
        auto parsedSurfaceMode = std::optional<NLS::Render::Resources::MaterialSurfaceMode> {};
        if (!surfaceMode.empty())
        {
            parsedSurfaceMode = NLS::Render::Resources::ParseMaterialSurfaceMode(surfaceMode);
            if (!parsedSurfaceMode.has_value())
            {
                NLS_LOG_ERROR("Failed to load ShaderLab material: invalid surfaceMode '" + surfaceMode + "'");
                return false;
            }
        }

        auto* shader = static_cast<NLS::Render::Resources::Shader*>(nullptr);
        if (!shaderPath.empty())
        {
            if (IsShaderLabSourceReference(shaderPath))
            {
                material.SetShaderLabSourcePath(shaderPath);
            }
            else
            {
                shader = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager).GetResource(
                    shaderPath,
                    options.loadMissingShaders);
            }
        }

        material.ClearSamplerOverrides();
        for (const auto& keyword : material.GetShaderLabKeywordNames())
            material.DisableKeyword(keyword);
        material.SetShader(shader);
        if (shader != nullptr && !shaderPath.empty() && !IsShaderLabSourceReference(shaderPath))
            material.SetShaderReferencePath(shaderPath);
        if (!shaderPath.empty() && IsShaderLabSourceReference(shaderPath))
        {
            material.SetShaderLabSourcePath(shaderPath);
            RegisterShaderLabPassArtifactsFromArtifactDatabase(material, shaderPath, material.path.empty() ? std::string{} : material.path, options);
        }

        if (parsedSurfaceMode.has_value())
            material.SetSurfaceMode(*parsedSurfaceMode);

        if (const auto doubleSided = GetLineValue(payload, "doubleSided"); !doubleSided.empty())
        {
            const bool isDoubleSided = ParseBool(doubleSided);
            material.SetBackfaceCulling(!isDoubleSided);
            material.SetFrontfaceCulling(false);
        }
        if (const auto depthWrite = GetLineValue(payload, "depthWrite"); !depthWrite.empty())
            material.SetDepthWriting(ParseBool(depthWrite, material.HasDepthWriting()));

        std::istringstream stream(payload);
        std::string line;
        while (std::getline(stream, line))
        {
            line = Trim(line);
            if (StartsWith(line, "keyword "))
            {
                auto tokens = SplitWhitespace(line);
                if (tokens.size() >= 2u)
                    material.EnableKeyword(tokens[1]);
                continue;
            }
            if (!StartsWith(line, "property "))
                continue;

            auto tokens = SplitWhitespace(line);
            if (tokens.size() < 4u)
                continue;

            const auto propertyOffset = line.find(tokens[1], std::string("property ").size());
            if (propertyOffset == std::string::npos)
                continue;
            const auto typeOffset = line.find(tokens[2], propertyOffset + tokens[1].size());
            if (typeOffset == std::string::npos)
                continue;
            const auto valueStart = typeOffset + tokens[2].size();
            ApplyShaderLabPropertyValue(
                material,
                tokens[1],
                tokens[2],
                NLS::Render::Resources::UnescapeMaterialField(Trim(line.substr(valueStart))),
                options);
        }

        ApplyShaderLabTextureSlotSamplerOverrides(material, payload);
        ApplyShaderLabSamplerOverrides(material, payload);
        return true;
    }

    bool ApplySerializedMaterial(
        Material& material,
        const std::string& payload,
        const NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions& options)
    {
        if (GetLineValue(payload, "shaderLabMaterialVersion") != "1")
        {
            NLS_LOG_ERROR("Failed to load material: legacy XML material payloads are not supported");
            return false;
        }

        return ApplyShaderLabSerializedMaterial(material, payload, options);
    }

}

namespace NLS::Render::Resources::Loaders
{
Material* MaterialLoader::Create(const std::string& p_path)
{
    return Create(p_path, {});
}

Material* MaterialLoader::Create(const std::string& p_path, const LoadOptions& options)
{
    std::string xml;
    {
        NLS::Base::Profiling::PerformanceStageScope scope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "PrewarmMaterialArtifactRead",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        xml = ReadMaterialPayloadText(p_path, options);
    }
    if (xml.empty())
    {
        NLS_LOG_ERROR("Failed to load material: " + p_path);
        return nullptr;
    }

    NLS::Base::Profiling::PerformanceStageScope scope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "PrewarmMaterialDeserializeAndResolve",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    return CreateFromSerializedPayload(p_path, xml, options);
}

Material* MaterialLoader::CreateFromSerializedPayload(
    const std::string& p_path,
    const std::string& p_xml,
    const LoadOptions& options)
{
    if (p_xml.empty())
        return nullptr;

    auto* material = new Material();
    material->path = p_path;
    if (!ApplySerializedMaterial(*material, p_xml, options))
    {
        delete material;
        return nullptr;
    }
    material->path = p_path;
    return material;
}

std::vector<std::string> MaterialLoader::ResolveShaderLabPassArtifactPaths(
    const std::string& materialPath,
    const std::string& shaderSourcePath,
    const LoadOptions& options)
{
    return ResolveShaderLabPassArtifactPathsFromArtifactDatabase(
        materialPath,
        shaderSourcePath,
        options);
}

size_t MaterialLoader::PreloadShaderLabPassArtifacts(
    const std::string& materialPath,
    const std::string& shaderSourcePath,
    const LoadOptions& options)
{
    return PreloadShaderLabPassArtifactsFromArtifactDatabase(
        materialPath,
        shaderSourcePath,
        options);
}

std::string MaterialLoader::ReadSerializedPayload(const std::string& p_path)
{
    return ReadMaterialPayloadText(p_path, {});
}

void MaterialLoader::Reload(Material& p_material, const std::string& p_path)
{
    Reload(p_material, p_path, {});
}

void MaterialLoader::Reload(Material& p_material, const std::string& p_path, const LoadOptions& options)
{
    const auto xml = ReadMaterialPayloadText(p_path, options);
    if (xml.empty())
    {
        NLS_LOG_ERROR("Failed to reload material: " + p_path);
        return;
    }

    if (!ApplySerializedMaterial(p_material, xml, options))
        return;
    p_material.path = p_path;
}

void MaterialLoader::Save(Material& p_material, const std::string& p_path)
{
    std::ostringstream output;
    output << "shaderLabMaterialVersion=1\n";
    const auto shaderReference = ResolveMaterialShaderReferenceForSave(p_material);
    if (!ValidateShaderLabMaterialShaderReference(shaderReference))
    {
        NLS_LOG_ERROR("Failed to save material: " + p_path);
        return;
    }
    output << "shader=" << NLS::Render::Resources::EscapeMaterialField(shaderReference) << "\n";
    output << "surfaceMode=" << MaterialSurfaceModeName(p_material.GetSurfaceMode()) << "\n";
    output << "alphaMode=" << (p_material.IsBlendable() ? "Blend" : "Opaque") << "\n";
    output << "doubleSided=" << ((!p_material.HasBackfaceCulling() && !p_material.HasFrontfaceCulling()) ? "true" : "false") << "\n";
    output << "depthWrite=" << (p_material.HasDepthWriting() ? "true" : "false") << "\n";
    for (const auto& keyword : p_material.GetShaderLabKeywordNames())
        output << "keyword " << keyword << "\n";
    for (const auto& [name, sampler] : p_material.GetSamplerOverrides())
    {
        output << "samplerOverride " << name
            << " wrapS=" << TextureWrapName(sampler.wrapU)
            << " wrapT=" << TextureWrapName(sampler.wrapV)
            << " minFilter=" << TextureFilterName(sampler.minFilter)
            << " magFilter=" << TextureFilterName(sampler.magFilter)
            << "\n";
    }

    auto writeProperty = [&](const std::string& name, const std::string& type, const UniformType uniformType, const std::any& value)
    {
        auto serializedValue = SerializeUniformValue(uniformType, value);
        if (uniformType == UniformType::UNIFORM_SAMPLER_2D && serializedValue.empty())
            serializedValue = p_material.GetTextureResourcePath(name);
        if (serializedValue.empty() && uniformType != UniformType::UNIFORM_SAMPLER_2D)
            return false;

        output << "property " << name << ' ' << type << ' ' <<
            (uniformType == UniformType::UNIFORM_SAMPLER_2D ||
             uniformType == UniformType::UNIFORM_SAMPLER_CUBE
                ? NLS::Render::Resources::EscapeMaterialField(serializedValue)
                : serializedValue)
            << "\n";
        return true;
    };

    std::map<std::string, bool> savedProperties;
    if (auto* shader = p_material.GetShader())
    {
        for (const auto& [name, value] : p_material.GetUniformsData())
        {
            const auto uniformInfo = shader->GetUniformInfo(name);
            if (!uniformInfo.has_value() || !value.has_value())
                continue;

            const auto type = UniformTypeToShaderLabPropertyType(uniformInfo->type, name);
            if (type.empty())
                continue;

            if (writeProperty(name, type, uniformInfo->type, value))
                savedProperties[name] = true;
        }
    }
    else
    {
        for (const auto& [name, value] : p_material.GetUniformsData())
        {
            if (!value.has_value())
                continue;

            const auto inferredType = InferSerializedMaterialPropertyType(name, value);
            if (!inferredType.has_value())
                continue;

            const auto type = UniformTypeToShaderLabPropertyType(*inferredType, name);
            if (type.empty())
                continue;

            if (writeProperty(name, type, *inferredType, value))
                savedProperties[name] = true;
        }
    }

    for (const auto& [name, path] : p_material.GetTextureResourcePaths())
    {
        if (savedProperties.find(name) != savedProperties.end())
            continue;

        auto type = std::string("Texture2D");
        if (auto* shader = p_material.GetShader())
        {
            const auto uniformInfo = shader->GetUniformInfo(name);
            if (!uniformInfo.has_value() ||
                (uniformInfo->type != UniformType::UNIFORM_SAMPLER_2D &&
                 uniformInfo->type != UniformType::UNIFORM_SAMPLER_CUBE))
            {
                continue;
            }

            type = UniformTypeToShaderLabPropertyType(uniformInfo->type, name);
            if (type.empty())
                continue;
        }

        output << "property " << name << ' ' << type << ' ' <<
            NLS::Render::Resources::EscapeMaterialField(path) << "\n";
    }

    const auto text = output.str();
    const std::vector<uint8_t> payload(text.begin(), text.end());
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Material;
    metadata.schemaName = "material";
    metadata.schemaVersion = 1u;
    const auto bytes = NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);

    std::filesystem::create_directories(std::filesystem::path(p_path).parent_path());
    std::ofstream file(p_path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        NLS_LOG_ERROR("Failed to save material: " + p_path);
        return;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool MaterialLoader::Destroy(Material*& p_material)
{
    if (!p_material)
        return false;

    delete p_material;
    p_material = nullptr;
    return true;
}
}
