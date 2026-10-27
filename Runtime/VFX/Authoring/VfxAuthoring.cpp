#include <VFX/VfxGraphSchemaSerialization.h>
#include "VfxAuthoring.h"
#include "VfxAuthoringSerialization.h"
#include "VfxLibrary.h"
#include <VFX/VfxValueSerialization.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <unordered_set>
#include <Guid.h>
#include <Json/json.hpp>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace NLS::VFX::Authoring
{
namespace
{
using Json = nlohmann::json;
constexpr uint32_t kStageControlVersion = 3;
bool HasStageControls(const System& system)
{
    for (const auto& emitter : system.emitters)
    {
        for (const auto& override : emitter.stackOverrides) if (override.bindings) return true;
        for (const auto& stack : emitter.stacks)
            if (!stack.bindings.enabled.empty() || !stack.bindings.iterations.empty() || (stack.stage == Stage::Simulation && !stack.iterations)) return true;
    }
    return false;
}
void ApplyInputMetadata(std::vector<ModuleDefinition>& modules)
{
    struct Entry
    {
        std::string_view module,input,unit,tooltip,visibleIf;
        bool hasRange = false;
        float minimum = 0, maximum = 0, step = 0;
    };
    // Curated Niagara-style metadata for inputs whose units or conditions are
    // unambiguous. Empty strings leave the corresponding metadata untouched.
    static constexpr Entry entries[] = {
        {"SpawnRate","Rate","1/s","Particles spawned per second.",{},true,0,100000,1},
        {"GravityForce","Acceleration","cm/s2","Constant acceleration applied by the force solver.",{},false},
        {"AccelerationForce","Acceleration","cm/s2","Constant acceleration applied by the force solver.",{},false},
        {"LinearForce","Acceleration","cm/s2","Constant acceleration applied by the force solver.",{},false},
        {"Drag","Drag","1/s","Linear drag applied by the force solver.",{},true,0,1000,0.01f},
        {"UpdateMeshOrientation","Angular Velocity","deg/s","Angular velocity applied to the mesh orientation each tick.",{},false},
        {"MeshRotationRate","Rate","deg/s","Constant spin rate around the local axis.",{},false},
        {"MeshRotationRate","Axis",{},"Local axis to spin around.",{},false},
        {"ApplyRotationVector","Rotational Velocity","deg/s","Angular velocity converted into a mesh orientation delta.",{},false},
        {"AddRotationalVelocity","Rotational Velocity","deg/s","Rotational velocity added to the particle.",{},false},
        {"WindForce","Wind Velocity","cm/s","Velocity the particle is blended towards.",{},false},
        {"WindForce","Wind Scale","x","Blend amount per second.",{},true,0,100,0.01f},
        {"PointRepulsionForce","Falloff Radius","cm","Radius over which the repulsion fades out.",{},false},
        {"PointForce","Force Strength","cm/s2","Acceleration magnitude at the force origin.",{},false},
        {"PointForce","Falloff Distance","cm","Distance over which the force fades out.","Use Falloff",false},
        {"PointForce","Invert Falloff",{},"Pull towards the origin instead of pushing away.","Use Falloff",false},
        {"PointAttractionForce","Attraction Strength","cm/s2","Acceleration applied towards the attractor.",{},false},
        {"LineAttractionForce","Attraction Strength","cm/s2","Acceleration applied towards the closest point on the line.",{},false},
        {"VortexForce","Vortex Strength","cm/s2","Tangential acceleration around the vortex axis.",{},false},
        {"SpringForce","Spring","1/s2","Spring stiffness towards the rest position.",{},false},
        {"SpringForce","Damping","1/s","Velocity damping applied by the spring.",{},false},
        {"VectorNoiseForce","Noise Strength","cm/s2","Noise-driven acceleration magnitude.",{},false},
        {"LimitForce","Force Limit","cm/s2","Maximum accumulated force magnitude.",{},true,0,100000,0.01f},
        {"ConstrainVectorToCone","Cone Angle","deg","Maximum angle from the cone direction.",{},true,0,180,0.1f},
        {"ConeMask","Cone Angle","deg","Cone opening angle around the cone direction.",{},true,0,180,0.1f},
        {"PendulumConstraint","Use Angle Constraint",{},"Limits the pendulum swing to the angle below.","",false},
        {"PendulumConstraint","Angle Constraint","deg","Maximum swing angle from the rest axis.","Use Angle Constraint",true,0,180,0.1f},
        {"PendulumSetup","Enable Spring Driver",{},"Enables the spring settings on the pendulum constraint.",{},false},
        {"PendulumSetup","Spring Tightness",{},"Spring correction strength used by the pendulum constraint.","Enable Spring Driver",true,0,1000,0.01f},
        {"FadeOverTime","Fade Rate","1/s","Convergence rate towards the target value.",{},true,0,1000,0.01f},
        {"InterpolateOverTime","Rate Of Change","1/s","Maximum change applied per second.",{},true,0,10000,0.01f},
        {"MeshRotationForce","Lever Radius","cm","Distance from the rotation axis used to convert torque into linear force.",{},false},
    };
    for (auto& module : modules)
        for (auto& input : module.inputs)
            for (const auto& entry : entries)
                if (module.id == entry.module && input.name == entry.input)
                {
                    auto& metadata = input.metadata;
                    if (!entry.unit.empty()) metadata.unit = entry.unit;
                    if (!entry.tooltip.empty()) metadata.tooltip = entry.tooltip;
                    if (!entry.visibleIf.empty()) metadata.visibleIf = entry.visibleIf;
                    if (entry.hasRange) { metadata.hasRange = true; metadata.minimum = entry.minimum; metadata.maximum = entry.maximum; }
                    if (entry.step > 0) metadata.step = entry.step;
                }
}
std::vector<ModuleDefinition> library{
    {"EmitterState","Emitter State","Emitter","Controls emitter duration and looping.",Stage::EmitterUpdate,
        {{"Duration",ValueType::Float,{5,0,0,0}},{"Loop",ValueType::Float,{1,0,0,0}}},{}},
    {"SpawnRate","Spawn Rate","Spawning","Spawns particles continuously.",Stage::EmitterUpdate,{{"Rate",ValueType::Float,{100,0,0,0}}},{}},
    {"SpawnPerFrame","Spawn Per Frame","Spawning","Spawns an exact number of particles every simulation sub-step.",Stage::EmitterUpdate,
        {{"Count",ValueType::Float,{1,0,0,0}}},{}},
    {"SpawnBurst","Spawn Burst Instantaneous","Spawning","Spawns a burst at the specified emitter time.",Stage::EmitterUpdate,
        {{"Time",ValueType::Float,{}},{"Count",ValueType::Float,{20,0,0,0}}},{}},
    {"InitializeParticle","Initialize Particle","Initialization","Sets the initial particle attributes.",Stage::ParticleSpawn,
        {{"Lifetime",ValueType::Float,{2,0,0,0}},{"Position",ValueType::Float3,{}},{"Velocity",ValueType::Float3,{0,3,0,0}},
         {"Color",ValueType::Float4,{1,0.5f,0.1f,1}},{"Size",ValueType::Float,{0.05f,0,0,0}}},{}},
    {"BoxLocation","Box Location","Location","Samples a box centered at the specified position.",Stage::ParticleSpawn,
        {{"Center",ValueType::Float3,{}},{"Extent",ValueType::Float3,{0.5f,0.5f,0.5f,0}}},"InitializeParticle"},
    {"AddVelocity","Add Velocity","Velocity","Adds velocity to the current parameter map.",Stage::ParticleSpawn,{{"Velocity",ValueType::Float3,{0,1,0,0}}},"InitializeParticle"},
    {"GravityForce","Gravity Force","Forces","Accumulates acceleration for the force solver.",Stage::ParticleUpdate,{{"Acceleration",ValueType::Float3,{0,-9.81f,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"Drag","Drag","Forces","Accumulates linear drag for the force solver.",Stage::ParticleUpdate,{{"Drag",ValueType::Float,{0.1f,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsDrag",ValueType::Float,{}}}},
    {"SolveForcesAndVelocity","Solve Forces and Velocity","Solvers","Integrates velocity and position in module order.",Stage::ParticleUpdate,{},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}},{"Particles.PhysicsDrag",ValueType::Float,{}}}},
    {"ScaleColor","Scale Color","Color","Multiplies particle color by an input.",Stage::ParticleUpdate,{{"Scale",ValueType::Float4,{1,1,1,1}}},{},{Stage::Simulation}},
    {"ColorOverLife","Color over Life","Color","Interpolates color over normalized particle age.",Stage::ParticleUpdate,
        {{"Start",ValueType::Float4,{1,0.5f,0.1f,1}},{"End",ValueType::Float4,{1,0.1f,0,0}}},{},{Stage::Simulation}},
    {"ScaleSize","Scale Sprite Size","Size","Multiplies the current particle size.",Stage::ParticleUpdate,{{"Scale",ValueType::Float,{1,0,0,0}}},{},{Stage::Simulation}},
    // Location
    {"SphereLocation","Sphere Location","Location","Samples a sphere centered at the specified position.",Stage::ParticleSpawn,
        {{"Center",ValueType::Float3,{}},{"Radius",ValueType::Float,{0.5f,0,0,0}},{"Surface Only",ValueType::Float,{1,0,0,0}}},"InitializeParticle"},
    {"CylinderLocation","Cylinder Location","Location","Samples a cylinder centered at the specified position.",Stage::ParticleSpawn,
        {{"Center",ValueType::Float3,{}},{"Radius",ValueType::Float,{0.5f,0,0,0}},{"Height",ValueType::Float,{0.5f,0,0,0}},{"Surface Only",ValueType::Float,{1,0,0,0}}},"InitializeParticle"},
    {"ConeLocation","Cone Location","Location","Samples a cone along the specified direction, in degrees.",Stage::ParticleSpawn,
        {{"Origin",ValueType::Float3,{}},{"Direction",ValueType::Float3,{0,1,0,0}},{"Angle",ValueType::Float,{30,0,0,0}},{"Length",ValueType::Float,{1,0,0,0}}},"InitializeParticle"},
    {"OffsetPosition","Offset Position","Location","Offsets the current position.",Stage::ParticleSpawn,
        {{"Offset",ValueType::Float3,{}}},"InitializeParticle"},
    {"JitterPosition","Jitter Position","Location","Adds a per-frame random offset to the position.",Stage::ParticleUpdate,
        {{"Jitter",ValueType::Float3,{}}},{}},
    // Velocity
    {"AddVelocityInCone","Add Velocity in Cone","Velocity","Adds velocity within a cone, in degrees.",Stage::ParticleSpawn,
        {{"Direction",ValueType::Float3,{0,1,0,0}},{"Angle",ValueType::Float,{30,0,0,0}},{"Speed",ValueType::Float,{1,0,0,0}}},"InitializeParticle"},
    {"AddVelocityFromPoint","Add Velocity from Point","Velocity","Adds velocity directed away from a point.",Stage::ParticleSpawn,
        {{"Position",ValueType::Float3,{}},{"Velocity Scale",ValueType::Float,{1,0,0,0}}},"InitializeParticle"},
    {"ScaleVelocity","Scale Velocity","Velocity","Scales the current velocity.",Stage::ParticleUpdate,{{"Scale",ValueType::Float,{1,0,0,0}}},{}},
    // Forces
    {"AccelerationForce","Acceleration Force","Forces","Accumulates a constant acceleration for the force solver.",Stage::ParticleUpdate,
        {{"Acceleration",ValueType::Float3,{0,-9.81f,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"PointAttractionForce","Point Attraction Force","Forces","Pulls particles towards a point.",Stage::ParticleUpdate,
        {{"Attractor Position",ValueType::Float3,{}},{"Attraction Strength",ValueType::Float,{10,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"VortexForce","Vortex Force","Forces","Pushes particles around an axis.",Stage::ParticleUpdate,
        {{"Vortex Origin",ValueType::Float3,{}},{"Vortex Axis",ValueType::Float3,{0,1,0,0}},{"Vortex Strength",ValueType::Float,{10,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    // Orientation
    {"SpriteRotationRate","Sprite Rotation Rate","Orientation","Rotates sprites over time, in degrees per second.",Stage::ParticleUpdate,
        {{"Rotation Rate",ValueType::Float,{90,0,0,0}}},{},{},{{"Particles.Rotation",ValueType::Float,{}}}},
    // SubUV
    {"SubUVAnimation","SubUV Animation","SubUV","Selects the sub image index across normalized age.",Stage::ParticleUpdate,
        {{"First Frame",ValueType::Float,{}},{"Frame Count",ValueType::Float,{4,0,0,0}},{"Random Start",ValueType::Float,{}}},{},{},{{"Particles.SubImageIndex",ValueType::Float,{}}}},
    // Size
    {"SpriteSizeBySpeed","Scale Sprite Size by Speed","Size","Scales sprite size between the given limits by particle speed.",Stage::ParticleUpdate,
        {{"Min Scale",ValueType::Float,{1,0,0,0}},{"Max Scale",ValueType::Float,{1,0,0,0}},{"Speed Threshold",ValueType::Float,{1,0,0,0}}},{}},
    // Location (second batch)
    {"TorusLocation","Torus Location","Location","Samples a torus surface around the specified axis.",Stage::ParticleSpawn,
        {{"Center",ValueType::Float3,{}},{"Major Radius",ValueType::Float,{0.5f,0,0,0}},{"Minor Radius",ValueType::Float,{0.1f,0,0,0}},{"Axis",ValueType::Float3,{0,1,0,0}}},"InitializeParticle"},
    {"WedgeLocation","Wedge Location","Location","Distributes particles through a 3D wedge.",Stage::ParticleSpawn,
        {{"Origin",ValueType::Float3,{}},{"Extent",ValueType::Float3,{0.5f,0.5f,0.5f,0}},{"Angle",ValueType::Float,{30,0,0,0}},{"Axis",ValueType::Float3,{0,1,0,0}}},"InitializeParticle"},
    {"RotateAroundPoint","Rotate Around Point","Location","Places the particle on a circle around a center point, in degrees.",Stage::ParticleSpawn,
        {{"Center",ValueType::Float3,{}},{"Radius",ValueType::Float,{1,0,0,0}},{"Angle",ValueType::Float,{}}},"InitializeParticle"},
    // Velocity (second batch)
    {"VortexVelocity","Vortex Velocity","Velocity","Adds angular velocity around an axis to the current velocity.",Stage::ParticleUpdate,
        {{"Vortex Origin",ValueType::Float3,{}},{"Vortex Axis",ValueType::Float3,{0,1,0,0}},{"Vortex Strength",ValueType::Float,{10,0,0,0}}},{}},
    {"AlignVelocityToRandomAxis","Align Velocity To Random Axis","Velocity","Keeps the speed but aligns velocity to a stable random axis.",Stage::ParticleUpdate,
        {{"Randomness",ValueType::Float,{1,0,0,0}}},{}},
    {"ScaleVelocityBySpeed","Scale Velocity by Speed","Velocity","Scales the current velocity between limits by its own magnitude.",Stage::ParticleUpdate,
        {{"Min Scale",ValueType::Float,{1,0,0,0}},{"Max Scale",ValueType::Float,{1,0,0,0}},{"Speed Threshold",ValueType::Float,{1,0,0,0}}},{}},
    // Forces (second batch)
    {"LinearForce","Linear Force","Forces","Accumulates a linear acceleration for the force solver.",Stage::ParticleUpdate,
        {{"Acceleration",ValueType::Float3,{0,-9.81f,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"LimitForce","Limit Force","Forces","Clamps the accumulated force magnitude.",Stage::ParticleUpdate,
        {{"Force Limit",ValueType::Float,{100,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    // Color (second batch)
    {"ScaleColorBySpeed","Scale Color by Speed","Color","Scales color between limits by the current speed.",Stage::ParticleUpdate,
        {{"Min Scale",ValueType::Float4,{1,1,1,1}},{"Max Scale",ValueType::Float4,{1,1,1,1}},{"Speed Threshold",ValueType::Float,{1,0,0,0}}},{},{Stage::Simulation}},
    {"ScaleColorByVelocity","Scale Color by Velocity","Color","Multiplies color by the velocity, component wise.",Stage::ParticleUpdate,
        {{"Scale",ValueType::Float,{0.1f,0,0,0}}},{},{Stage::Simulation}},
    // Size (second batch)
    {"SpriteSizeByVelocity","Scale Sprite Size by Velocity","Size","Scales sprite size by the velocity magnitude, component wise.",Stage::ParticleUpdate,
        {{"Scale",ValueType::Float,{0.1f,0,0,0}}},{}},
    // Lifecycle
    {"KillParticles","Kill Particles","Kill","Kills the particle when the input is true.",Stage::ParticleUpdate,
        {{"Kill",ValueType::Float,{0,0,0,0}}},{}},
    // Mesh renderer attributes: the shader already consumes Particles.Scale and
    // Particles.MeshOrientation, but no module could write them until now.
    {"ScaleMeshSize","Scale Mesh Size","Size","Multiplies the current mesh scale.",Stage::ParticleUpdate,
        {{"Scale",ValueType::Float,{1,0,0,0}}},{},{},{{"Particles.Scale",ValueType::Float3,{1,1,1,0}}}},
    {"ScaleMeshSizeBySpeed","Scale Mesh Size by Speed","Size","Scales the mesh size between limits by the current speed.",Stage::ParticleUpdate,
        {{"Min Scale",ValueType::Float,{1,0,0,0}},{"Max Scale",ValueType::Float,{1,0,0,0}},{"Speed Threshold",ValueType::Float,{1,0,0,0}}},{},{},{{"Particles.Scale",ValueType::Float3,{1,1,1,0}}}},
    {"InitialMeshOrientation","Initial Mesh Orientation","Orientation","Sets the mesh orientation quaternion from an axis and angle, in degrees.",Stage::ParticleSpawn,
        {{"Axis",ValueType::Float3,{0,1,0,0}},{"Angle",ValueType::Float,{}}},"InitializeParticle",{},{{"Particles.MeshOrientation",ValueType::Float4,{0,0,0,1}}}},
    {"UpdateMeshOrientation","Update Mesh Orientation","Orientation","Rotates the mesh orientation quaternion over time from an angular velocity, in degrees per second.",Stage::ParticleUpdate,
        {{"Angular Velocity",ValueType::Float3,{0,90,0,0}}},{},{},{{"Particles.MeshOrientation",ValueType::Float4,{0,0,0,1}}}},
    {"MeshRotationRate","Mesh Rotation Rate","Orientation","Adds a constant spin about a local axis, in degrees per second.",Stage::ParticleUpdate,
        {{"Axis",ValueType::Float3,{0,1,0,0}},{"Rate",ValueType::Float,{90,0,0,0}}},{},{},{{"Particles.MeshOrientation",ValueType::Float4,{0,0,0,1}}}},
    // Pure attribute math that needs no new renderer or data interface.
    // Interpolate/Fade Over Time are intentionally absent: the reference lets
    // the user pick which attribute to blend, and the typed input model has no
    // attribute-destination binding yet, so a fixed-target version would
    // misrepresent the module.
    {"FrameCounter","Frame Counter","Utility","Increments a per-particle counter each frame while the input is true.",Stage::ParticleUpdate,
        {{"Increment Counter",ValueType::Float,{1,0,0,0}}},{},{},{{"Particles.FrameCounter",ValueType::Float,{}}}},
    {"TrackDistanceTraveled","Track Distance Traveled","Math","Accumulates the distance the particle travels.",Stage::ParticleUpdate,
        {{"Scale",ValueType::Float,{1,0,0,0}}},{},{},{{"Particles.DistanceTraveled",ValueType::Float,{}}}},
    {"ConeMask","Cone Mask","Math / Blend","Writes one inside a cone and zero outside it.",Stage::ParticleUpdate,
        {{"Cone Origin",ValueType::Float3,{}},{"Cone Direction",ValueType::Float3,{0,1,0,0}},{"Cone Angle",ValueType::Float,{30,0,0,0}}},{},{},{{"Particles.ConeMask",ValueType::Float,{}}}},
    // Dynamic material parameters: the sprite shader already forwards four
    // float4 vertex interpolators to the material, but no module could write
    // them. All four slots are exposed with the same defaults the renderer uses.
    {"DynamicMaterialParameters","Dynamic Material Parameters","Material","Writes the dynamic material parameter vertex interpolators.",Stage::ParticleUpdate,
        {{"Index 0",ValueType::Float4,{1,1,1,1}},{"Index 1",ValueType::Float4,{1,1,1,1}},
         {"Index 2",ValueType::Float4,{1,1,1,1}},{"Index 3",ValueType::Float4,{1,1,1,1}}},{},{},
        {{"Particles.DynamicMaterialParameter",ValueType::Float4,{1,1,1,1}},
         {"Particles.DynamicMaterialParameter1",ValueType::Float4,{1,1,1,1}},
         {"Particles.DynamicMaterialParameter2",ValueType::Float4,{1,1,1,1}},
         {"Particles.DynamicMaterialParameter3",ValueType::Float4,{1,1,1,1}}}},
    {"Color","Color","Color","Directly sets particle colour with scale factors for the RGB and alpha parts.",Stage::ParticleUpdate,
        {{"Color",ValueType::Float3,{1,1,1,0}},{"Alpha",ValueType::Float,{1,0,0,0}},
         {"Scale",ValueType::Float,{1,0,0,0}}},{},{}},
    {"ApplyRotationVector","Apply Rotation Vector","Rotation","Converts an angular velocity into a quaternion and multiplies it into the mesh orientation.",Stage::ParticleUpdate,
        {{"Rotational Velocity",ValueType::Float3,{0,90,0,0}}},{},{},{{"Particles.MeshOrientation",ValueType::Float4,{0,0,0,1}}}},
    {"IncrementOverTime","Increment Over Time","Utility","Increases the bound value each frame by delta time times a user rate.",Stage::ParticleUpdate,
        {{"Increment",ValueType::Float,{1,0,0,0}}},{},{},{{"Particles.IncrementCounter",ValueType::Float,{}}}},
    {"ConstrainVectorToCone","Constrain Vector to Cone","Math","Constrains the velocity vector into a cone around the given direction, in degrees.",Stage::ParticleUpdate,
        {{"Cone Direction",ValueType::Float3,{0,1,0,0}},{"Cone Angle",ValueType::Float,{30,0,0,0}}},{},{}},
    {"FindClosestPointOnLineSegment","Find Closest Point on Line Segment","Math","Writes how far along the bound segment the particle sits, clamped to the segment.",Stage::ParticleUpdate,
        {{"Segment Start",ValueType::Float3,{}},{"Segment End",ValueType::Float3,{0,0,1,0}}},{},{},{{"Particles.LineSegmentFraction",ValueType::Float,{}}}},
    // Forces (fifth batch): the remaining force primitives that need no new
    // data interface or noise texture and accumulate into the existing solver.
    {"LineAttractionForce","Line Attraction Force","Forces","Pulls particles towards the closest point on a line segment.",Stage::ParticleUpdate,
        {{"Line Start",ValueType::Float3,{}},{"Line End",ValueType::Float3,{0,0,1,0}},{"Attraction Strength",ValueType::Float,{10,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"SpringForce","Spring Force","Forces","Pulls particles towards a rest position with spring damping.",Stage::ParticleUpdate,
        {{"Rest Position",ValueType::Float3,{}},{"Spring",ValueType::Float,{10,0,0,0}},{"Damping",ValueType::Float,{1,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"VectorNoiseForce","Vector Noise Force","Forces","Adds a per-particle random force that is stable across the particle lifetime.",Stage::ParticleUpdate,
        {{"Noise Strength",ValueType::Float3,{10,10,10,0}},{"Noise Frequency",ValueType::Float,{1,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"PointRepulsionForce","Point Repulsion Force","Forces","Pushes particles away from a point, with optional falloff by distance.",Stage::ParticleUpdate,
        {{"Repulsor Position",ValueType::Float3,{}},{"Repulsion Strength",ValueType::Float,{10,0,0,0}},{"Falloff Radius",ValueType::Float,{1,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"WindForce","Wind Force","Forces","Blends the particle velocity towards a wind velocity over time.",Stage::ParticleUpdate,
        {{"Wind Velocity",ValueType::Float3,{1,0,0,0}},{"Wind Scale",ValueType::Float,{1,0,0,0}}},{},{}},
    // Sixth batch: analytic kill volumes, one-shot gates, mass/energy and region
    // masks. All of these are expressible with the existing attribute set.
    {"KillParticlesInVolume","Kill Particles in Volume","Kill","Kills particles inside an analytic sphere, with an optional inverted test.",Stage::ParticleUpdate,
        {{"Volume Center",ValueType::Float3,{}},{"Volume Radius",ValueType::Float,{1,0,0,0}},{"Invert",ValueType::Float,{0,0,0,0}}},{}},
    {"DoOnce","Do Once","Utility","Writes one the first frame the trigger is true, then zero afterwards.",Stage::ParticleUpdate,
        {{"Trigger",ValueType::Float,{1,0,0,0}}},{},{},{{"Particles.DoOnce",ValueType::Float,{}}}},
    {"CalculateMassAndRotationalInertiaByVolume","Calculate Mass and Rotational Inertia by Volume","Mass","Derives mass and rotational inertia from the particle size and a density, in kg per cubic metre.",Stage::ParticleUpdate,
        {{"Density",ValueType::Float,{1000,0,0,0}}},{},{},
        {{"Particles.Mass",ValueType::Float,{}},{"Particles.RotationalInertia",ValueType::Float,{}}}},
    {"FindKineticAndPotentialEnergy","Find Kinetic and Potential Energy","Physics","Writes kinetic energy from the velocity and the total including potential energy.",Stage::ParticleUpdate,
        {{"Mass",ValueType::Float,{1,0,0,0}},{"Potential Energy",ValueType::Float,{}}},{},{},
        {{"Particles.KineticEnergy",ValueType::Float,{}},{"Particles.TotalEnergy",ValueType::Float,{}}}},
    {"RegionMask","Region Mask","Mask","Writes one inside an axis aligned region and zero outside it.",Stage::ParticleUpdate,
        {{"Region Minimum",ValueType::Float3,{-1,-1,-1,0}},{"Region Maximum",ValueType::Float3,{1,1,1,0}}},{},{},
        {{"Particles.RegionMask",ValueType::Float,{}}}},
    // Seventh batch: derived state that is fully expressible with the existing
    // particle attributes (previous position, owner scale, noise output).
    {"CalculateAccurateVelocity","Calculate Accurate Velocity","Post Solve","Derives velocity from the position change since the previous frame.",Stage::ParticleUpdate,
        {{"Velocity Scale",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.PreviousPosition",ValueType::Float3,{}}}},
    {"InheritVelocity","Inherit Velocity","Velocity","Adds a scaled source velocity to the particle velocity.",Stage::ParticleUpdate,
        {{"Source Velocity",ValueType::Float3,{}},{"Inherit Scale",ValueType::Float,{1,0,0,0}}},{}},
    {"ApplyOwnerScaleToAttributes","Apply Owner Scale to Attributes","Size","Scales position and size by an owner scale, per axis.",Stage::ParticleUpdate,
        {{"Owner Scale",ValueType::Float3,{1,1,1,0}}},{},{}},
    {"VectorNoiseValue","Vector Noise Value","Utility","Writes a stable per-particle noise vector for later modules.",Stage::ParticleUpdate,
        {{"Noise Strength",ValueType::Float3,{1,1,1,0}},{"Noise Offset",ValueType::Float3,{}}},{},{},
        {{"Particles.NoiseValue",ValueType::Float3,{}}}},
    {"SolveFloatSpringConstraint","Solve Float Spring Constraint","Math","Solves a damped spring on a scalar attribute towards a rest value.",Stage::ParticleUpdate,
        {{"Rest Value",ValueType::Float,{}},{"Spring",ValueType::Float,{10,0,0,0}},{"Damping",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.SpringValue",ValueType::Float,{}},{"Particles.SpringVelocity",ValueType::Float,{}}}},
    // Event handlers. These configure the emitter's event receiver rather than
    // contributing to the particle program, which mirrors how Niagara pairs a
    // Generate module with a Receive module on another emitter.
    {"EventHandlerProperties","Event Handler Properties","Events","Selects the source emitter, execution mode and per-tick event limit.",Stage::Event,
        {{"Source Emitter Index",ValueType::Float,{-1,0,0,0}},{"Execution Mode",ValueType::Float,{}},{"Max Events Per Frame",ValueType::Float,{}}},{}},
    {"ReceiveDeathEvent","Receive Death Event","Events","Spawns particles at the positions of death events from another emitter.",Stage::Event,
        {{"Spawn Count",ValueType::Float,{1,0,0,0}},{"Random Age",ValueType::Float,{}}},{}},
    {"ReceiveCollisionEvent","Receive Collision Event","Events","Spawns particles at the positions of collision events from another emitter.",Stage::Event,
        {{"Spawn Count",ValueType::Float,{1,0,0,0}},{"Random Age",ValueType::Float,{}}},{}},
    {"ReceiveLocationEvent","Receive Location Event","Events","Spawns particles at the positions of spawn events from another emitter.",Stage::Event,
        {{"Spawn Count",ValueType::Float,{1,0,0,0}},{"Random Age",ValueType::Float,{}}},{}},
    // Camera-relative modules. They read the reserved Camera.* parameters that
    // the System host publishes before each tick.
    {"CameraOffset","Camera Offset","Camera","Offsets the particle along the vector from the particle towards the camera.",Stage::ParticleUpdate,
        {{"Offset Scale",ValueType::Float,{1,0,0,0}}},{}},
    {"ViewMask","View Mask","Default","Writes a mask by the particle's distance to the active viewport edge, and one when inside the view.",Stage::ParticleUpdate,
        {{"Edge Width",ValueType::Float,{}}},{},{},
        {{"Particles.ViewMask",ValueType::Float,{}},{"Particles.InsideView",ValueType::Float,{}}}},
    {"ViewRecycler","View Recycler","Default","Moves particles that fall outside the view back inside it along the camera axis.",Stage::ParticleUpdate,
        {{"Recycle Margin",ValueType::Float,{0.1f,0,0,0}}},{}},
    {"MaintainInCameraParticleScale","Maintain in Camera Particle Scale","Camera","Scales the particle by its camera-relative depth and the camera FOV.",Stage::ParticleUpdate,
        {{"Reference Distance",ValueType::Float,{100,0,0,0}},{"Reference Fov",ValueType::Float,{60,0,0,0}}},{}},
    // Eighth batch: analytic geometry, mass-driven size/inertia and rotation.
    {"FindClosestPointOnTriangle","Find Closest Point on Triangle","Math","Writes how close the bound triangle's closest point is to the particle.",Stage::ParticleUpdate,
        {{"Vertex A",ValueType::Float3,{}},{"Vertex B",ValueType::Float3,{1,0,0,0}},{"Vertex C",ValueType::Float3,{0,1,0,0}}},{},{},
        {{"Particles.TriangleDistance",ValueType::Float,{}}}},
    {"CalculateSizeAndRotationalInertiaByMass","Calculate Size and Rotational Inertia by Mass","Mass","Derives sprite size and rotational inertia from a user mass and density.",Stage::ParticleUpdate,
        {{"Mass",ValueType::Float,{1,0,0,0}},{"Density",ValueType::Float,{1000,0,0,0}}},{},{},
        {{"Particles.RotationalInertia",ValueType::Float,{}}}},
    {"AddRotationalVelocity","Add Rotational Velocity","Physics","Adds to the particle's rotational velocity, in degrees per second.",Stage::ParticleUpdate,
        {{"Rotational Velocity",ValueType::Float3,{0,90,0,0}}},{},{},
        {{"Particles.RotationalVelocity",ValueType::Float3,{}}}},
    {"UpdateVelocityOnMassChange","Update Velocity on Mass Change","Mass","Scales velocity inversely with mass so momentum is preserved.",Stage::ParticleUpdate,
        {{"Previous Mass",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.Mass",ValueType::Float,{1,0,0,0}}}},
    // Ninth batch: engine frame/time context values.
    {"FirstFrame","First Frame","Lifetime","Writes one on the system's first tick and zero afterwards.",Stage::ParticleUpdate,
        {},{},{},
        {{"Particles.FirstFrame",ValueType::Float,{1,0,0,0}}}},
    {"EmitterFrameCounter","Emitter Frame Counter","Utility","Increments an emitter-level counter each step while enabled.",Stage::ParticleUpdate,
        {{"Increment Counter",ValueType::Float,{1,0,0,0}}},{},{},{{"Particles.EmitterFrameCounter",ValueType::Float,{}}}},
    {"Timeline","Timeline","Default","Drives a timeline from the system age and writes raw and normalised position.",Stage::ParticleUpdate,
        {{"Timeline Length",ValueType::Float,{1,0,0,0}},{"Play Rate",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.TimelinePosition",ValueType::Float,{}},{"Particles.TimelineNormalized",ValueType::Float,{}},
         {"Particles.TimelinePlaying",ValueType::Float,{0,0,0,0}}}},
    {"TimeBasedStateMachine","Time Based State Machine","Utility","Writes a two-state on/off value from a repeating timeline cycle.",Stage::ParticleUpdate,
        {{"On Duration",ValueType::Float,{1,0,0,0}},{"Off Duration",ValueType::Float,{1,0,0,0}},{"Rate",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.OnOffPercentage",ValueType::Float,{}}}},
    // Tenth batch: system transform and system-level completion.
    {"SystemLocation","System Location","Location","Offsets spawned particles by the owning system position.",Stage::ParticleSpawn,
        {{"Offset",ValueType::Float3,{}}},"InitializeParticle"},
    {"SystemState","System State","Lifetime","Writes the system age and its normalised position over the system duration.",Stage::ParticleUpdate,
        {{"System Duration",ValueType::Float,{10,0,0,0}}},{},{},
        {{"Particles.SystemAge",ValueType::Float,{}},{"Particles.SystemNormalizedAge",ValueType::Float,{}}}},
    {"InheritSystemVelocity","Inherit System Velocity","Velocity","Adds the owning system velocity to spawned particles.",Stage::ParticleSpawn,
        {{"Inherit Scale",ValueType::Float,{1,0,0,0}}},"InitializeParticle"},
    {"CompleteIfUnused","Complete If Unused","Default","Completes the whole System once every emitter has been empty for the grace period.",Stage::EmitterUpdate,
        {{"Unused Grace Period",ValueType::Float,{0.5f,0,0,0}}},{}},
    {"SpawnPerUnit","Spawn Per Unit","Spawning","Spawns particles in proportion to the distance the owning system travels.",Stage::EmitterUpdate,
        {{"Spawn Per Unit",ValueType::Float,{1,0,0,0}}},{}},
    // Ribbon/Beam. SpawnBeam places particles along the spline between its two
    // endpoints and writes the link order the ribbon renderer consumes.
    {"RibbonWidth","Ribbon Width","Ribbon","Sets the ribbon strip width, optionally varying along the link order.",Stage::ParticleUpdate,
        {{"Width",ValueType::Float,{0.1f,0,0,0}},{"Width Scale",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.RibbonWidth",ValueType::Float,{}}}},
    {"SpawnBeam","Spawn Beam","Beam","Places particles along the line between two endpoints and writes their link order.",Stage::ParticleSpawn,
        {{"Start",ValueType::Float3,{}},{"End",ValueType::Float3,{0,0,100,0}},{"Particles Per Unit",ValueType::Float,{1,0,0,0}}},"InitializeParticle",{},
        {{"Particles.RibbonLinkOrder",ValueType::Float,{}},{"Particles.RibbonWidth",ValueType::Float,{}}}},
    {"UpdateBeam","Update Beam","Beam","Recomputes the beam spline endpoints every frame, keeping the link order stable.",Stage::ParticleUpdate,
        {{"Start",ValueType::Float3,{}},{"End",ValueType::Float3,{0,0,100,0}}},{},{},
        {{"Particles.RibbonLinkOrder",ValueType::Float,{}}}},
    {"BeamWidth","Beam Width","Beam","Sets the ribbon width along the beam using its link order.",Stage::ParticleUpdate,
        {{"Width",ValueType::Float,{0.1f,0,0,0}},{"Scale",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.RibbonWidth",ValueType::Float,{}}}},
    {"ScaleBeamWidth","Scale Beam Width","Beam","Scales the current ribbon width by a factor.",Stage::ParticleUpdate,
        {{"Scale",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.RibbonWidth",ValueType::Float,{}}}},
    // Eleventh batch: reusable scalar state machines and spatial constraints.
    // These modules use existing particle columns and the shared force solver,
    // so CPU and GPU consume one deterministic graph without new interfaces.
    {"ScaleRibbonWidth","Scale Ribbon Width","Ribbon","Scales the ribbon width established by an earlier width module.",Stage::ParticleUpdate,
        {{"Scale",ValueType::Float,{1,0,0,0}}},{},{},
        {{"Particles.RibbonWidth",ValueType::Float,{}}}},
    {"FadeOverTime","Fade Over Time","Math","Converges a stored value down to a target without passing a lower bound.",Stage::ParticleUpdate,
        {{"Target Value",ValueType::Float,{}},{"Fade Rate",ValueType::Float,{1,0,0,0}},{"Minimum Value",ValueType::Float,{0,0,0,0}},
         {"Initial Value",ValueType::Float,{1,0,0,0}}},{},{Stage::ParticleSpawn},
        {{"Particles.FadeValue",ValueType::Float,{1,0,0,0}}}},
    {"InterpolateOverTime","Interpolate Over Time","Math","Moves a stored scalar toward a target at a bounded rate.",Stage::ParticleUpdate,
        {{"Target Value",ValueType::Float,{}},{"Rate Of Change",ValueType::Float,{1,0,0,0}},{"Initial Value",ValueType::Float,{}}},{},{Stage::ParticleSpawn},
        {{"Particles.InterpolatedValue",ValueType::Float,{}}}},
    {"MaintainA_SetDistanceBetweenPoints","Maintain A: Set Distance Between Points","Constraints","Projects the particle position to an ideal distance from its pivot.",Stage::ParticleUpdate,
        {{"Target Position",ValueType::Float3,{}},{"Ideal Distance",ValueType::Float,{1,0,0,0}},{"Influence",ValueType::Float,{1,0,0,0}}},{},{}},
    {"PendulumSetup","Pendulum Setup","Constraints","Stores the pivot, rest axis and length used by the pendulum constraint.",Stage::ParticleSpawn,
        {{"Pendulum Pivot",ValueType::Float3,{}},{"Pendulum Rest Axis",ValueType::Float3,{0,-1,0,0}},{"Pendulum Length",ValueType::Float,{1,0,0,0}},
         {"Enable Spring Driver",ValueType::Float,{0,0,0,0}},{"Spring Tightness",ValueType::Float,{20,0,0,0}}},{},{},
        {{"Particles.PendulumPivot",ValueType::Float3,{}},{"Particles.PendulumRestAxis",ValueType::Float3,{0,-1,0,0}},
         {"Particles.PendulumLength",ValueType::Float,{1,0,0,0}},{"Particles.PendulumSpring",ValueType::Float,{0,0,0,0}},
         {"Particles.PendulumTightness",ValueType::Float,{20,0,0,0}}}},
    {"PendulumConstraint","Pendulum Constraint","Constraints","Keeps the particle at the configured pendulum length and applies spring-driven correction.",Stage::ParticleUpdate,
        {{"Use Angle Constraint",ValueType::Float,{0,0,0,0}},{"Angle Constraint",ValueType::Float,{45,0,0,0}}},{},{Stage::Simulation},
        {{"Particles.PendulumPivot",ValueType::Float3,{}},{"Particles.PendulumRestAxis",ValueType::Float3,{0,-1,0,0}},
         {"Particles.PendulumLength",ValueType::Float,{1,0,0,0}},
         {"Particles.PendulumSpring",ValueType::Float,{0,0,0,0}},{"Particles.PendulumTightness",ValueType::Float,{20,0,0,0}}}},
    {"PointForce","Point Force","Forces","Adds a force toward or away from an arbitrary point with optional radial falloff.",Stage::ParticleUpdate,
        {{"Force Origin",ValueType::Float3,{}},{"Force Strength",ValueType::Float,{-10,0,0,0}},{"Falloff Distance",ValueType::Float,{1,0,0,0}},
         {"Use Falloff",ValueType::Float,{1,0,0,0}},{"Invert Falloff",ValueType::Float,{0,0,0,0}}},{},{Stage::Simulation},{{"Particles.PhysicsForce",ValueType::Float3,{}}}},
    {"InheritSourceMovement","Inherit Source Movement","Forces","Adds the owning source movement to the particle velocity.",Stage::ParticleUpdate,
        {{"Source Velocity",ValueType::Float3,{}},{"Applied Force Scale",ValueType::Float,{1,0,0,0}}},"InitializeParticle",{Stage::ParticleSpawn}},
    // Event generation is an emitter policy, analogous to the reference
    // Generate modules. Keeping this in the Event execution stack lets the
    // authoring layer distinguish senders from receivers without adding
    // per-particle attributes.
    {"GenerateDeathEvent","Generate Death Event","Events","Emits a death event when a particle dies.",Stage::Event,
        {{"Event Generation Enabled",ValueType::Float,{1,0,0,0}}},{},{}},
    {"GenerateLocationEvent","Generate Location Event","Events","Emits a location event for every spawned or moving particle.",Stage::Event,
        {{"Event Generation Enabled",ValueType::Float,{1,0,0,0}}},{},{}},
    {"GenerateCollisionEvent","Generate Collision Event","Events","Emits a collision event when a particle collides.",Stage::Event,
        {{"Event Generation Enabled",ValueType::Float,{1,0,0,0}}},{},{}},
    {"BeamEmitterSetup","Beam Emitter Setup","Beam","Stores beam endpoints and tangents, then maps particles along the beam.",Stage::ParticleSpawn,
        {{"Beam Start",ValueType::Float3,{}},{"Beam End",ValueType::Float3,{0,0,100,0}},
         {"Beam Start Tangent",ValueType::Float3,{0,0,1,0}},{"Beam End Tangent",ValueType::Float3,{0,0,-1,0}},
         {"Use Beam Tangents",ValueType::Float,{1,0,0,0}},{"Width",ValueType::Float,{0.1f,0,0,0}}},"InitializeParticle",{},
        {{"Particles.RibbonLinkOrder",ValueType::Float,{}},{"Particles.RibbonWidth",ValueType::Float,{0.1f,0,0,0}},
         {"Particles.BeamStart",ValueType::Float3,{}},{"Particles.BeamEnd",ValueType::Float3,{0,0,100,0}},
         {"Particles.BeamStartTangent",ValueType::Float3,{0,0,1,0}},{"Particles.BeamEndTangent",ValueType::Float3,{0,0,-1,0}},
          {"Particles.BeamTangentWeight",ValueType::Float,{1,0,0,0}}}},
    {"ParticleState","Particle State","Lifetime","Updates particle age with optional lifetime looping and kill policy.",Stage::ParticleUpdate,
        {{"Loop Particles Lifetime",ValueType::Float,{0,0,0,0}},
         {"Kill Particles When Lifetime Has Elapsed",ValueType::Float,{1,0,0,0}}},{},{Stage::Simulation},
        {{"Particles.NormalizedAge",ValueType::Float,{}}}},
    {"SpriteBasedLine","Sprite Based Line","Debug","Turns two positions into the center, size and velocity used by a stretched sprite.",Stage::ParticleUpdate,
        {{"Point A",ValueType::Float3,{}},{"Point B",ValueType::Float3,{0,1,0,0}},{"Sprite Width",ValueType::Float,{0.1f,0,0,0}}},{},{},
        {{"Particles.Size",ValueType::Float,{0.1f,0,0,0}},{"Particles.Velocity",ValueType::Float3,{0,1,0,0}}}},
    // Thirteenth batch: world queries and A/B attribute blending. Collision is
    // an emitter policy that drives the existing PhysX trace and bounce path;
    // Lerp stays in the shared graph so CPU and GPU agree.
    {"Collision","Collision","Collision","Enables world collision queries for this emitter with radius, restitution and friction.",Stage::ParticleUpdate,
        {{"Collision Enabled",ValueType::Float,{1,0,0,0}},{"Particle Radius",ValueType::Float,{0.05f,0,0,0}},
         {"Restitution",ValueType::Float,{0.5f,0,0,0}},{"Friction",ValueType::Float,{0,0,0,0}},
         {"Collision Layers",ValueType::Float,{-1,0,0,0}}},{},{Stage::Simulation},
        {{"Particles.CollisionNormal",ValueType::Float3,{}},{"Particles.Colliding",ValueType::Float,{}}}},
    {"CollisionQueryAndResponse","Collision Query and Response","Collision","Enables world collision queries without overriding radius or response.",Stage::ParticleUpdate,
        {{"Collision Enabled",ValueType::Float,{1,0,0,0}}},{},{Stage::Simulation}},
    {"LerpParticleAttributes","Lerp Particle Attributes","Math","Blends position, velocity and color between an A and a B value.",Stage::ParticleUpdate,
        {{"Lerp Alpha",ValueType::Float,{0.5f,0,0,0}},{"Position A",ValueType::Float3,{}},{"Position B",ValueType::Float3,{}},
         {"Velocity A",ValueType::Float3,{}},{"Velocity B",ValueType::Float3,{}},{"Color A",ValueType::Float4,{1,1,1,1}},{"Color B",ValueType::Float4,{1,1,1,1}}},{},{}},
    // Fourteenth batch: the remaining force conversion path stays in the shared graph.
    {"MeshRotationForce","Mesh Rotation Force","Forces","Accumulates a rotational impulse from the yaw, pitch and roll torques.",Stage::ParticleUpdate,
        {{"Rotation",ValueType::Float3,{0,0,0,0}},{"Lever Radius",ValueType::Float,{1,0,0,0}}},{},{Stage::Simulation},
        {{"Particles.RotationalVelocity",ValueType::Float3,{}},{"Particles.PhysicsRotationalForce",ValueType::Float3,{}}}},
    {"ApplyInitialForces","Apply Initial Forces","Solvers","Converts accumulated forces into velocity, and optionally pre-offsets the initial position.",Stage::ParticleUpdate,
        {{"Apply Force to Velocity",ValueType::Float,{1,0,0,0}},{"Apply Force to Position",ValueType::Float,{0,0,0,0}},
         {"Apply Rotational Force to Rotational Velocity",ValueType::Float,{1,0,0,0}},{"Positional Force Warmup Time",ValueType::Float,{0,0,0,0}}},{},{Stage::Simulation},
        {{"Particles.RotationalVelocity",ValueType::Float3,{}},{"Particles.Mass",ValueType::Float,{1,0,0,0}},{"Particles.PhysicsForce",ValueType::Float3,{}},{"Particles.PhysicsRotationalForce",ValueType::Float3,{}}}},
    // Fifteenth batch: grid placement. Both modules consume the spawn ordinal
    // exposed by Operation::SpawnIndex, so cells are exact rather than random.
    {"GridLocation","Grid Location","Location","Places spawned particles on an evenly spaced grid, optionally jittered inside each cell.",Stage::ParticleSpawn,
        {{"Grid Origin",ValueType::Float3,{}},{"Grid X Count",ValueType::Float,{4,0,0,0}},{"Grid Y Count",ValueType::Float,{1,0,0,0}},
         {"Grid Z Count",ValueType::Float,{1,0,0,0}},{"XYZ Dimensions",ValueType::Float3,{1,1,1,0}},
         {"Jitter Inside Grid Cells",ValueType::Float,{0,0,0,0}}},"InitializeParticle",{},{}},
    {"SpawnParticlesInGrid","Spawn Particles in Grid","Utility","Writes a stable grid cell address and normalized cell age for each spawned particle.",Stage::ParticleSpawn,
        {{"Age",ValueType::Float,{0,0,0,0}},{"X Count",ValueType::Float,{4,0,0,0}},{"Y Count",ValueType::Float,{1,0,0,0}},
         {"Z Count",ValueType::Float,{1,0,0,0}}},"InitializeParticle",{},
        {{"Particles.GridCellIndex",ValueType::Float,{}},{"Particles.GridCellAge",ValueType::Float,{}},{"Particles.Age",ValueType::Float,{}},
         {"Particles.SpawnIndex",ValueType::Float,{}}}},
    {"PartitionParticles","Partition Particles","Utility","Splits particles into evenly spaced partitions and reports the normalized position within each partition.",Stage::ParticleUpdate,
        {{"Number Of Partitions",ValueType::Float,{4,0,0,0}},{"Particles Per Partition",ValueType::Float,{25,0,0,0}}},{},{},
        {{"Particles.Partition",ValueType::Float,{}},{"Particles.PartitionNormalized",ValueType::Float,{}}}},
    // Seventeenth batch: per-particle sprite orientation. The vertex shader
    // reads these two optional columns, so no shader ABI change is required.
    {"SpriteFacingAndAlignment","Sprite Facing and Alignment","Orientation","Sets the per-particle sprite facing and alignment vectors.",Stage::ParticleUpdate,
        {{"Facing",ValueType::Float3,{0,1,0,0}},{"Alignment",ValueType::Float3,{0,0,0,0}}},{},{},
        {{"Particles.SpriteFacing",ValueType::Float3,{0,1,0,0}},{"Particles.SpriteAlignment",ValueType::Float3,{0,0,0,0}}}},
    {"AlignSpriteToMeshOrientation","Align Sprite to Mesh Orientation","Orientation","Derives the sprite facing vector from the particle's mesh orientation.",Stage::ParticleUpdate,
        {{"Axis",ValueType::Float3,{1,0,0,0}}},{},{},
        {{"Particles.SpriteFacing",ValueType::Float3,{0,1,0,0}}}}
};
const bool moduleCapabilitiesApplied = []
{
    struct Entry { const char* id; uint32_t capabilities; };
    const Entry entries[] = {
        {"CameraOffset",ModuleCapability_Camera},
        {"ViewMask",ModuleCapability_Camera},
        {"ViewRecycler",ModuleCapability_Camera},
        {"MaintainInCameraParticleScale",ModuleCapability_Camera},
        {"FirstFrame",ModuleCapability_EngineValue},
        {"EmitterFrameCounter",ModuleCapability_EngineValue},
        {"Timeline",ModuleCapability_EngineValue},
        {"TimeBasedStateMachine",ModuleCapability_EngineValue},
        {"SystemLocation",ModuleCapability_EngineValue},
        {"InheritSystemVelocity",ModuleCapability_EngineValue},
        {"SystemState",ModuleCapability_EngineValue},
        {"SpawnPerUnit",ModuleCapability_EngineValue},
        {"SolveForcesAndVelocity",ModuleCapability_ConsumesForce | ModuleCapability_ConsumesDrag},
        {"ApplyInitialForces",ModuleCapability_ConsumesForce},
        {"LimitForce",ModuleCapability_ForceLimit},
        {"Collision",ModuleCapability_Collision},
        {"CollisionQueryAndResponse",ModuleCapability_Collision},
        {"SpawnBurst",ModuleCapability_SpawnBurst},
        {"CompleteIfUnused",ModuleCapability_CompleteIfUnused},
        {"PartitionParticles",ModuleCapability_PartitionParticles},
    };
    for (auto& module : library)
        for (const auto& entry : entries)
            if (module.id == entry.id) { module.capabilities = entry.capabilities; break; }
    return true;
}();
const bool inputMetadataApplied = [] { ApplyInputMetadata(library); return true; }();
bool Finite(const Value& value) { return std::all_of(value.begin(),value.end(),[](float v) { return std::isfinite(v); }); }
bool GuidValid(const std::string& text) { const auto id = Guid::TryParse(text); return id && id->IsValid(); }
Json EncodeGraph(const Graph& graph, bool editor)
{
    Json result{{"id",graph.id},{"stage",static_cast<uint32_t>(graph.stage)},{"nextNodeId",graph.nextNodeId},{"nodes",Json::array()}};
    Schema::EncodeGraphDeclarations(result,graph);
    for (const auto& node : graph.nodes)
    {
        Json entry{{"id",node.id},{"operation",static_cast<uint32_t>(node.operation)},{"type",static_cast<uint32_t>(node.type)},
            {"name",node.name},{"value",NLS::VFX::Detail::EncodeValue(node.value,node.type)},{"inputs",node.inputs}};
        if (!node.structType.empty()) entry["structType"] = node.structType;
        if (editor) entry["position"] = node.position;
        result["nodes"].push_back(std::move(entry));
    }
    return result;
}
Graph DecodeGraph(const Json& source)
{
    Graph graph;
    Schema::DecodeGraphDeclarations(source,graph);
    graph.id = source.at("id").get<std::string>();
    const auto stage = source.at("stage").get<uint32_t>();
    if (!GuidValid(graph.id) || stage > static_cast<uint32_t>(ScriptStage::SystemUpdate)) throw std::runtime_error("Invalid script graph");
    graph.stage = static_cast<ScriptStage>(stage);
    graph.nextNodeId = source.at("nextNodeId").get<NodeId>();
    const auto& nodes = source.at("nodes");
    if (!nodes.is_array() || nodes.size() > 65536) throw std::runtime_error("Invalid dynamic input node list");
    std::unordered_set<NodeId> ids;
    for (const auto& entry : nodes)
    {
        Node node;
        node.id = entry.at("id").get<NodeId>();
        const auto operation = entry.at("operation").get<uint32_t>(), type = entry.at("type").get<uint32_t>();
        if (!node.id || node.id >= (uint64_t{1}<<60) || !ids.insert(node.id).second || operation > static_cast<uint32_t>(Operation::DataInterfaceCall) || type > static_cast<uint32_t>(ValueType::Bool))
            throw std::runtime_error("Invalid dynamic input node");
        node.operation = static_cast<Operation>(operation); node.type = static_cast<ValueType>(type);
        node.name = entry.at("name").get<std::string>(); node.value = NLS::VFX::Detail::DecodeValue(entry.at("value"),node.type);
        node.inputs = entry.at("inputs").get<std::vector<NodeId>>();
        node.structType = entry.value("structType",std::string{});
        node.position = entry.value("position",std::array<float,2>{});
        if (!ValidValue(node.value,node.type) || !ValidInputCount(node.operation,node.inputs.size()) || !std::isfinite(node.position[0]) || !std::isfinite(node.position[1]))
            throw std::runtime_error("Invalid dynamic input value or pins");
        graph.nextNodeId = (std::max)(graph.nextNodeId,node.id+1);
        graph.nodes.push_back(std::move(node));
    }
    return graph;
}
}

namespace Detail
{
bool HasTypedData(const Json& source)
{
    if (source.is_object())
    {
        if (source.contains("type") && source["type"].is_number_unsigned() && source["type"].get<uint32_t>() > 3) return true;
        if (source.contains("operation") && source["operation"].is_number_unsigned() && source["operation"].get<uint32_t>() > static_cast<uint32_t>(Operation::StageIteration)) return true;
        if (source.contains("typed") && source["typed"].is_boolean() && source["typed"].get<bool>()) return true;
        if (source.contains("argumentTypes") && source["argumentTypes"].is_object() && !source["argumentTypes"].empty()) return true;
    }
    if (source.is_object() || source.is_array()) for (const auto& child : source) if (HasTypedData(child)) return true;
    return false;
}
Json EncodeStageBindings(const SimulationStageBindings& bindings)
    { Json result{{"enabled",bindings.enabled},{"iterations",bindings.iterations}}; if (bindings.typed) result["typed"] = true; return result; }
SimulationStageBindings DecodeStageBindings(const Json& source)
{
    SimulationStageBindings bindings{source.value("enabled",std::string{}),source.value("iterations",std::string{})};
    bindings.typed = source.value("typed",false);
    std::string error; if (!ValidateSimulationStageBindings(bindings,error)) throw std::runtime_error(error); return bindings;
}
Json EncodeGraph(const Graph& graph,bool editor) { return ::NLS::VFX::Authoring::EncodeGraph(graph,editor); }
Graph DecodeGraph(const Json& source) { return ::NLS::VFX::Authoring::DecodeGraph(source); }
Json EncodeReference(const VersionReference& reference) { return {{"asset",reference.asset},{"version",reference.version}}; }
VersionReference DecodeReference(const Json& source)
{
    VersionReference result{source.at("asset").get<std::string>(),source.at("version").get<std::string>()};
    if ((!result.Empty() && !GuidValid(result.asset)) || (!result.version.empty() && !GuidValid(result.version))) throw std::runtime_error("Invalid versioned asset reference");
    return result;
}
Json EncodeInput(const Input& input,bool editor)
{
    Json result{{"mode",static_cast<uint32_t>(input.mode)},{"value",NLS::VFX::Detail::EncodeValue(input.value,input.type)},{"parameter",input.parameter}};
    if (IsIntegralType(input.type)) result["type"] = static_cast<uint32_t>(input.type);
    if (input.mode == InputMode::DynamicInput) result["expression"] = EncodeGraph(input.expression,editor);
    if (input.mode == InputMode::Curve)
    {
        result["curve"] = Json::array();
        for (const auto& key : input.curve) result["curve"].push_back({{"time",key.time},{"value",key.value},{"arrive",key.arriveTangent},{"leave",key.leaveTangent},{"interpolation",static_cast<uint32_t>(key.interpolation)}});
    }
    if (input.mode == InputMode::Script)
    {
        result["scriptAsset"] = EncodeReference(input.scriptAsset); result["arguments"] = Json::object(); result["argumentTypes"] = Json::object();
        for (const auto& [name,value] : input.scriptArguments)
        {
            const auto type = input.scriptArgumentTypes.contains(name) ? input.scriptArgumentTypes.at(name) : ValueType::Float4;
            result["arguments"][name] = NLS::VFX::Detail::EncodeValue(value,type);
            if (IsIntegralType(type)) result["argumentTypes"][name] = static_cast<uint32_t>(type);
        }
    }
    return result;
}
Input DecodeInput(const Json& source)
{
    Input result; const auto mode = source.at("mode").get<uint32_t>();
    if (mode > 4) throw std::runtime_error("Unknown input mode");
    result.type = static_cast<ValueType>(source.value("type",0u));
    result.mode = static_cast<InputMode>(mode); result.value = NLS::VFX::Detail::DecodeValue(source.at("value"),result.type); result.parameter = source.at("parameter").get<std::string>();
    if (result.mode == InputMode::Script)
    {
        result.scriptAsset = DecodeReference(source.at("scriptAsset"));
        if (!source.at("arguments").is_object() || source.at("arguments").size() > 256) throw std::runtime_error("Invalid script argument list");
        const auto types = source.value("argumentTypes",Json::object());
        for (const auto& [name,value] : source.at("arguments").items())
        {
            const auto type = static_cast<ValueType>(types.value(name,static_cast<uint32_t>(ValueType::Float4)));
            result.scriptArguments[name] = NLS::VFX::Detail::DecodeValue(value,type); if (IsIntegralType(type)) result.scriptArgumentTypes[name] = type;
        }
    }
    if (result.mode == InputMode::DynamicInput) result.expression = DecodeGraph(source.at("expression"));
    if (result.mode == InputMode::Curve)
    {
        const auto& keys = source.at("curve");
        if (!keys.is_array() || keys.size() > 256) throw std::runtime_error("Invalid curve key list");
        for (const auto& key : keys)
        {
            const auto interpolation = key.at("interpolation").get<uint32_t>();
            if (interpolation > 1) throw std::runtime_error("Unknown curve interpolation");
            result.curve.push_back({key.at("time").get<float>(),key.at("value").get<Value>(),key.at("arrive").get<Value>(),key.at("leave").get<Value>(),static_cast<CurveInterpolation>(interpolation)});
        }
    }
    return result;
}
bool ValidateInput(const Input& input,std::string& error)
{
    if (static_cast<uint32_t>(input.mode) > 4 || !ValidValue(input.value,input.type) || (input.mode == InputMode::Parameter && input.parameter.empty()))
    { error = "Invalid input value or binding"; return false; }
    if (input.mode == InputMode::Script)
    {
        try { (void)DecodeReference(EncodeReference(input.scriptAsset)); }
        catch (const std::exception& exception) { error = exception.what(); return false; }
        if (input.scriptAsset.Empty() || input.scriptArguments.size() > 256) { error = "Dynamic input script reference is missing"; return false; }
        for (const auto& [name,value] : input.scriptArguments) if (name.empty() || !ValidValue(value,input.scriptArgumentTypes.contains(name) ? input.scriptArgumentTypes.at(name) : ValueType::Float4)) { error = "Invalid script input argument"; return false; }
    }
    if (input.mode == InputMode::DynamicInput)
    {
        try { (void)DecodeGraph(EncodeGraph(input.expression,true)); }
        catch (const std::exception& exception) { error = exception.what(); return false; }
    }
    if (input.mode == InputMode::Curve)
    {
        if (input.curve.empty() || input.curve.size() > 256) { error = "Curve requires 1 to 256 keys"; return false; }
        for (size_t i = 0; i < input.curve.size(); ++i)
        {
            const auto& key = input.curve[i];
            if (!std::isfinite(key.time) || !Finite(key.value) || !Finite(key.arriveTangent) || !Finite(key.leaveTangent) || static_cast<uint32_t>(key.interpolation) > 1 || (i && key.time-input.curve[i-1].time < 1e-5f))
            { error = "Invalid curve key or key order"; return false; }
        }
    }
    error.clear(); return true;
}
}
std::span<const ModuleDefinition> ModuleLibrary() { return library; }
const ModuleDefinition* FindModule(std::string_view id)
{
    const auto found = std::find_if(library.begin(),library.end(),[&](const auto& module) { return module.id == id; });
    return found == library.end() ? nullptr : &*found;
}
bool SupportsStage(const ModuleDefinition& module, Stage stage)
{ return module.stage == stage || std::find(module.additionalStages.begin(),module.additionalStages.end(),stage) != module.additionalStages.end(); }
const char* StageName(Stage stage)
{
    static constexpr const char* names[]{"System Spawn","System Update","Emitter Spawn","Emitter Update","Particle Spawn","Particle Update","Event Handler","Simulation Stage"};
    const auto index = static_cast<uint32_t>(stage);
    return index < std::size(names) ? names[index] : "Unknown Stage";
}
Module CreateModule(std::string_view script)
{
    Module result; result.id = Guid::New().ToString(); result.script = script;
    if (const auto* definition = FindModule(script))
        for (const auto& input : definition->inputs) result.inputs[input.name].value = input.defaultValue;
    return result;
}
Emitter CreateEmitter(std::string name)
{
    Emitter emitter; emitter.settings = CreateDefaultSystem().emitters.front();
    emitter.settings.name = std::move(name);
    emitter.settings.spawn.nodes.clear(); emitter.settings.update.nodes.clear();
    for (const auto stage : {Stage::EmitterSpawn,Stage::EmitterUpdate,Stage::ParticleSpawn,Stage::ParticleUpdate})
        emitter.stacks.push_back({Guid::New().ToString(),stage,StageName(stage)});
    FindStack(emitter,Stage::EmitterUpdate)->modules = {CreateModule("EmitterState"),CreateModule("SpawnRate")};
    FindStack(emitter,Stage::ParticleSpawn)->modules = {CreateModule("InitializeParticle"),CreateModule("BoxLocation")};
    FindStack(emitter,Stage::ParticleUpdate)->modules = {CreateModule("SolveForcesAndVelocity")};
    return emitter;
}
System CreateSystem(std::string name)
{
    System system; system.id = Guid::New().ToString(); system.name = std::move(name);
    const auto defaults = CreateDefaultSystem();
    system.systemAttributes = defaults.systemAttributes;
    system.systemSpawn = defaults.systemSpawn;
    system.systemUpdate = defaults.systemUpdate;
    system.emitters.push_back(CreateEmitter()); return system;
}
Stack* FindStack(Emitter& emitter, Stage stage)
{
    const auto found = std::find_if(emitter.stacks.begin(),emitter.stacks.end(),[&](const Stack& stack) { return stack.stage == stage; });
    return found == emitter.stacks.end() ? nullptr : &*found;
}
Stack CreateSimulationStage(std::string name)
{ return {Guid::New().ToString(),Stage::Simulation,std::move(name)}; }
bool MoveSimulationStage(Emitter& emitter,std::string_view id,size_t index,std::string& error)
{
    std::vector<size_t> slots; size_t from = 0; bool found = false;
    for (size_t i = 0; i < emitter.stacks.size(); ++i) if (emitter.stacks[i].stage == Stage::Simulation)
    { if (emitter.stacks[i].id == id) { from = slots.size(); found = true; } slots.push_back(i); }
    if (!found || index >= slots.size()) { error = "Simulation Stage or destination is missing"; return false; }
    while (from < index) { std::swap(emitter.stacks[slots[from]],emitter.stacks[slots[from+1]]); ++from; }
    while (from > index) { std::swap(emitter.stacks[slots[from]],emitter.stacks[slots[from-1]]); --from; }
    error.clear(); return true;
}
std::string DuplicateSimulationStage(Emitter& emitter,std::string_view id,std::string& error)
{
    if (std::count_if(emitter.stacks.begin(),emitter.stacks.end(),[](const auto& stack) { return stack.stage == Stage::Simulation; }) >= 64)
    { error = "An emitter supports at most 64 Simulation Stages"; return {}; }
    for (size_t i = 0; i < emitter.stacks.size(); ++i) if (emitter.stacks[i].id == id && emitter.stacks[i].stage == Stage::Simulation)
    {
        auto copy = emitter.stacks[i]; copy.id = Guid::New().ToString(); copy.inheritedId.clear(); copy.name += " Copy";
        for (auto& module : copy.modules)
        {
            module.id = Guid::New().ToString(); module.inheritedId.clear();
            for (auto& [name,input] : module.inputs) if (input.mode == InputMode::DynamicInput) input.expression.id = Guid::New().ToString();
        }
        const auto result = copy.id; emitter.stacks.insert(emitter.stacks.begin()+i+1,std::move(copy)); error.clear(); return result;
    }
    error = "Simulation Stage is missing"; return {};
}
Value EvaluateCurve(std::span<const CurveKey> keys, float time)
{
    if (keys.empty() || !std::isfinite(time)) return {};
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (size_t i = 1; i < keys.size(); ++i) if (time <= keys[i].time)
    {
        const auto& a = keys[i-1]; const auto& b = keys[i];
        const float duration = b.time-a.time;
        if (duration <= 0) return a.value;
        const float t = (time-a.time)/duration, t2 = t*t, t3 = t2*t;
        Value result{};
        for (size_t component = 0; component < 4; ++component)
            result[component] = a.interpolation == CurveInterpolation::Linear ? a.value[component]+(b.value[component]-a.value[component])*t :
                (2*t3-3*t2+1)*a.value[component]+(t3-2*t2+t)*duration*a.leaveTangent[component]+
                (-2*t3+3*t2)*b.value[component]+(t3-t2)*duration*b.arriveTangent[component];
        return result;
    }
    return keys.back().value;
}
bool Validate(const System& system, std::vector<Issue>& issues)
{
    issues.clear();
    if ((system.version < kSchemaVersion || system.version > 5) || system.kind != AssetKind::System || !GuidValid(system.id) || system.emitters.size() > 1024 || system.parameters.size() > 4096)
        issues.push_back({"Invalid or unsupported v2-v5 system header"});
    std::string timingError;
    if (!ValidateSystemTiming(system.timing,timingError)) issues.push_back({timingError});
    std::unordered_set<std::string> ids{system.id}, names;
    const auto unique = [&](const std::string& id) { return GuidValid(id) && ids.insert(id).second; };
    for (const auto& parameter : system.parameters)
        if (!unique(parameter.id) || !parameter.name.starts_with("User.") || !names.insert(parameter.name).second || !ValidValue(parameter.value,parameter.type))
            issues.push_back({"Invalid or duplicate user parameter: "+parameter.name});
    for (const auto& emitter : system.emitters)
    {
        if (!unique(emitter.settings.id) || emitter.stacks.size() > 128 || !std::isfinite(emitter.position[0]) || !std::isfinite(emitter.position[1]))
            issues.push_back({"Invalid emitter",emitter.settings.id});
        try { (void)Detail::DecodeReference(Detail::EncodeReference(emitter.parent)); }
        catch (const std::exception& exception) { issues.push_back({exception.what(),emitter.settings.id}); }
        std::unordered_set<std::string> overridden;
        for (const auto& item : emitter.overrides)
        {
            if (!GuidValid(item.module) || !overridden.insert(item.module).second || item.inputs.size() > 256)
                issues.push_back({"Invalid or duplicate inherited module override",emitter.settings.id,{},item.module});
            for (const auto& [name,input] : item.inputs)
            { std::string error; if (name.empty() || !Detail::ValidateInput(input,error)) issues.push_back({error,emitter.settings.id,{},item.module,name}); }
        }
        static const std::set<std::string> settingsNames{"target","capacity","initialCapacity","seed","duration","looping","interpolatedSpawning","sortByViewDepth","frustumCulling","collisionEnabled","collisionRadius","restitution","friction","collisionLayers","sprite","renderer","mesh"};
        std::set<std::string> settingOverrides;
        for (const auto& name : emitter.settingsOverrides) if (!settingsNames.contains(name) || !settingOverrides.insert(name).second) issues.push_back({"Unknown or duplicate emitter setting override: "+name,emitter.settings.id});
        std::set<std::string> stackOverrides;
        if (emitter.stackOverrides.size() > 64 || (emitter.parent.Empty() && !emitter.stackOverrides.empty()))
            issues.push_back({"Stage overrides require a parent and at most 64 entries",emitter.settings.id});
        for (const auto& item : emitter.stackOverrides)
        {
            if (!GuidValid(item.stack) || !stackOverrides.insert(item.stack).second || (item.iterations && *item.iterations > 128))
                issues.push_back({"Invalid or duplicate inherited stage override",emitter.settings.id,item.stack});
            std::string error; if (item.bindings && !ValidateSimulationStageBindings(*item.bindings,error)) issues.push_back({error,emitter.settings.id,item.stack});
        }
        std::unordered_set<uint32_t> stages;
        uint32_t simulationStages = 0, iterations = 0;
        for (const auto& stack : emitter.stacks)
        {
            const auto stage = static_cast<uint32_t>(stack.stage);
            if (!unique(stack.id) || stage > static_cast<uint32_t>(Stage::Simulation) || (stack.stage != Stage::Simulation && !stack.iterations) || stack.iterations > 128 || stack.modules.size() > 4096 ||
                (stack.stage != Stage::Simulation && stack.stage != Stage::Event && !stages.insert(stage).second))
                issues.push_back({"Invalid or duplicate stack",emitter.settings.id,stack.id});
            std::string bindingError;
            if (!ValidateSimulationStageBindings(stack.bindings,bindingError) ||
                (stack.stage != Stage::Simulation && (!stack.bindings.enabled.empty() || !stack.bindings.iterations.empty())))
                issues.push_back({bindingError.empty() ? "Control bindings require a Simulation Stage" : bindingError,emitter.settings.id,stack.id});
            if (stack.stage == Stage::Simulation && (++simulationStages > 64 || (iterations += stack.iterations) > 1024))
                issues.push_back({"Simulation Stages exceed 64 stages or 1024 total iterations",emitter.settings.id,stack.id});
            for (const auto& module : stack.modules)
            {
                if (!unique(module.id) || module.script.empty() || module.inputs.size() > 256)
                    issues.push_back({"Invalid module",emitter.settings.id,stack.id,module.id});
                try { (void)Detail::DecodeReference(Detail::EncodeReference(module.scriptAsset)); }
                catch (const std::exception& exception) { issues.push_back({exception.what(),emitter.settings.id,stack.id,module.id}); }
                for (const auto& [name,input] : module.inputs)
                {
                    if (name.empty() || static_cast<uint32_t>(input.mode) > 4 || !ValidValue(input.value,input.type) ||
                        (input.mode == InputMode::Parameter && input.parameter.empty()))
                        issues.push_back({"Invalid module input",emitter.settings.id,stack.id,module.id,name});
                    if (input.mode == InputMode::Script) { std::string error; if (!Detail::ValidateInput(input,error)) issues.push_back({error,emitter.settings.id,stack.id,module.id,name}); }
                    if (input.mode == InputMode::DynamicInput)
                    {
                        try
                        {
                            (void)DecodeGraph(EncodeGraph(input.expression,true));
                            if (!unique(input.expression.id)) throw std::runtime_error("Duplicate dynamic input graph identity");
                        }
                        catch (const std::exception& exception) { issues.push_back({exception.what(),emitter.settings.id,stack.id,module.id,name}); }
                    }
                    if (input.mode == InputMode::Curve)
                    {
                        if (input.curve.empty() || input.curve.size() > 256) issues.push_back({"Curve requires 1 to 256 keys",emitter.settings.id,stack.id,module.id,name});
                        for (size_t i = 0; i < input.curve.size(); ++i)
                        {
                            const auto& key = input.curve[i];
                            if (!std::isfinite(key.time) || !Finite(key.value) || !Finite(key.arriveTangent) || !Finite(key.leaveTangent) ||
                                static_cast<uint32_t>(key.interpolation) > 1 || (i && key.time-input.curve[i-1].time < 1e-5f))
                                issues.push_back({"Invalid curve key or key order",emitter.settings.id,stack.id,module.id,name});
                        }
                    }
                }
            }
        }
    }
    return issues.empty();
}
std::string Serialize(const System& system, bool editorData)
{
    SystemAsset runtime; runtime.id = system.id; runtime.name = system.name;
    runtime.timing = system.timing; runtime.completion = system.completion;
    runtime.systemAttributes = system.systemAttributes;
    runtime.systemSpawn = system.systemSpawn;
    runtime.systemUpdate = system.systemUpdate;
    for (const auto& emitter : system.emitters) runtime.emitters.push_back(emitter.settings);
    const auto version = HasStageControls(system) ? (std::max)(system.version,kStageControlVersion) : system.version;
    Json result{{"version",version},{"kind","System"},{"id",system.id},{"name",system.name},
        {"runtime",Json::parse(SerializeSystem(runtime,editorData))},{"parameters",Json::array()},{"emitters",Json::array()}};
    for (const auto& parameter : system.parameters)
        result["parameters"].push_back({{"id",parameter.id},{"name",parameter.name},{"type",static_cast<uint32_t>(parameter.type)},{"value",NLS::VFX::Detail::EncodeValue(parameter.value,parameter.type)}});
    for (const auto& emitter : system.emitters)
    {
        Json entry{{"id",emitter.settings.id},{"enabled",emitter.enabled},{"stacks",Json::array()}};
        if (!emitter.parent.Empty()) entry["parent"] = Detail::EncodeReference(emitter.parent);
        if (!emitter.settingsOverrides.empty()) entry["settingsOverrides"] = emitter.settingsOverrides;
        if (!emitter.stackOverrides.empty())
        {
            entry["stackOverrides"] = Json::array();
            for (const auto& item : emitter.stackOverrides)
            {
                Json value{{"stack",item.stack}};
                if (item.enabled) value["enabled"] = *item.enabled;
                if (item.iterations) value["iterations"] = *item.iterations;
                if (item.name) value["name"] = *item.name;
                if (item.bindings) value["bindings"] = Detail::EncodeStageBindings(*item.bindings);
                entry["stackOverrides"].push_back(std::move(value));
            }
        }
        if (!emitter.overrides.empty())
        {
            entry["overrides"] = Json::array();
            for (const auto& item : emitter.overrides)
            {
                Json value{{"module",item.module},{"inputs",Json::object()}};
                if (item.enabled) value["enabled"] = *item.enabled;
                for (const auto& [name,input] : item.inputs) value["inputs"][name] = Detail::EncodeInput(input,editorData);
                entry["overrides"].push_back(std::move(value));
            }
        }
        if (editorData) entry["position"] = emitter.position;
        for (const auto& stack : emitter.stacks)
        {
            Json stackJson{{"id",stack.id},{"stage",static_cast<uint32_t>(stack.stage)},{"name",stack.name},
                {"enabled",stack.enabled},{"iterations",stack.iterations},{"modules",Json::array()}};
            if (!stack.bindings.enabled.empty() || !stack.bindings.iterations.empty()) stackJson["bindings"] = Detail::EncodeStageBindings(stack.bindings);
            for (const auto& module : stack.modules)
            {
                Json moduleJson{{"id",module.id},{"script",module.script},{"enabled",module.enabled},{"inputs",Json::object()}};
                if (!module.scriptAsset.Empty()) moduleJson["scriptAsset"] = Detail::EncodeReference(module.scriptAsset);
                for (const auto& [name,input] : module.inputs)
                {
                    Json inputJson = Detail::EncodeInput(input,editorData);
                    if (input.mode == InputMode::Script) inputJson = Detail::EncodeInput(input,editorData);
                    if (input.mode == InputMode::DynamicInput) inputJson["expression"] = EncodeGraph(input.expression,editorData);
                    if (input.mode == InputMode::Curve)
                    {
                        inputJson["curve"] = Json::array();
                        for (const auto& key : input.curve) inputJson["curve"].push_back({{"time",key.time},{"value",key.value},
                            {"arrive",key.arriveTangent},{"leave",key.leaveTangent},{"interpolation",static_cast<uint32_t>(key.interpolation)}});
                    }
                    moduleJson["inputs"][name] = std::move(inputJson);
                }
                stackJson["modules"].push_back(std::move(moduleJson));
            }
            entry["stacks"].push_back(std::move(stackJson));
        }
        result["emitters"].push_back(std::move(entry));
    }
    if (Detail::HasTypedData(result)) result["version"] = (std::max)(result["version"].get<uint32_t>(),4u);
    if (system.timing != SystemTimingSettings{}) result["version"] = 5;
    // Completion settings are additive: they keep the runtime format valid on
    // their own but still require the v5 source layout that carries them.
    if (system.completion.enabled) result["version"] = (std::max)(result["version"].get<uint32_t>(),5u);
    // Comments and groups are editor-only annotations; they are omitted from
    // SemanticSource() and from runtime artifacts.
    if (editorData && !system.comments.empty())
    {
        result["comments"] = Json::array();
        for (const auto& comment : system.comments)
            result["comments"].push_back({{"id",comment.id},{"text",comment.text},
                {"position",comment.position},{"size",comment.size},{"color",comment.color}});
    }
    if (editorData && !system.groups.empty())
    {
        result["groups"] = Json::array();
        for (const auto& group : system.groups)
            result["groups"].push_back({{"id",group.id},{"title",group.title},{"emitters",group.emitters}});
    }
    return result.dump(2);
}
std::string SemanticSource(const System& system) { return Serialize(system,false); }
bool Deserialize(std::string_view text, System& system, std::string& error)
{
    if (text.size() > 64u*1024u*1024u) { error = "VFX source exceeds 64 MiB"; return false; }
    try
    {
        const auto source = Json::parse(text);
        const auto version = source.at("version").get<uint32_t>();
        if (version < kSchemaVersion || version > 5 || source.value("kind",std::string{}) != "System")
            throw std::runtime_error("Particle Editor requires a module-stack System asset (v2-v5). Legacy graph assets cannot be edited; the original file was not modified.");
        if (version < 4 && Detail::HasTypedData(source)) throw std::runtime_error("Typed values require source format v4");
        if (source.at("kind") != "System") throw std::runtime_error("Expected a System asset");
        System candidate; candidate.version = version; candidate.id = source.at("id").get<std::string>(); candidate.name = source.at("name").get<std::string>();
        SystemAsset runtime;
        if (!DeserializeSystem(source.at("runtime").dump(),runtime,error)) return false;
        if (version < 5 && source.at("runtime").contains("timing")) throw std::runtime_error("System timing requires source format v5");
        candidate.timing = runtime.timing; candidate.completion = runtime.completion;
        candidate.systemAttributes = runtime.systemAttributes;
        candidate.systemSpawn = runtime.systemSpawn;
        candidate.systemUpdate = runtime.systemUpdate;
        if (version < 5 && runtime.completion.enabled) throw std::runtime_error("System completion requires source format v5");
        const auto& emitters = source.at("emitters"); const auto& parameters = source.at("parameters");
        if (runtime.id != candidate.id || runtime.name != candidate.name || !emitters.is_array() || emitters.size() != runtime.emitters.size() || !parameters.is_array() || parameters.size() > 4096)
            throw std::runtime_error("Inconsistent v2 system data");
        for (const auto& entry : parameters)
        {
            const auto type = entry.at("type").get<uint32_t>();
            if (type > static_cast<uint32_t>(ValueType::Bool)) throw std::runtime_error("Unsupported parameter type");
            candidate.parameters.push_back({entry.at("id").get<std::string>(),entry.at("name").get<std::string>(),static_cast<ValueType>(type),NLS::VFX::Detail::DecodeValue(entry.at("value"),static_cast<ValueType>(type))});
        }
        for (size_t i = 0; i < emitters.size(); ++i)
        {
            const auto& entry = emitters[i]; Emitter emitter;
            emitter.settings = std::move(runtime.emitters[i]);
            if (entry.at("id").get<std::string>() != emitter.settings.id) throw std::runtime_error("Emitter settings identity mismatch");
            emitter.enabled = entry.at("enabled").get<bool>(); emitter.position = entry.value("position",std::array<float,2>{});
            if (entry.contains("parent")) emitter.parent = Detail::DecodeReference(entry.at("parent"));
            emitter.settingsOverrides = entry.value("settingsOverrides",std::vector<std::string>{});
            if (entry.contains("stackOverrides"))
            {
                const auto& values = entry.at("stackOverrides");
                if (!values.is_array() || values.size() > 64) throw std::runtime_error("Invalid inherited stage override list");
                for (const auto& value : values)
                {
                    Emitter::StackOverride item; item.stack = value.at("stack").get<std::string>();
                    if (value.contains("enabled")) item.enabled = value.at("enabled").get<bool>();
                    if (value.contains("iterations")) item.iterations = value.at("iterations").get<uint32_t>();
                    if (value.contains("name")) item.name = value.at("name").get<std::string>();
                    if (value.contains("bindings")) item.bindings = Detail::DecodeStageBindings(value.at("bindings"));
                    emitter.stackOverrides.push_back(std::move(item));
                }
            }
            if (entry.contains("overrides"))
            {
                const auto& values = entry.at("overrides");
                if (!values.is_array() || values.size() > 4096) throw std::runtime_error("Invalid inherited module override list");
                for (const auto& value : values)
                {
                    Emitter::ModuleOverride item; item.module = value.at("module").get<std::string>();
                    if (value.contains("enabled")) item.enabled = value.at("enabled").get<bool>();
                    if (!value.at("inputs").is_object() || value.at("inputs").size() > 256) throw std::runtime_error("Invalid override input list");
                    for (const auto& [name,input] : value.at("inputs").items()) item.inputs[name] = Detail::DecodeInput(input);
                    emitter.overrides.push_back(std::move(item));
                }
            }
            const auto& stacks = entry.at("stacks");
            if (!stacks.is_array() || stacks.size() > 128) throw std::runtime_error("Invalid stack list");
            for (const auto& stackJson : stacks)
            {
                Stack stack; stack.id = stackJson.at("id").get<std::string>(); stack.name = stackJson.at("name").get<std::string>();
                const auto stage = stackJson.at("stage").get<uint32_t>();
                if (stage > 7) throw std::runtime_error("Unknown stack stage");
                stack.stage = static_cast<Stage>(stage); stack.enabled = stackJson.at("enabled").get<bool>(); stack.iterations = stackJson.at("iterations").get<uint32_t>();
                if (stackJson.contains("bindings")) stack.bindings = Detail::DecodeStageBindings(stackJson.at("bindings"));
                const auto& modules = stackJson.at("modules");
                if (!modules.is_array() || modules.size() > 4096) throw std::runtime_error("Invalid module list");
                for (const auto& moduleJson : modules)
                {
                    Module module; module.id = moduleJson.at("id").get<std::string>(); module.script = moduleJson.at("script").get<std::string>();
                    module.enabled = moduleJson.at("enabled").get<bool>();
                    if (moduleJson.contains("scriptAsset")) module.scriptAsset = Detail::DecodeReference(moduleJson.at("scriptAsset"));
                    if (!moduleJson.at("inputs").is_object() || moduleJson.at("inputs").size() > 256) throw std::runtime_error("Invalid input list");
                    for (const auto& [name,inputJson] : moduleJson.at("inputs").items())
                    {
                        Input input = Detail::DecodeInput(inputJson);
                        module.inputs.emplace(name,std::move(input));
                    }
                    stack.modules.push_back(std::move(module));
                }
                emitter.stacks.push_back(std::move(stack));
            }
            candidate.emitters.push_back(std::move(emitter));
        }
        if (source.contains("comments"))
        {
            const auto& comments = source.at("comments");
            if (!comments.is_array() || comments.size() > 256) throw std::runtime_error("Invalid comment list");
            for (const auto& entry : comments)
            {
                Comment comment;
                comment.id = entry.value("id",Guid::New().ToString());
                comment.text = entry.value("text",std::string{});
                if (comment.text.size() > 4096) throw std::runtime_error("Comment text is too long");
                comment.position = entry.value("position",std::array<float,2>{});
                comment.size = entry.value("size",std::array<float,2>{220,120});
                comment.color = entry.value("color",comment.color);
                candidate.comments.push_back(std::move(comment));
            }
        }
        if (source.contains("groups"))
        {
            const auto& groups = source.at("groups");
            if (!groups.is_array() || groups.size() > 256) throw std::runtime_error("Invalid group list");
            for (const auto& entry : groups)
            {
                Group group;
                group.id = entry.value("id",Guid::New().ToString());
                group.title = entry.value("title",std::string{});
                if (group.title.size() > 256) throw std::runtime_error("Group title is too long");
                group.emitters = entry.value("emitters",std::vector<std::string>{});
                candidate.groups.push_back(std::move(group));
            }
        }
        if (candidate.version < kStageControlVersion && HasStageControls(candidate)) throw std::runtime_error("Simulation Stage controls require source format v3");
        std::vector<Issue> issues;
        if (!Validate(candidate,issues)) throw std::runtime_error(issues.front().message);
        system = std::move(candidate); error.clear(); return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool Load(const std::filesystem::path& path, System& system, std::string& error)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) { error = "Cannot open VFX asset: "+path.string(); return false; }
    const auto length = file.tellg();
    if (length < 0 || length > 64u*1024u*1024u) { error = "Invalid VFX file size"; return false; }
    std::string source(static_cast<size_t>(length),'\0'); file.seekg(0); file.read(source.data(),static_cast<std::streamsize>(source.size()));
    if (!file) { error = "Cannot read complete VFX asset"; return false; }
    return Deserialize(source,system,error);
}
bool IsSystemSource(std::string_view text)
{
    if (text.size() > 64u*1024u*1024u) return false;
    try { const auto source = Json::parse(text); const auto version = source.value("version",0u); return version >= kSchemaVersion && version <= 5 && source.value("kind",std::string{}) == "System"; }
    catch (const std::exception&) { return false; }
}
bool Save(const std::filesystem::path& path, const System& system, std::string& error)
{
    std::filesystem::path temporary;
    try
    {
        if (std::filesystem::exists(path))
        {
            std::ifstream previous(path,std::ios::binary); uint32_t version = 0; std::string kind;
            try { Json header; previous >> header; version = header.at("version").get<uint32_t>(); kind = header.value("kind",std::string{}); } catch (...) {}
            if (version < kSchemaVersion || version > 5 || kind != "System") { error = "Choose a new file: saving v2-v5 over a legacy or different asset kind is not allowed"; return false; }
        }
        const auto source = Serialize(system); System validated;
        if (!Deserialize(source,validated,error)) return false;
        temporary = path; temporary += "."+Guid::New().ToString()+".tmp";
        { std::ofstream file(temporary,std::ios::binary|std::ios::trunc); file.write(source.data(),static_cast<std::streamsize>(source.size())); file.close();
          if (!file) throw std::runtime_error("Cannot write VFX asset"); }
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace VFX asset: "+std::system_category().message(GetLastError()));
#else
        std::filesystem::rename(temporary,path);
#endif
        error.clear(); return true;
    }
    catch (const std::exception& exception)
    {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary,ignored); }
        error = exception.what(); return false;
    }
}
bool MoveModule(System& system, std::string_view id, std::string_view destination, size_t index, std::string& error,const SourceLibrary* library)
{
    Stack *source = nullptr, *target = nullptr; size_t from = 0;
    for (auto& emitter : system.emitters) for (auto& stack : emitter.stacks)
    {
        if (stack.id == destination) target = &stack;
        for (size_t i = 0; i < stack.modules.size(); ++i) if (stack.modules[i].id == id) { source = &stack; from = i; }
    }
    if (!source || !target || index > target->modules.size()) { error = "Module or destination stack is missing"; return false; }
    const auto& item = source->modules[from]; const auto* definition = FindModule(item.script);
    bool compatible = item.scriptAsset.Empty() && definition && SupportsStage(*definition,target->stage);
    if (!item.scriptAsset.Empty() && library)
        if (const auto* asset = library->Script(item.scriptAsset.asset)) if (const auto* version = SelectVersion(*asset,item.scriptAsset.version))
            compatible = asset->kind == AssetKind::Module && std::find(version->stages.begin(),version->stages.end(),target->stage) != version->stages.end();
    if (!compatible) { error = "Module is incompatible with the destination stage"; return false; }
    auto module = std::move(source->modules[from]); source->modules.erase(source->modules.begin()+from);
    for (auto& [name,input] : module.inputs) if (input.mode == InputMode::DynamicInput)
        input.expression.stage = target->stage == Stage::Simulation ? ScriptStage::SimulationStage : target->stage == Stage::ParticleSpawn ? ScriptStage::ParticleSpawn : ScriptStage::ParticleUpdate;
    if (source == target && from < index) --index;
    target->modules.insert(target->modules.begin()+index,std::move(module)); error.clear(); return true;
}
std::string DuplicateModule(System& system, std::string_view id, std::string& error)
{
    for (auto& emitter : system.emitters) for (auto& stack : emitter.stacks) for (size_t i = 0; i < stack.modules.size(); ++i)
        if (stack.modules[i].id == id)
        {
            auto copy = stack.modules[i]; copy.id = Guid::New().ToString();
            for (auto& [name,input] : copy.inputs) if (input.mode == InputMode::DynamicInput) input.expression.id = Guid::New().ToString();
            const auto result = copy.id; stack.modules.insert(stack.modules.begin()+i+1,std::move(copy)); error.clear(); return result;
        }
    error = "Module is missing"; return {};
}
}
