#pragma once
#include "Animation.h"
#include <functional>
#include <map>
#include <variant>

namespace NLS::Animation
{
enum class ParameterType : uint8_t {Float, Bool, Vector, Trigger};
struct Parameter
{
    uint64_t id = 0;
    std::string name;
    ParameterType type = ParameterType::Float;
    Value initial{};
};
enum class Compare : uint8_t {True, False, Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, All, Any, Not};
struct Condition
{
    std::string parameter;
    Compare operation = Compare::True;
    float value = 0;
    std::vector<Condition> children;
    int32_t parameterIndex = -1; // Compiled, never serialized by the authoring frontend.
};
enum class NodeKind : uint8_t
{
    ReferencePose, SequencePlayer, SequenceEvaluator, BlendSpacePlayer,
    Blend, MultiBlend, LayeredBlend, Additive, StateMachine, SaveCachedPose, UseCachedPose, OutputPose
};
struct Node
{
    uint64_t id = 0;
    NodeKind kind = NodeKind::ReferencePose;
    std::string name, clip, blendSpace, mask, parameter, xParameter, yParameter;
    std::vector<uint64_t> inputs;
    std::vector<float> weights;
    float weight = .5f;
    double sampleTime = 0;
    uint64_t machine = 0;
    bool loop = true;
    std::array<float,2> position{};
};
struct State {uint64_t id=0; std::string name; uint64_t pose=0;};
enum class BlendCurve : uint8_t {Linear, SmoothStep};
struct Transition
{
    uint64_t id=0, from=0, to=0; // from == 0 means Any State.
    Condition condition;
    double duration=.2, exitTime=-1;
    int32_t priority=0;
    bool interruptible=true;
    BlendCurve curve=BlendCurve::Linear;
};
struct StateMachine
{
    uint64_t id=0, initial=0;
    std::string name;
    std::vector<State> states;
    std::vector<Transition> transitions;
};
struct GraphAsset
{
    uint32_t version=1;
    std::string id, name="Animation Graph", skeleton;
    uint64_t output=0;
    std::vector<Parameter> parameters;
    std::vector<Node> nodes;
    std::vector<StateMachine> machines;
};
struct BlendSample {std::string clip; float x=0,y=0;};
struct BlendSpaceAsset
{
    uint32_t version=1;
    std::string id,name="Blend Space",skeleton;
    uint32_t dimensions=1;
    std::vector<BlendSample> samples;
};
struct BoneMaskAsset
{
    uint32_t version=1;
    std::string id,name="Bone Mask",skeleton;
    float defaultWeight=0;
    std::map<std::string,float> weights;
};
struct ClipAsset
{
    uint32_t version=1;
    std::string id,name="Animation Clip",source,skeleton;
    double start=0,end=-1;
    bool loop=true;
    std::vector<Curve> curves;
    std::vector<Notify> notifies;
};
struct BlendSpace
{
    BlendSpaceAsset source;
    std::vector<std::shared_ptr<const Clip>> clips;
    std::vector<std::array<uint32_t,3>> triangles;
};
struct CompiledNode
{
    Node source;
    std::vector<uint32_t> inputs;
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const BlendSpace> blendSpace;
    std::vector<float> mask;
    int32_t parameter=-1,xParameter=-1,yParameter=-1,machine=-1;
};
struct AnimationProgram
{
    uint32_t version=1;
    std::string id,dependencySnapshot;
    std::shared_ptr<const Skeleton> skeleton;
    std::vector<Parameter> parameters;
    std::vector<CompiledNode> nodes;
    std::vector<StateMachine> machines;
    uint32_t output=0;
};
struct AnimationFrameResult
{
    std::string skeletonSignature;
    uint64_t revision=0,historyGeneration=0;
    double age=0;
    Pose local;
    std::vector<Maths::Matrix4> model;
    std::map<std::string,float> curves;
    std::vector<NotifyEvent> notifies;
    std::vector<float> nodeWeights;
    std::vector<uint64_t> activeStates;
};
struct BlendWeight {uint32_t sample=0;float weight=0;};
NLS_ANIMATION_API std::vector<BlendWeight> CalculateBlendWeights(const BlendSpace& blend,float x,float y);

class NLS_ANIMATION_API AnimationInstance
{
public:
    AnimationInstance();
    ~AnimationInstance();
    AnimationInstance(const AnimationInstance&)=delete;
    AnimationInstance& operator=(const AnimationInstance&)=delete;
    bool SetProgram(std::shared_ptr<const AnimationProgram> program,std::string& error);
    bool PlayClip(std::shared_ptr<const Skeleton> skeleton,std::shared_ptr<const Clip> clip,std::string& error);
    bool SetFloat(uint64_t id,float value);
    bool SetBool(uint64_t id,bool value);
    bool SetVector(uint64_t id,Value value);
    bool SetTrigger(uint64_t id);
    bool SetFloat(std::string_view name,float value);
    bool SetBool(std::string_view name,bool value);
    bool SetVector(std::string_view name,Value value);
    bool SetTrigger(std::string_view name);
    void Play();
    void Pause(bool paused=true);
    void Stop();
    void Seek(double seconds);
    void SetLooping(bool loop);
    void SetSpeed(float speed);
    bool IsPaused() const;
    double Age() const;
    bool Tick(double deltaTime,std::string& error);
    std::shared_ptr<const AnimationFrameResult> Capture() const;
    const std::shared_ptr<const AnimationProgram>& Program() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Scene-owned serial scheduler. The snapshot/evaluate/publish boundary is shared
// with editor previews and can later be scheduled across independent instances.
class NLS_ANIMATION_API AnimationSystem
{
public:
    void Register(AnimationInstance& instance);
    void Unregister(AnimationInstance& instance);
    bool Update(double deltaTime,std::string& error);
    void Finalize(const std::function<void(AnimationInstance&,const NotifyEvent&)>& sink);
private:
    std::vector<AnimationInstance*> m_instances;
    std::map<AnimationInstance*,uint64_t> m_dispatched;
};
}
