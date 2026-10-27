#pragma once
#include "AnimationCompilerDef.h"
#include <Animation/Graph.h>
#include <filesystem>
#include <map>

namespace NLS::AnimationCompiler
{
struct Diagnostic {uint64_t node=0;std::string message;};
struct Resources
{
    std::shared_ptr<const Animation::Skeleton> skeleton;
    std::map<std::string,std::shared_ptr<const Animation::Clip>> clips;
    std::map<std::string,Animation::BlendSpaceAsset> blendSpaces;
    std::map<std::string,Animation::BoneMaskAsset> masks;
};
struct CompileResult
{
    std::shared_ptr<const Animation::AnimationProgram> program;
    std::vector<Diagnostic> diagnostics;
    bool Succeeded()const{return program!=nullptr&&diagnostics.empty();}
};
NLS_ANIMATION_COMPILER_API CompileResult Compile(const Animation::GraphAsset& graph,const Resources& resources);
NLS_ANIMATION_COMPILER_API std::shared_ptr<Animation::BlendSpace> CompileBlendSpace(const Animation::BlendSpaceAsset& asset,const Resources& resources,std::string& error);
NLS_ANIMATION_COMPILER_API std::shared_ptr<Animation::Clip> CompileClip(const Animation::ClipAsset& asset,const Resources& resources,std::string& error);
NLS_ANIMATION_COMPILER_API std::string Serialize(const Animation::GraphAsset& asset);
NLS_ANIMATION_COMPILER_API std::string Serialize(const Animation::BlendSpaceAsset& asset);
NLS_ANIMATION_COMPILER_API std::string Serialize(const Animation::BoneMaskAsset& asset);
NLS_ANIMATION_COMPILER_API std::string Serialize(const Animation::ClipAsset& asset);
NLS_ANIMATION_COMPILER_API bool Deserialize(std::string_view source,Animation::GraphAsset& asset,std::string& error);
NLS_ANIMATION_COMPILER_API bool Deserialize(std::string_view source,Animation::BlendSpaceAsset& asset,std::string& error);
NLS_ANIMATION_COMPILER_API bool Deserialize(std::string_view source,Animation::BoneMaskAsset& asset,std::string& error);
NLS_ANIMATION_COMPILER_API bool Deserialize(std::string_view source,Animation::ClipAsset& asset,std::string& error);
NLS_ANIMATION_COMPILER_API std::vector<uint8_t> SerializeProgram(const Animation::AnimationProgram& program);
NLS_ANIMATION_COMPILER_API std::shared_ptr<const Animation::AnimationProgram> DeserializeProgram(std::span<const uint8_t> bytes,std::string& error);
NLS_ANIMATION_COMPILER_API Animation::GraphAsset CreateGraph(std::string name="Animation Graph");
}
