#pragma once
#include "AnimationCompilerDef.h"
#include <Animation/ImportSettings.h>
#include <filesystem>
#include <map>

namespace NLS::AnimationCompiler
{
struct GltfSourcePayload
{
    std::string json;
    std::vector<Animation::SkeletalMesh> primitives;
    std::map<std::string,std::vector<uint8_t>> embeddedImages;
    std::vector<std::string> externalFiles;
};
NLS_ANIMATION_COMPILER_API bool ReadGltfSourcePayload(const std::filesystem::path& path,
    const Animation::ImportSettings& settings,GltfSourcePayload& payload,std::string& error);
// A null result with an empty error means there is no skeletal animation data.
NLS_ANIMATION_COMPILER_API std::shared_ptr<Animation::Model> ImportGltfAnimation(
    const std::filesystem::path& path,const Animation::ImportSettings& settings,std::string& error);
NLS_ANIMATION_COMPILER_API bool BindAnimationSkeleton(Animation::Model& model,
    const Animation::Skeleton& target,std::string& error);
}
