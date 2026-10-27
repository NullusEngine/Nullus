#pragma once

#include "AnimationDef.h"
#include <Math/Matrix4.h>
#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NLS::Animation
{
using BoneIndex = uint32_t;
inline constexpr BoneIndex NoBone = UINT32_MAX;
inline constexpr uint32_t AnimationFormatVersion = 1;

struct Transform
{
    Maths::Vector3 translation{};
    Maths::Quaternion rotation = Maths::Quaternion::Identity;
    Maths::Vector3 scale{1,1,1};
};
using Pose = std::vector<Transform>;
struct Bone
{
    std::string id; // Persistent source path, never the runtime array index.
    std::string name;
    BoneIndex parent = NoBone;
    Transform reference;
};
struct Skeleton
{
    std::string id;
    std::vector<Bone> bones;
    NLS_ANIMATION_API BoneIndex Find(std::string_view idOrName) const;
    NLS_ANIMATION_API bool Validate(std::string& error) const;
    NLS_ANIMATION_API Pose ReferencePose() const;
    NLS_ANIMATION_API std::string Signature() const;
};

enum class Interpolation : uint8_t {Step, Linear, CubicSpline};
using Value = std::array<float,4>;
struct Channel
{
    Interpolation interpolation = Interpolation::Linear;
    std::vector<double> times;
    std::vector<Value> values;
    std::vector<Value> inTangents, outTangents;
    NLS_ANIMATION_API bool Validate(std::string& error) const;
    NLS_ANIMATION_API Value Sample(double time, Value fallback, bool quaternion = false) const;
};
struct BoneTrack
{
    std::string bone;
    Channel translation, rotation, scale;
};
struct Curve {std::string name; Channel channel;};
struct Notify
{
    uint64_t id = 0;
    std::string name, payload;
    double start = 0, end = 0;
    float threshold = .01f;
};
struct Clip
{
    std::string id, name, skeleton;
    double duration = 0;
    std::vector<BoneTrack> tracks;
    std::vector<Curve> curves;
    std::vector<Notify> notifies;
    NLS_ANIMATION_API bool Validate(const Skeleton& skeleton, std::string& error) const;
};
struct SamplingContext
{
    std::vector<BoneIndex> trackBones;
    const Clip* clip = nullptr;
    std::string skeletonSignature;
    NLS_ANIMATION_API bool Bind(const Skeleton& skeleton, const Clip& source, std::string& error);
};
struct SkinBinding
{
    std::string id, skeleton, meshNode;
    std::vector<BoneIndex> joints;
    std::vector<Maths::Matrix4> inverseBind;
    Maths::Matrix4 meshFromSkeleton = Maths::Matrix4::Identity;
};
struct Influence
{
    std::array<uint16_t,4> joints{};
    std::array<uint16_t,4> weights{};
};
struct Vertex
{
    float position[3]{};
    float texCoords[2]{};
    float normals[3]{};
    float tangent[3]{};
    float bitangent[3]{};
    Influence influence;
};
static_assert(sizeof(Vertex) == 72);
struct SkeletalMesh
{
    std::string id, name, skin;
    uint32_t material = 0;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};
struct Model
{
    Skeleton skeleton;
    std::vector<SkinBinding> skins;
    std::vector<SkeletalMesh> meshes;
    std::vector<Clip> clips;
    std::string reader;
    std::vector<std::string> warnings;
    NLS_ANIMATION_API bool Validate(std::string& error) const;
};
struct SkinningPalette
{
    std::vector<Maths::Matrix4> position, normal;
};
enum class NotifyPhase : uint8_t {Instant, Begin, End};
struct NotifyEvent
{
    uint64_t player = 0, id = 0;
    std::string name, payload;
    NotifyPhase phase = NotifyPhase::Instant;
    int64_t cycle = 0;
    double time = 0;
};

NLS_ANIMATION_API Maths::Matrix4 ToMatrix(const Transform& transform);
NLS_ANIMATION_API bool Decompose(const Maths::Matrix4& matrix, Transform& result, std::string& error);
NLS_ANIMATION_API double WrapTime(double time, double duration);
NLS_ANIMATION_API bool Sample(const Skeleton& skeleton, const Clip& clip, double time,
    Pose& output, SamplingContext& context, std::string& error);
NLS_ANIMATION_API bool LocalToModel(const Skeleton& skeleton, std::span<const Transform> local,
    std::vector<Maths::Matrix4>& model, std::string& error);
NLS_ANIMATION_API Transform Blend(const Transform& a, const Transform& b, float weight);
NLS_ANIMATION_API bool BlendPoses(std::span<const Transform> a, std::span<const Transform> b,
    float weight, Pose& output, std::span<const float> mask = {});
NLS_ANIMATION_API bool BlendWeightedPoses(std::span<const Pose* const> poses,std::span<const float> weights,Pose& output);
NLS_ANIMATION_API bool Additive(std::span<const Transform> base, std::span<const Transform> value,
    std::span<const Transform> reference, float weight, Pose& output);
NLS_ANIMATION_API bool BuildPalette(std::span<const Maths::Matrix4> model,
    const SkinBinding& binding, SkinningPalette& output, std::string& error);
NLS_ANIMATION_API bool PackInfluences(std::span<const std::pair<uint32_t,float>> source,
    uint32_t jointCount, Influence& output, bool& truncated, std::string& error);
NLS_ANIMATION_API void CollectNotifies(const Clip& clip, double previous, double next,
    uint64_t player, float weight, bool looping, std::vector<NotifyEvent>& events);
NLS_ANIMATION_API void SkinVertices(std::span<const Vertex> source, const SkinningPalette& palette,
    std::vector<Vertex>& output);
NLS_ANIMATION_API std::vector<uint8_t> SerializeModel(const Model& model);
NLS_ANIMATION_API std::shared_ptr<Model> DeserializeModel(std::span<const uint8_t> bytes, std::string& error);
}
