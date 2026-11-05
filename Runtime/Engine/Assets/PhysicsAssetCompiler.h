#pragma once

#include "EngineDef.h"
#include "PhysicsMaterial.h"
#include "PhysicsShapeAsset.h"
#include <Assets/AssetMeta.h>
#include <Assets/ArtifactWriter.h>
#include <string_view>

namespace NLS::Engine::Assets
{
// Pure immutable inputs. The caller owns publication and revision/cancellation checks.
NLS_ENGINE_API bool CompilePhysicsAsset(Core::Assets::AssetType type,std::string_view source,
    std::string_view targetPlatform,Core::Assets::ArtifactPayload& artifact,std::string& error);
NLS_ENGINE_API bool ParsePhysicsMaterial(std::string_view source,PhysicsMaterialData& data,std::string& error);
NLS_ENGINE_API std::shared_ptr<const PhysicsShapeData> ParsePhysicsShape(std::string_view source,std::string& error);
NLS_ENGINE_API bool CapturePhysicsAssetSource(const std::filesystem::path& path,std::vector<uint8_t>& bytes,std::string& error);
NLS_ENGINE_API std::string PhysicsCookingFingerprint();
NLS_ENGINE_API bool IsPhysicsCookingTargetSupported(std::string_view target,std::string_view architecture={});
}
