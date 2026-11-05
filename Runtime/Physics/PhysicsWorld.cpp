#include "PhysicsWorld.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <map>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <type_traits>

#include <PxPhysicsAPI.h>
#include <extensions/PxCustomGeometryExt.h>
#include <geometry/PxConvexCoreGeometry.h>
#include <TaskGraph/TaskGraph.h>

namespace NLS::Physics
{
namespace
{
using namespace physx;
PxVec3 ToNative(Maths::Vector3 vector) { return {vector.x,vector.y,vector.z}; }
Maths::Vector3 FromNative(PxVec3 vector) { return {vector.x,vector.y,vector.z}; }
bool Finite(Maths::Vector3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
bool ValidRotation(Maths::Quaternion value)
{
    const auto length = value.x*value.x+value.y*value.y+value.z*value.z+value.w*value.w;
    return std::isfinite(length) && length > 1e-12f;
}
bool ValidMassProperties(const BodySettings& settings)
{
    return Finite(settings.centerOfMass) && ValidRotation(settings.massFrameRotation) && Finite(settings.inertiaTensor) &&
        settings.inertiaTensor.x>0 && settings.inertiaTensor.y>0 && settings.inertiaTensor.z>0 &&
        std::isfinite(1.0f/settings.inertiaTensor.x) && std::isfinite(1.0f/settings.inertiaTensor.y) && std::isfinite(1.0f/settings.inertiaTensor.z) &&
        std::isfinite(settings.sleepThreshold) && settings.sleepThreshold>=0 && settings.sleepThreshold<std::numeric_limits<float>::max();
}
PxTransform Pose(Maths::Vector3 position, Maths::Quaternion rotation)
{
    auto quaternion = PxQuat(rotation.x,rotation.y,rotation.z,rotation.w);
    quaternion.normalize();
    return {ToNative(position),quaternion};
}
bool ValidRay(Maths::Vector3 origin, Maths::Vector3 direction, float distance)
{
    return Finite(origin) && Finite(direction) && std::isfinite(direction.LengthSquared()) && direction.LengthSquared() > 1e-12f &&
        std::isfinite(distance) && distance > 0;
}
bool ValidQueryBounds(const PxGeometry& geometry, const PxTransform& pose)
{
    PxBounds3 bounds; PxGeometryQuery::computeGeomBounds(bounds,geometry,pose);
    const auto size=bounds.maximum-bounds.minimum;
    return bounds.minimum.isFinite() && bounds.maximum.isFinite() && size.isFinite() && size.x>0 && size.y>0 && size.z>0;
}
PxHitFlags SweepFlags(const PxGeometry& geometry, bool precise)
{
    PxHitFlags flags=PxHitFlag::eDEFAULT;
    const auto type=geometry.getType();
    if (precise && (type==PxGeometryType::eBOX || type==PxGeometryType::eSPHERE || type==PxGeometryType::eCAPSULE)) flags|=PxHitFlag::ePRECISE_SWEEP;
    return flags;
}

class EngineDispatcher final : public PxCpuDispatcher
{
public:
    std::shared_ptr<Base::TaskGraph::TaskGraph> graph;
    void submitTask(PxBaseTask& task) override
    {
        if (!graph) { task.run(); task.release(); return; }
        Base::TaskGraph::TaskDesc desc;
        desc.function = [&task] { task.run(); task.release(); };
        desc.debugName = "Physics3D"; desc.cancellable = false;
        const auto submission = graph->SubmitTracked(std::move(desc));
        if (!submission.handle.IsValid()) { task.run(); task.release(); return; }
        std::lock_guard lock(m_mutex);
        m_completions.push_back(submission.completion);
    }
    PxU32 getWorkerCount() const override { return graph ? graph->GetWorkerCount() : 0; }
    void Wait()
    {
        for (;;)
        {
            std::vector<Base::TaskGraph::TaskCompletion> completions;
            { std::lock_guard lock(m_mutex); completions.swap(m_completions); }
            if (completions.empty()) break;
            for (auto& completion : completions) (void)completion.Wait();
        }
    }
private:
    std::mutex m_mutex;
    std::vector<Base::TaskGraph::TaskCompletion> m_completions;
};

struct Sdk
{
    PxDefaultAllocator allocator;
    PxDefaultErrorCallback errors;
    PxFoundation* foundation = nullptr;
    PxPhysics* physics = nullptr;
    Sdk()
    {
        foundation = PxCreateFoundation(PX_PHYSICS_VERSION,allocator,errors);
        if (!foundation) throw std::runtime_error("PhysX foundation initialization failed");
        physics = PxCreatePhysics(PX_PHYSICS_VERSION,*foundation,PxTolerancesScale(),false,nullptr);
        if (!physics) { foundation->release(); foundation = nullptr; throw std::runtime_error("PhysX SDK initialization failed"); }
    }
    ~Sdk()
    {
        if (physics) physics->release();
        if (foundation) foundation->release();
    }
};

std::shared_ptr<Sdk> AcquireSdk()
{
    static const auto sdk = std::make_shared<Sdk>();
    return sdk;
}

struct BodyRecord
{
    BodyHandle handle;
    PxRigidActor* actor = nullptr;
    BodySettings settings;
    std::vector<ShapeHandle> shapes;
    uint64_t poseRevision = 1;
};

struct ShapeRecord
{
    ShapeHandle handle;
    BodyHandle body;
    PxShape* shape = nullptr; // The actor owns the exclusive native shape.
    ShapeSettings settings;
    std::vector<uint32_t> faceRemap;
    std::shared_ptr<void> geometryOwner;
    const PxCustomGeometryExt::CylinderCallbacks* cylinderCallbacks = nullptr; // Retained by geometryOwner.
};

class Filter final : public PxQueryFilterCallback
{
public:
    explicit Filter(const QueryFilter& filter, bool all = false, BodyHandle source = {}) : m_filter(filter),m_all(all),m_source(source) {}
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor, PxHitFlags&) override
    {
        const auto* body = static_cast<const BodyRecord*>(actor->userData);
        const auto* record = static_cast<const ShapeRecord*>(shape->userData);
        if (!body || !record || body->handle==m_source || !(record->settings.layer & m_filter.layers) || (record->settings.trigger && !m_filter.includeTriggers) ||
            std::find(m_filter.ignored.begin(),m_filter.ignored.end(),body->handle) != m_filter.ignored.end()) return PxQueryHitType::eNONE;
        return m_all ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override
    { return m_all ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK; }
private:
    const QueryFilter& m_filter;
    bool m_all;
    BodyHandle m_source;
};

PxQueryFilterData QueryFlags(const QueryFilter& filter)
{
    PxQueryFlags flags = PxQueryFlag::ePREFILTER;
    if (filter.includeStatic) flags |= PxQueryFlag::eSTATIC;
    if (filter.includeDynamic) flags |= PxQueryFlag::eDYNAMIC;
    return PxQueryFilterData(flags);
}

template<class Hit>
QueryHit Convert(const Hit& native)
{
    QueryHit hit;
    const auto* body = static_cast<const BodyRecord*>(native.actor->userData);
    const auto* shape = static_cast<const ShapeRecord*>(native.shape->userData);
    if (body) hit.body = body->handle;
    if (shape) { hit.shape = shape->handle; hit.owner = shape->settings.owner; hit.material = shape->settings.material; hit.subShapeId=shape->settings.subShapeId; }
    hit.distance = native.distance; hit.faceIndex = native.faceIndex;
    if (shape && hit.faceIndex < shape->faceRemap.size()) hit.faceIndex = shape->faceRemap[hit.faceIndex];
    if constexpr (std::is_same_v<Hit,PxSweepHit>) hit.initialOverlap=native.hadInitialOverlap();
    if (!hit.initialOverlap && native.flags.isSet(PxHitFlag::ePOSITION)) hit.position=FromNative(native.position);
    if (native.flags.isSet(PxHitFlag::eNORMAL)) hit.normal=FromNative(native.normal);
    hit.hasPointAndNormal = !hit.initialOverlap && native.flags.isSet(PxHitFlag::ePOSITION) && native.flags.isSet(PxHitFlag::eNORMAL) &&
        hit.normal.LengthSquared() > 0.5f;
    return hit;
}

QueryHit Convert(const PxOverlapHit& native)
{
    QueryHit hit;
    if (const auto* record=static_cast<const ShapeRecord*>(native.shape->userData))
    { hit.body=record->body; hit.shape=record->handle; hit.owner=record->settings.owner; hit.material=record->settings.material; hit.subShapeId=record->settings.subShapeId; }
    return hit;
}

template<class Hit, class Sink>
struct QueryCollector final : PxHitCallback<Hit>
{
    Hit storage[64];
    Sink& sink;
    explicit QueryCollector(Sink& value) : PxHitCallback<Hit>(storage,64),sink(value) {}
    PxAgain processTouches(const Hit* values, PxU32 count) override
    { for (PxU32 i=0;i<count;++i) sink.Add(Convert(values[i])); return true; }
    void Finish() { for (PxU32 i=0;i<this->nbTouches;++i) sink.Add(Convert(this->touches[i])); }
};
struct VectorQueryResults
{
    std::vector<QueryHit> hits;
    void Add(const QueryHit& hit) { hits.push_back(hit); }
    void Sort()
    {
        std::sort(hits.begin(),hits.end(),[](const auto& a,const auto& b)
        { return a.distance<b.distance || (a.distance==b.distance && a.shape<b.shape); });
    }
};
}

namespace
{
constexpr uint32_t kCollisionElementLimit=1u<<20;
constexpr uint32_t kCollisionByteLimit=64u<<20;
bool ValidBakeScale(Maths::Vector3 scale)
{
    return Finite(scale) && std::abs(scale.x)>=1e-6f && std::abs(scale.y)>=1e-6f && std::abs(scale.z)>=1e-6f &&
        std::abs(scale.x)<=1e6f && std::abs(scale.y)<=1e6f && std::abs(scale.z)<=1e6f;
}
uint64_t CollisionChecksum(std::span<const uint8_t> bytes)
{
    uint64_t hash=14695981039346656037ull;
    for (auto byte:bytes) { hash^=byte; hash*=1099511628211ull; }
    return hash;
}
void CollisionU32(std::vector<uint8_t>& bytes,uint32_t value)
{ for(unsigned i=0;i<4;++i) bytes.push_back(static_cast<uint8_t>(value>>(8*i))); }
void CollisionFloat(std::vector<uint8_t>& bytes,float value) { CollisionU32(bytes,std::bit_cast<uint32_t>(value)); }
struct CollisionReader
{
    std::span<const uint8_t> bytes; size_t cursor=0;
    uint32_t U32()
    {
        if (bytes.size()-cursor<4) throw std::runtime_error("Truncated collision artifact.");
        uint32_t value=0; for(unsigned i=0;i<4;++i) value|=uint32_t(bytes[cursor++])<<(8*i); return value;
    }
    float Float() { return std::bit_cast<float>(U32()); }
};
}

struct CookedCollisionMesh::Impl
{
    CollisionMeshKind kind=CollisionMeshKind::Convex;
    Maths::Vector3 bakeScale{1,1,1};
    float heightScale=1,rowSpacing=1,columnSpacing=1,weldTolerance=0.001f;
    uint32_t convexVertexLimit=255; bool weldVertices=false;
    std::vector<uint8_t> cooked;
    mutable std::mutex nativeMutex;
    mutable std::shared_ptr<void> nativeResource;
};
CookedCollisionMesh::CookedCollisionMesh(std::shared_ptr<Impl> impl):m_impl(std::move(impl)) {}
CookedCollisionMesh::~CookedCollisionMesh()=default;
CollisionMeshKind CookedCollisionMesh::GetKind() const { return m_impl->kind; }
Maths::Vector3 CookedCollisionMesh::GetBakedScale() const { return m_impl->bakeScale; }
size_t CookedCollisionMesh::GetCookedByteCount() const { return m_impl->cooked.size(); }
std::string CookedCollisionMesh::CompatibilityTag()
{
    std::string tag="physx:"+std::to_string(PX_PHYSICS_VERSION)+":collision1:";
#if defined(_WIN32)
    tag+="windows";
#elif defined(__APPLE__)
    tag+="macos";
#elif defined(__linux__)
    tag+="linux";
#else
    tag+="unknown";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
    tag+="-arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    tag+="-x64";
#else
    tag+="-other";
#endif
    tag+=std::endian::native==std::endian::little ? "-le" : "-be";
    return tag;
}
std::shared_ptr<const CookedCollisionMesh> CookedCollisionMesh::Cook(const CollisionMeshSource& source,std::string& error)
{
    const auto fail=[&](const char* message)->std::shared_ptr<const CookedCollisionMesh> { error=message; return {}; };
    if (source.kind>CollisionMeshKind::HeightField || !ValidBakeScale(source.bakeScale) ||
        !std::isfinite(source.weldTolerance) || source.weldTolerance<0 || source.weldTolerance>1000 ||
        (source.weldVertices && source.weldTolerance==0) || source.convexVertexLimit<8 || source.convexVertexLimit>255)
        return fail("Invalid collision cooking parameters.");
    auto data=std::make_shared<Impl>(); data->kind=source.kind; data->bakeScale=source.bakeScale;
    data->weldTolerance=source.weldTolerance; data->weldVertices=source.weldVertices; data->convexVertexLimit=source.convexVertexLimit;
    std::vector<PxVec3> points;
    std::vector<uint32_t> indices;
    std::vector<PxHeightFieldSample> samples;
    if (source.kind==CollisionMeshKind::HeightField)
    {
        if (!source.vertices.empty() || !source.indices.empty() || source.rows<2 || source.columns<2 ||
            uint64_t(source.rows)*source.columns!=source.heights.size() || source.heights.size()>kCollisionElementLimit ||
            source.bakeScale.x<=0 || source.bakeScale.y<=0 || source.bakeScale.z<=0 ||
            !std::isfinite(source.rowSpacing) || source.rowSpacing<1e-4f || !std::isfinite(source.columnSpacing) || source.columnSpacing<1e-4f)
            return fail("Heightfield requires a finite positive grid and positive bake scale.");
        data->rowSpacing=source.rowSpacing*source.bakeScale.x; data->columnSpacing=source.columnSpacing*source.bakeScale.z;
        if (!std::isfinite(data->rowSpacing) || !std::isfinite(data->columnSpacing) ||
            data->rowSpacing<1e-4f || data->columnSpacing<1e-4f ||
            double(data->rowSpacing)*source.rows>1e7 || double(data->columnSpacing)*source.columns>1e7)
            return fail("Heightfield grid dimensions exceed the supported bounds.");
        float maximum=0;
        for (float height:source.heights)
        {
            const float scaled=height*source.bakeScale.y;
            if (!std::isfinite(scaled) || std::abs(scaled)>1e7f) return fail("Heightfield contains an invalid height.");
            maximum=(std::max)(maximum,std::abs(scaled));
        }
        data->heightScale=(std::max)(maximum/32766.0f,1e-4f); samples.resize(source.heights.size());
        for (size_t i=0;i<samples.size();++i)
        {
            samples[i].height=static_cast<PxI16>(std::lround(source.heights[i]*source.bakeScale.y/data->heightScale));
            samples[i].materialIndex0=0; samples[i].materialIndex1=0; samples[i].clearTessFlag();
        }
    }
    else
    {
        if (!source.heights.empty() || source.vertices.size()<(source.kind==CollisionMeshKind::Convex?4u:3u) ||
            source.vertices.size()>kCollisionElementLimit || source.indices.size()>3ull*kCollisionElementLimit)
            return fail("Collision mesh has invalid or excessive source geometry.");
        points.reserve(source.vertices.size());
        for (auto vertex:source.vertices)
        {
            const PxVec3 point(vertex.x*source.bakeScale.x,vertex.y*source.bakeScale.y,vertex.z*source.bakeScale.z);
            if (!point.isFinite() || point.abs().maxElement()>1e7f) return fail("Collision mesh contains an invalid scaled vertex.");
            points.push_back(point);
        }
        if (source.kind==CollisionMeshKind::Convex)
        {
            if (!source.indices.empty()) return fail("Convex cooking accepts a point cloud without triangle indices.");
            PxVec3 axis(0),normal(0); float farthest=0,area=0,volume=0;
            for (auto point:points) if ((point-points[0]).magnitudeSquared()>farthest)
            { axis=point-points[0]; farthest=axis.magnitudeSquared(); }
            for (auto point:points) if (axis.cross(point-points[0]).magnitudeSquared()>area)
            { normal=axis.cross(point-points[0]); area=normal.magnitudeSquared(); }
            for (auto point:points) volume=(std::max)(volume,std::abs(normal.dot(point-points[0])));
            if (!std::isfinite(volume) || volume<=1e-10f) return fail("Convex cooking requires a nondegenerate three-dimensional point cloud.");
        }
        else
        {
            if (source.indices.empty() || source.indices.size()%3) return fail("Triangle cooking requires complete indexed triangles.");
            indices=source.indices;
            const bool reflected=double(source.bakeScale.x)*source.bakeScale.y*source.bakeScale.z<0;
            for (size_t i=0;i<indices.size();i+=3)
            {
                if (indices[i]>=points.size() || indices[i+1]>=points.size() || indices[i+2]>=points.size()) return fail("Triangle index is outside the vertex array.");
                const auto normal=(points[indices[i+1]]-points[indices[i]]).cross(points[indices[i+2]]-points[indices[i]]);
                if (!normal.isFinite() || !std::isfinite(normal.magnitudeSquared()) || normal.magnitudeSquared()<=1e-18f)
                    return fail("Triangle source contains a degenerate face.");
                if (reflected) std::swap(indices[i+1],indices[i+2]);
            }
        }
    }
    try
    {
        const auto sdk=AcquireSdk(); PxDefaultMemoryOutputStream output;
        PxCookingParams params(sdk->physics->getTolerancesScale()); params.suppressTriangleMeshRemapTable=false;
        params.meshWeldTolerance=source.weldTolerance;
        if (source.weldVertices) params.meshPreprocessParams|=PxMeshPreprocessingFlag::eWELD_VERTICES;
        bool success=false;
        if (source.kind==CollisionMeshKind::Convex)
        {
            PxConvexMeshDesc desc; desc.points.count=static_cast<PxU32>(points.size());
            desc.points.stride=sizeof(PxVec3); desc.points.data=points.data(); desc.flags=PxConvexFlag::eCOMPUTE_CONVEX;
            desc.vertexLimit=static_cast<PxU16>(source.convexVertexLimit); success=PxCookConvexMesh(params,desc,output);
        }
        else if (source.kind==CollisionMeshKind::Triangle)
        {
            PxTriangleMeshDesc desc; desc.points.count=static_cast<PxU32>(points.size()); desc.points.stride=sizeof(PxVec3); desc.points.data=points.data();
            desc.triangles.count=static_cast<PxU32>(indices.size()/3); desc.triangles.stride=sizeof(uint32_t)*3; desc.triangles.data=indices.data();
            success=PxCookTriangleMesh(params,desc,output);
        }
        else
        {
            PxHeightFieldDesc desc; desc.nbRows=source.rows; desc.nbColumns=source.columns;
            desc.samples.data=samples.data(); desc.samples.stride=sizeof(PxHeightFieldSample); success=PxCookHeightField(desc,output);
        }
        if (!success || output.getSize()==0 || output.getSize()>kCollisionByteLimit) return fail("PhysX collision cooking failed or exceeded the byte limit.");
        data->cooked.assign(output.getData(),output.getData()+output.getSize()); error.clear();
        return std::shared_ptr<const CookedCollisionMesh>(new CookedCollisionMesh(std::move(data)));
    }
    catch (const std::exception& exception) { error=exception.what(); return {}; }
}
std::vector<uint8_t> CookedCollisionMesh::Serialize() const
{
    std::vector<uint8_t> bytes={'N','L','S','C','O','L','L','1'};
    CollisionU32(bytes,1); CollisionU32(bytes,static_cast<uint32_t>(m_impl->kind));
    const auto tag=CompatibilityTag(); CollisionU32(bytes,static_cast<uint32_t>(tag.size())); bytes.insert(bytes.end(),tag.begin(),tag.end());
    for(float value:{m_impl->bakeScale.x,m_impl->bakeScale.y,m_impl->bakeScale.z,m_impl->heightScale,m_impl->rowSpacing,m_impl->columnSpacing,m_impl->weldTolerance}) CollisionFloat(bytes,value);
    CollisionU32(bytes,m_impl->convexVertexLimit); CollisionU32(bytes,m_impl->weldVertices?1:0);
    CollisionU32(bytes,static_cast<uint32_t>(m_impl->cooked.size())); bytes.insert(bytes.end(),m_impl->cooked.begin(),m_impl->cooked.end());
    const auto checksum=CollisionChecksum(bytes); CollisionU32(bytes,static_cast<uint32_t>(checksum)); CollisionU32(bytes,static_cast<uint32_t>(checksum>>32));
    return bytes;
}
std::shared_ptr<const CookedCollisionMesh> CookedCollisionMesh::Deserialize(std::span<const uint8_t> bytes,std::string& error)
{
    try
    {
        if (bytes.size()<80 || bytes.size()>size_t(kCollisionByteLimit)+256 ||
            !std::equal(bytes.begin(),bytes.begin()+8,"NLSCOLL1")) throw std::runtime_error("Invalid collision artifact header or byte count.");
        CollisionReader checksumReader{bytes,bytes.size()-8};
        const uint64_t low=checksumReader.U32(),high=checksumReader.U32();
        if (CollisionChecksum(bytes.first(bytes.size()-8))!=(low|(high<<32))) throw std::runtime_error("Collision artifact checksum mismatch.");
        CollisionReader reader{bytes.first(bytes.size()-8),8};
        if (reader.U32()!=1) throw std::runtime_error("Unsupported collision artifact version.");
        const auto kind=reader.U32(),tagSize=reader.U32();
        if (kind>static_cast<uint32_t>(CollisionMeshKind::HeightField) || tagSize>128 || tagSize>reader.bytes.size()-reader.cursor)
            throw std::runtime_error("Invalid collision artifact kind or compatibility tag.");
        const std::string tag(reinterpret_cast<const char*>(bytes.data()+reader.cursor),tagSize); reader.cursor+=tagSize;
        if (tag!=CompatibilityTag()) throw std::runtime_error("Collision artifact requires another PhysX version, cooking schema or native platform.");
        auto data=std::make_shared<Impl>(); data->kind=static_cast<CollisionMeshKind>(kind);
        data->bakeScale={reader.Float(),reader.Float(),reader.Float()};
        data->heightScale=reader.Float(); data->rowSpacing=reader.Float(); data->columnSpacing=reader.Float(); data->weldTolerance=reader.Float();
        data->convexVertexLimit=reader.U32(); const auto flags=reader.U32(),count=reader.U32();
        if (!ValidBakeScale(data->bakeScale) || !std::isfinite(data->heightScale) || data->heightScale<=0 ||
            !std::isfinite(data->rowSpacing) || data->rowSpacing<1e-4f || !std::isfinite(data->columnSpacing) || data->columnSpacing<1e-4f ||
            !std::isfinite(data->weldTolerance) || data->weldTolerance<0 || data->weldTolerance>1000 || flags>1 ||
            data->convexVertexLimit<8 || data->convexVertexLimit>255 || count<16 || count>kCollisionByteLimit || count!=reader.bytes.size()-reader.cursor)
            throw std::runtime_error("Invalid collision artifact metadata or payload size.");
        data->weldVertices=flags!=0; data->cooked.assign(reader.bytes.begin()+reader.cursor,reader.bytes.end()); error.clear();
        return std::shared_ptr<const CookedCollisionMesh>(new CookedCollisionMesh(std::move(data)));
    }
    catch (const std::exception& exception) { error=exception.what(); return {}; }
}

static PxFilterFlags SimulationFilter(PxFilterObjectAttributes a, PxFilterData first, PxFilterObjectAttributes b,
    PxFilterData second, PxPairFlags& flags, const void*, PxU32)
{
    if (!(first.word0 & second.word1) || !(second.word0 & first.word1)) return PxFilterFlag::eSUPPRESS;
    if (PxFilterObjectIsTrigger(a) || PxFilterObjectIsTrigger(b)) flags = PxPairFlag::eTRIGGER_DEFAULT;
    else flags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eNOTIFY_TOUCH_FOUND | PxPairFlag::eNOTIFY_TOUCH_PERSISTS |
        PxPairFlag::eNOTIFY_TOUCH_LOST | PxPairFlag::eNOTIFY_CONTACT_POINTS | PxPairFlag::eDETECT_CCD_CONTACT;
    return PxFilterFlag::eDEFAULT;
}

struct PhysicsWorld::State : PxSimulationEventCallback
{
    std::shared_ptr<Sdk> sdk;
    EngineDispatcher dispatcher;
    PxScene* scene = nullptr;
    PxMaterial* material = nullptr;
    uint64_t world = 0;
    mutable Detail::WorldAccessMutex mutex;
    std::vector<std::unique_ptr<BodyRecord>> bodies;
    std::vector<std::unique_ptr<ShapeRecord>> shapes;
    std::vector<uint32_t> generations, shapeGenerations, freeBodies, freeShapes;
    PhysicsStatistics statistics;
    uint64_t topologyRevision = 0;
    std::vector<ContactEvent> events, callbackEvents;
    std::mutex callbackMutex;
    struct PairKey
    {
        ShapeHandle first, second;
        auto operator<=>(const PairKey&) const = default;
    };
    struct Identity
    {
        uint64_t owner = 0;
        ShapeHandle fallback;
        auto operator<=>(const Identity&) const = default;
    };
    struct OwnerPair
    {
        Identity first, second;
        bool trigger = false;
        auto operator<=>(const OwnerPair&) const = default;
    };
    std::map<PairKey, ContactEvent> shapeContacts;
    std::map<OwnerPair, ContactEvent> ownerContacts;

    static void Swap(ContactEvent& event)
    {
        std::swap(event.first, event.second); std::swap(event.firstShape, event.secondShape);
        std::swap(event.firstOwner, event.secondOwner);
        std::swap(event.firstSubShapeId,event.secondSubShapeId);
        for (uint32_t i = 0; i < event.pointCount; ++i)
        { event.points[i].normal *= -1; event.points[i].impulse *= -1; }
    }
    static PairKey ShapeKey(ContactEvent& event)
    {
        if (event.secondShape < event.firstShape) Swap(event);
        return {event.firstShape, event.secondShape};
    }
    static OwnerPair AggregateKey(ContactEvent& event)
    {
        Identity a{event.firstOwner, event.firstOwner ? ShapeHandle{} : event.firstShape};
        Identity b{event.secondOwner, event.secondOwner ? ShapeHandle{} : event.secondShape};
        if (b < a) { std::swap(a, b); Swap(event); }
        return {a, b, event.trigger};
    }
    ContactEvent MakeEvent(const PxShape* first, const PxShape* second, ContactPhase phase, bool trigger) const
    {
        ContactEvent event;
        const auto* a = static_cast<const ShapeRecord*>(first->userData);
        const auto* b = static_cast<const ShapeRecord*>(second->userData);
        if (!a || !b) return event;
        event.first = a->body; event.second = b->body; event.phase = phase; event.trigger = trigger;
        event.firstShape = a->handle; event.secondShape = b->handle;
        event.firstOwner = a->settings.owner; event.secondOwner = b->settings.owner;
        event.firstSubShapeId=a->settings.subShapeId; event.secondSubShapeId=b->settings.subShapeId;
        event.stepIndex = statistics.stepIndex;
        return event;
    }
    void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
    void onWake(PxActor**, PxU32) override {}
    void onSleep(PxActor**, PxU32) override {}
    void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}
    void onContact(const PxContactPairHeader& header, const PxContactPair* pairs, PxU32 count) override
    {
        if (header.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1)) return;
        std::lock_guard lock(callbackMutex);
        for (PxU32 i = 0; i < count; ++i)
        {
            const auto& pair = pairs[i];
            if (pair.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1)) continue;
            auto event = MakeEvent(pair.shapes[0], pair.shapes[1], ContactPhase::Stay, false);
            if (!event.firstShape.IsValid() || !event.secondShape.IsValid()) continue;
            PxContactPairPoint points[4];
            event.pointCount = pair.extractContacts(points, 4);
            event.hasContactData = event.pointCount != 0;
            for (uint32_t p = 0; p < event.pointCount; ++p)
                event.points[p] = {FromNative(points[p].position), FromNative(points[p].normal),
                    FromNative(points[p].impulse), points[p].separation};
            if (pair.events & PxPairFlag::eNOTIFY_TOUCH_FOUND)
            { event.phase = ContactPhase::Enter; callbackEvents.push_back(event); }
            if (pair.events & PxPairFlag::eNOTIFY_TOUCH_PERSISTS)
            { event.phase = ContactPhase::Stay; callbackEvents.push_back(event); }
            if (pair.events & PxPairFlag::eNOTIFY_TOUCH_LOST)
            { event.phase = ContactPhase::Exit; event.pointCount = 0; event.hasContactData = false; callbackEvents.push_back(event); }
        }
    }
    void onTrigger(PxTriggerPair* pairs, PxU32 count) override
    {
        std::lock_guard lock(callbackMutex);
        for (PxU32 i = 0; i < count; ++i)
        {
            const auto& pair = pairs[i];
            if (pair.flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER | PxTriggerPairFlag::eREMOVED_SHAPE_OTHER)) continue;
            if (pair.status & PxPairFlag::eNOTIFY_TOUCH_FOUND)
                callbackEvents.push_back(MakeEvent(pair.triggerShape, pair.otherShape, ContactPhase::Enter, true));
            if (pair.status & PxPairFlag::eNOTIFY_TOUCH_LOST)
                callbackEvents.push_back(MakeEvent(pair.triggerShape, pair.otherShape, ContactPhase::Exit, true));
        }
    }
    void CollectEvents()
    {
        std::erase_if(shapeContacts, [&](const auto& entry) { return !Find(entry.first.first) || !Find(entry.first.second); });
        std::map<OwnerPair, ContactEvent> began;
        {
            std::lock_guard lock(callbackMutex);
            for (auto event : callbackEvents)
            {
                const auto key = ShapeKey(event);
                if (event.phase == ContactPhase::Exit) shapeContacts.erase(key);
                else if (Find(key.first) && Find(key.second)) shapeContacts[key] = event;
                if (event.phase == ContactPhase::Enter)
                { auto canonical = event; began[AggregateKey(canonical)] = canonical; }
            }
            callbackEvents.clear();
        }
        std::map<OwnerPair, ContactEvent> current;
        for (const auto& [_, stored] : shapeContacts)
        {
            auto event = stored;
            if (event.stepIndex != statistics.stepIndex) { event.pointCount = 0; event.hasContactData = false; }
            event.stepIndex = statistics.stepIndex;
            const auto key = AggregateKey(event);
            auto [it, inserted] = current.try_emplace(key, event);
            if (!inserted && event.hasContactData && !it->second.hasContactData) it->second = event;
        }
        for (auto& [key, event] : current)
        {
            event.phase = ownerContacts.contains(key) ? ContactPhase::Stay : ContactPhase::Enter;
            events.push_back(event);
        }
        for (const auto& [key, stored] : ownerContacts)
            if (!current.contains(key))
            {
                auto event = stored; event.phase = ContactPhase::Exit; event.stepIndex = statistics.stepIndex;
                event.pointCount = 0; event.hasContactData = false; events.push_back(event);
            }
        for (auto& [key, event] : began)
            if (!current.contains(key) && !ownerContacts.contains(key))
            {
                event.phase = ContactPhase::Enter; events.push_back(event);
                event.phase = ContactPhase::Exit; event.pointCount = 0; event.hasContactData = false; events.push_back(event);
            }
        ownerContacts = std::move(current);
    }
    ~State()
    {
        dispatcher.Wait();
        if (scene) scene->setSimulationEventCallback(nullptr);
        for (const auto& body : bodies) if (body && body->actor) body->actor->release();
        if (scene) scene->release();
        if (material) material->release();
    }
    BodyRecord* Find(BodyHandle handle) const
    {
        if (handle.world != world || handle.index >= bodies.size() || generations[handle.index] != handle.generation) return nullptr;
        return bodies[handle.index].get();
    }
    ShapeRecord* Find(ShapeHandle handle) const
    {
        if (handle.world != world || handle.index >= shapes.size() || shapeGenerations[handle.index] != handle.generation) return nullptr;
        return shapes[handle.index].get();
    }
    void UpdateMass(BodyRecord& body)
    {
        if (auto* dynamic = body.actor->is<PxRigidDynamic>())
        {
            if (!body.settings.autoMassProperties)
            {
                dynamic->setMass(body.settings.mass);
                dynamic->setMassSpaceInertiaTensor(ToNative(body.settings.inertiaTensor));
                dynamic->setCMassLocalPose(Pose(body.settings.centerOfMass,body.settings.massFrameRotation));
            }
            else if (!body.shapes.empty()) PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, body.settings.mass, nullptr, true);
            else
            {
                dynamic->setMass(body.settings.mass); dynamic->setMassSpaceInertiaTensor(PxVec3(body.settings.mass));
                dynamic->setCMassLocalPose(PxTransform(PxIdentity));
            }
        }
    }
    ShapeHandle Attach(BodyHandle bodyHandle, const PxGeometry& geometry, const ShapeSettings& settings,
        std::span<const uint32_t> faceRemap = {}, std::shared_ptr<void> geometryOwner = {},
        const PxCustomGeometryExt::CylinderCallbacks* cylinderCallbacks = nullptr)
    {
        if (!Finite(settings.localPosition) || !ValidRotation(settings.localRotation) ||
            !std::isfinite(settings.staticFriction) || settings.staticFriction < 0 ||
            !std::isfinite(settings.dynamicFriction) || settings.dynamicFriction < 0 ||
            !std::isfinite(settings.restitution) || settings.restitution < 0 || settings.restitution > 1 ||
            settings.frictionCombine>MaterialCombineMode::Maximum || settings.restitutionCombine>MaterialCombineMode::Maximum) return {};
        std::unique_lock lock(mutex);
        auto* body = Find(bodyHandle);
        if (!body || ((geometry.getType() == PxGeometryType::eTRIANGLEMESH || geometry.getType() == PxGeometryType::eHEIGHTFIELD) && body->settings.dynamic) ||
            (settings.trigger && (geometry.getType() == PxGeometryType::eTRIANGLEMESH || geometry.getType() == PxGeometryType::eHEIGHTFIELD))) return {};
        auto* dynamic=body->actor->is<PxRigidDynamic>();
        const bool sleeping=dynamic && !body->settings.kinematic && dynamic->isSleeping();
        auto record = std::make_unique<ShapeRecord>();
        record->body = bodyHandle; record->settings = settings;
        record->geometryOwner = std::move(geometryOwner);
        record->cylinderCallbacks=cylinderCallbacks;
        record->faceRemap.assign(faceRemap.begin(), faceRemap.end());
        PxMaterial* shapeMaterial = material;
        const bool customMaterial = settings.staticFriction != 0.5f || settings.dynamicFriction != 0.5f || settings.restitution != 0.5f ||
            settings.frictionCombine!=MaterialCombineMode::Average || settings.restitutionCombine!=MaterialCombineMode::Average;
        if (customMaterial) shapeMaterial = sdk->physics->createMaterial(settings.staticFriction, settings.dynamicFriction, settings.restitution);
        if (!shapeMaterial) return {};
        if (customMaterial)
        {
            shapeMaterial->setFrictionCombineMode(static_cast<PxCombineMode::Enum>(settings.frictionCombine));
            shapeMaterial->setRestitutionCombineMode(static_cast<PxCombineMode::Enum>(settings.restitutionCombine));
        }
        const auto flags = PxShapeFlag::eSCENE_QUERY_SHAPE |
            (settings.trigger ? PxShapeFlag::eTRIGGER_SHAPE : PxShapeFlag::eSIMULATION_SHAPE);
        auto* shape = sdk->physics->createShape(geometry, *shapeMaterial, true, flags);
        if (customMaterial) shapeMaterial->release();
        if (!shape) return {};
        shape->setLocalPose(Pose(settings.localPosition, settings.localRotation));
        shape->setSimulationFilterData(PxFilterData(settings.layer, settings.collisionMask, 0, 0));
        shape->userData = record.get();
        if (!body->actor->attachShape(*shape)) { shape->release(); return {}; }
        record->shape = shape;
        shape->release();
        uint32_t index;
        if (freeShapes.empty()) { index = static_cast<uint32_t>(shapes.size()); shapes.emplace_back(); shapeGenerations.push_back(1); }
        else { index = freeShapes.back(); freeShapes.pop_back(); }
        record->handle = {world, index, shapeGenerations[index]};
        const auto handle = record->handle;
        shapes[index] = std::move(record); body->shapes.push_back(handle); ++statistics.shapeCount; ++topologyRevision;
        if (++body->poseRevision == 0) ++body->poseRevision;
        UpdateMass(*body);
        if (sleeping) dynamic->putToSleep();
        scene->flushQueryUpdates();
        return handle;
    }
    bool RemoveShape(ShapeHandle handle)
    {
        auto* shape = Find(handle); if (!shape) return false;
        if (auto* body = Find(shape->body))
        {
            auto* dynamic=body->actor->is<PxRigidDynamic>();
            const bool sleeping=dynamic && !body->settings.kinematic && dynamic->isSleeping();
            body->actor->detachShape(*shape->shape);
            std::erase(body->shapes, handle); UpdateMass(*body);
            if (sleeping) dynamic->putToSleep();
            if (++body->poseRevision == 0) ++body->poseRevision;
        }
        shapes[handle.index].reset();
        if (++shapeGenerations[handle.index] == 0) ++shapeGenerations[handle.index];
        freeShapes.push_back(handle.index); --statistics.shapeCount; ++topologyRevision;
        return true;
    }
};

struct PhysicsWorld::ReadAccess::Impl
{
    std::shared_ptr<State> state;
    std::shared_lock<Detail::WorldAccessMutex> lock;
    QueryFilter filter;
    Impl(std::shared_ptr<State> value, QueryFilter queryFilter) : state(std::move(value)),lock(state->mutex),filter(std::move(queryFilter)) {}
    ShapeQueryStatus Geometry(const QueryGeometry3D& input, PxGeometryHolder& geometry, PxQuat& alignment, BodyHandle& source) const
    {
        alignment=PxQuat(PxIdentity); source={};
        switch (input.type)
        {
        case QueryGeometryType3D::Sphere:
            if (!std::isfinite(input.radius) || input.radius<=0) return ShapeQueryStatus::InvalidInput;
            geometry.storeAny(PxSphereGeometry(input.radius)); return ShapeQueryStatus::Success;
        case QueryGeometryType3D::Box:
            if (!Finite(input.halfExtents) || input.halfExtents.x<=0 || input.halfExtents.y<=0 || input.halfExtents.z<=0) return ShapeQueryStatus::InvalidInput;
            geometry.storeAny(PxBoxGeometry(ToNative(input.halfExtents))); return ShapeQueryStatus::Success;
        case QueryGeometryType3D::Capsule:
            if (!std::isfinite(input.radius) || input.radius<=0 || !std::isfinite(input.halfHeight) || input.halfHeight<0) return ShapeQueryStatus::InvalidInput;
            geometry.storeAny(PxCapsuleGeometry(input.radius,input.halfHeight)); alignment=PxQuat(0,0,0.70710678f,0.70710678f); return ShapeQueryStatus::Success;
        case QueryGeometryType3D::Cylinder:
            if (!std::isfinite(input.radius) || input.radius<=0 || !std::isfinite(input.halfHeight) || input.halfHeight<=0 || !std::isfinite(input.halfHeight*2))
                return ShapeQueryStatus::InvalidInput;
            geometry.storeAny(PxConvexCoreGeometry(PxConvexCore::Cylinder(input.halfHeight*2,input.radius)));
            alignment=PxQuat(0,0,0.70710678f,0.70710678f); return ShapeQueryStatus::Success;
        case QueryGeometryType3D::Shape:
        {
            const auto* shape=state->Find(input.shape); if (!shape) return ShapeQueryStatus::InvalidShape;
            const auto& native=shape->shape->getGeometry();
            if (const auto* cylinder=shape->cylinderCallbacks; native.getType()==PxGeometryType::eCUSTOM && cylinder)
            {
                geometry.storeAny(PxConvexCoreGeometry(PxConvexCore::Cylinder(cylinder->getHeight(),cylinder->getRadius()),cylinder->getMargin()));
                if (cylinder->getAxis()==1) alignment=PxQuat(0,0,0.70710678f,0.70710678f);
                else if (cylinder->getAxis()==2) alignment=PxQuat(0,-0.70710678f,0,0.70710678f);
                source=shape->body; return ShapeQueryStatus::Success;
            }
            switch (native.getType())
            {
            case PxGeometryType::eSPHERE: case PxGeometryType::eBOX: case PxGeometryType::eCAPSULE:
            case PxGeometryType::eCONVEXCORE: case PxGeometryType::eCONVEXMESH:
                geometry.storeAny(native); source=shape->body; return ShapeQueryStatus::Success;
            default: return ShapeQueryStatus::UnsupportedGeometry;
            }
        }
        }
        return ShapeQueryStatus::InvalidInput;
    }
};

PhysicsWorld::PhysicsWorld(std::shared_ptr<State> state) : m_state(std::move(state)) {}
PhysicsWorld::~PhysicsWorld() = default;
std::unique_ptr<PhysicsMutationBatch> PhysicsWorld::BeginMutationBatch()
{ return std::unique_ptr<PhysicsMutationBatch>(new PhysicsMutationBatch(m_state, m_state->mutex)); }

std::shared_ptr<PhysicsWorld> PhysicsWorld::Create(std::string& error) { return Create({}, error); }
std::shared_ptr<PhysicsWorld> PhysicsWorld::Create(const WorldSettings& settings, std::string& error)
{
    if (!Finite(settings.gravity)) { error = "Invalid 3D world gravity."; return {}; }
    try
    {
        auto state = std::make_shared<State>();
        state->sdk = AcquireSdk(); state->dispatcher.graph = settings.tasks;
        if (!state->dispatcher.graph && settings.workerCount)
        {
            Base::TaskGraph::TaskGraphConfig config; config.workerCount = (std::min)(settings.workerCount, 31u);
            state->dispatcher.graph = std::make_shared<Base::TaskGraph::TaskGraph>(config);
            if (!state->dispatcher.graph->Start()) throw std::runtime_error("Cannot start the physics task graph.");
        }
        if (state->dispatcher.graph && !state->dispatcher.graph->IsStarted())
            throw std::runtime_error("The supplied physics task graph is not started.");
        PxSceneDesc desc(state->sdk->physics->getTolerancesScale());
        desc.gravity = ToNative(settings.gravity);
        desc.cpuDispatcher = &state->dispatcher;
        desc.filterShader = SimulationFilter;
        desc.simulationEventCallback = state.get();
        desc.flags |= PxSceneFlag::eENABLE_CCD;
        state->scene = state->sdk->physics->createScene(desc);
        state->material = state->sdk->physics->createMaterial(0.5f,0.5f,0.5f);
        if (!state->scene || !state->material) throw std::runtime_error("PhysX scene creation failed");
        static std::atomic<uint64_t> next{1};
        state->world = next.fetch_add(1);
        error.clear();
        return std::shared_ptr<PhysicsWorld>(new PhysicsWorld(std::move(state)));
    }
    catch (const std::exception& exception) { error = exception.what(); return {}; }
}

BodyHandle PhysicsWorld::CreateBody(const BodySettings& settings)
{
    if (!Finite(settings.position) || !ValidRotation(settings.rotation) || !ValidRotation(settings.localRotation) ||
        !Finite(settings.velocity) || !Finite(settings.angularVelocity) || !std::isfinite(settings.mass) || settings.mass <= 0 || !std::isfinite(1.0f/settings.mass) ||
        !std::isfinite(settings.linearDamping) || settings.linearDamping < 0 ||
        !std::isfinite(settings.angularDamping) || settings.angularDamping < 0 || !ValidMassProperties(settings)) return {};
    std::unique_lock lock(m_state->mutex);
    auto record = std::make_unique<BodyRecord>(); record->settings = settings;
    auto* actor = settings.dynamic ? static_cast<PxRigidActor*>(m_state->sdk->physics->createRigidDynamic(Pose(settings.position,settings.rotation)))
                                  : static_cast<PxRigidActor*>(m_state->sdk->physics->createRigidStatic(Pose(settings.position,settings.rotation)));
    if (!actor) return {};
    record->actor=actor;
    if (auto* dynamic = actor->is<PxRigidDynamic>())
    {
        dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, settings.kinematic);
        dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, settings.continuousCollision && !settings.kinematic);
        dynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !settings.useGravity);
        dynamic->setLinearDamping(settings.linearDamping); dynamic->setAngularDamping(settings.angularDamping);
        dynamic->setSleepThreshold(settings.sleepThreshold);
        m_state->UpdateMass(*record);
        PxRigidDynamicLockFlags locks;
        if (settings.lockPositionX) locks |= PxRigidDynamicLockFlag::eLOCK_LINEAR_X;
        if (settings.lockPositionY) locks |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Y;
        if (settings.lockPositionZ) locks |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Z;
        if (settings.lockRotationX) locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
        if (settings.lockRotationY) locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
        if (settings.lockRotationZ) locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;
        dynamic->setRigidDynamicLockFlags(locks);
        if (!settings.kinematic)
        { dynamic->setLinearVelocity(ToNative(settings.velocity)); dynamic->setAngularVelocity(ToNative(settings.angularVelocity)); }
    }
    uint32_t index;
    if (m_state->freeBodies.empty())
    { index = static_cast<uint32_t>(m_state->bodies.size()); m_state->bodies.emplace_back(); m_state->generations.push_back(1); }
    else { index = m_state->freeBodies.back(); m_state->freeBodies.pop_back(); }
    record->handle = {m_state->world, index, m_state->generations[index]};
    actor->userData = record.get();
    const auto handle = record->handle;
    m_state->bodies[index] = std::move(record);
    m_state->scene->addActor(*actor); ++m_state->statistics.bodyCount; ++m_state->topologyRevision;
    if (settings.dynamic && !settings.kinematic && !settings.startAwake) static_cast<PxRigidDynamic*>(actor)->putToSleep();
    return handle;
}
bool PhysicsWorld::UpdateDynamics(BodyHandle handle, const BodySettings& settings)
{
    if (!std::isfinite(settings.mass) || settings.mass<=0 || !std::isfinite(1.0f/settings.mass) || !std::isfinite(settings.linearDamping) || settings.linearDamping<0 ||
        !std::isfinite(settings.angularDamping) || settings.angularDamping<0 || !ValidMassProperties(settings)) return false;
    std::unique_lock lock(m_state->mutex);
    auto* body=m_state->Find(handle); if (!body || body->settings.dynamic!=settings.dynamic) return false;
    if (auto* dynamic=body->actor->is<PxRigidDynamic>())
    {
        dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD,false);
        dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC,settings.kinematic);
        dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD,settings.continuousCollision && !settings.kinematic);
        dynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY,!settings.useGravity);
        dynamic->setLinearDamping(settings.linearDamping); dynamic->setAngularDamping(settings.angularDamping);
        dynamic->setSleepThreshold(settings.sleepThreshold);
        PxRigidDynamicLockFlags locks;
        if(settings.lockPositionX) locks|=PxRigidDynamicLockFlag::eLOCK_LINEAR_X;
        if(settings.lockPositionY) locks|=PxRigidDynamicLockFlag::eLOCK_LINEAR_Y;
        if(settings.lockPositionZ) locks|=PxRigidDynamicLockFlag::eLOCK_LINEAR_Z;
        if(settings.lockRotationX) locks|=PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
        if(settings.lockRotationY) locks|=PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
        if(settings.lockRotationZ) locks|=PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;
        dynamic->setRigidDynamicLockFlags(locks);
        if(body->shapes.empty()) dynamic->setMass(settings.mass);
    }
    body->settings=settings; m_state->UpdateMass(*body); ++m_state->topologyRevision; return true;
}
ShapeHandle PhysicsWorld::AttachBox(BodyHandle body, Maths::Vector3 halfExtents, const ShapeSettings& settings)
{
    if (!Finite(halfExtents) || halfExtents.x <= 0 || halfExtents.y <= 0 || halfExtents.z <= 0) return {};
    return m_state->Attach(body, PxBoxGeometry(ToNative(halfExtents)), settings);
}
ShapeHandle PhysicsWorld::AttachSphere(BodyHandle body, float radius, const ShapeSettings& settings)
{
    if (!std::isfinite(radius) || radius <= 0) return {};
    return m_state->Attach(body, PxSphereGeometry(radius), settings);
}
ShapeHandle PhysicsWorld::AttachCapsule(BodyHandle body, float radius, float halfHeight, const ShapeSettings& settings)
{
    if (!std::isfinite(radius) || radius <= 0 || !std::isfinite(halfHeight) || halfHeight < 0) return {};
    return m_state->Attach(body, PxCapsuleGeometry(radius,halfHeight), settings);
}
ShapeHandle PhysicsWorld::AttachConvexMesh(BodyHandle body, std::span<const Maths::Vector3> vertices, const ShapeSettings& settings)
{
    if (vertices.size() < 4 || vertices.size() > UINT32_MAX || !std::all_of(vertices.begin(),vertices.end(),Finite) || !IsValid(body)) return {};
    std::vector<PxVec3> points; points.reserve(vertices.size());
    for (const auto vertex : vertices) points.push_back(ToNative(vertex));
    PxConvexMeshDesc desc;
    desc.points.count = static_cast<PxU32>(points.size()); desc.points.stride = sizeof(PxVec3); desc.points.data = points.data();
    desc.flags = PxConvexFlag::eCOMPUTE_CONVEX;
    const PxCookingParams params(m_state->sdk->physics->getTolerancesScale());
    auto* mesh = PxCreateConvexMesh(params, desc, m_state->sdk->physics->getPhysicsInsertionCallback());
    if (!mesh) return {};
    const auto handle = m_state->Attach(body, PxConvexMeshGeometry(mesh), settings);
    mesh->release(); return handle;
}
ShapeHandle PhysicsWorld::AttachCollisionMesh(BodyHandle body,const std::shared_ptr<const CookedCollisionMesh>& mesh,
    const ShapeSettings& settings,Maths::Vector3 scale)
{
    if (!mesh || !ValidBakeScale(scale) || scale.x<=0 || scale.y<=0 || scale.z<=0 ||
        !Finite(settings.localPosition) || !ValidRotation(settings.localRotation)) return {};
    auto data=mesh->m_impl;
    {
        std::shared_lock lock(m_state->mutex);
        const auto* record=m_state->Find(body);
        if (!record || (data->kind!=CollisionMeshKind::Convex && (record->settings.dynamic || settings.trigger))) return {};
    }
    std::shared_ptr<void> resource;
    {
        std::lock_guard lock(data->nativeMutex);
        if (!data->nativeResource)
        {
            PxDefaultMemoryInputData input(data->cooked.data(),static_cast<PxU32>(data->cooked.size()));
            const auto sdk=m_state->sdk;
            if (data->kind==CollisionMeshKind::Convex)
            {
                auto* native=sdk->physics->createConvexMesh(input); if (!native) return {};
                data->nativeResource={native,[sdk](void* pointer){static_cast<PxConvexMesh*>(pointer)->release();}};
            }
            else if (data->kind==CollisionMeshKind::Triangle)
            {
                auto* native=sdk->physics->createTriangleMesh(input); if (!native) return {};
                data->nativeResource={native,[sdk](void* pointer){static_cast<PxTriangleMesh*>(pointer)->release();}};
            }
            else
            {
                auto* native=sdk->physics->createHeightField(input); if (!native) return {};
                data->nativeResource={native,[sdk](void* pointer){static_cast<PxHeightField*>(pointer)->release();}};
            }
        }
        resource=data->nativeResource;
    }
    PxGeometryHolder geometry; std::span<const uint32_t> remap;
    if (data->kind==CollisionMeshKind::Convex) geometry.storeAny(PxConvexMeshGeometry(static_cast<PxConvexMesh*>(resource.get()),PxMeshScale(ToNative(scale))));
    else if (data->kind==CollisionMeshKind::Triangle)
    {
        auto* native=static_cast<PxTriangleMesh*>(resource.get());
        geometry.storeAny(PxTriangleMeshGeometry(native,PxMeshScale(ToNative(scale))));
        if (const auto* map=native->getTrianglesRemap()) remap={map,native->getNbTriangles()};
    }
    else
    {
        const PxHeightFieldGeometry heightfield(static_cast<PxHeightField*>(resource.get()),PxMeshGeometryFlags(),
            data->heightScale*scale.y,data->rowSpacing*scale.x,data->columnSpacing*scale.z);
        if(!heightfield.isValid()) return {};
        geometry.storeAny(heightfield);
    }
    PxBounds3 bounds; PxGeometryQuery::computeGeomBounds(bounds,geometry.any(),Pose(settings.localPosition,settings.localRotation));
    const auto dimensions=bounds.maximum-bounds.minimum;
    if (!bounds.minimum.isFinite() || !bounds.maximum.isFinite() || !dimensions.isFinite() ||
        bounds.minimum.abs().maxElement()>1e7f || bounds.maximum.abs().maxElement()>1e7f ||
        dimensions.x<0 || dimensions.y<0 || dimensions.z<0 || (int(dimensions.x>0)+int(dimensions.y>0)+int(dimensions.z>0))<2) return {};
    return m_state->Attach(body,geometry.any(),settings,remap,std::move(resource));
}
ShapeHandle PhysicsWorld::AttachCylinder(BodyHandle body, float radius, float halfHeight, const ShapeSettings& settings)
{
    if (!std::isfinite(radius) || radius<=0 || !std::isfinite(halfHeight) || halfHeight<=0 || !std::isfinite(halfHeight*2)) return {};
    auto callbacks = std::make_shared<PxCustomGeometryExt::CylinderCallbacks>(halfHeight*2,radius,1);
    return m_state->Attach(body,PxCustomGeometry(*callbacks),settings,{},callbacks,callbacks.get());
}
ShapeHandle PhysicsWorld::AttachHeightField(BodyHandle body, uint32_t rows, uint32_t columns, std::span<const float> heights,
    float rowSpacing, float columnSpacing, const ShapeSettings& settings)
{
    if (rows<2 || columns<2 || uint64_t(rows)*columns!=heights.size() || heights.size()>UINT32_MAX ||
        !std::isfinite(rowSpacing) || rowSpacing<1e-4f || !std::isfinite(columnSpacing) || columnSpacing<1e-4f) return {};
    {
        std::shared_lock lock(m_state->mutex);
        const auto* record=m_state->Find(body); if (!record || record->settings.dynamic || settings.trigger) return {};
    }
    float maximum=0;
    for (float height:heights) { if (!std::isfinite(height)) return {}; maximum=(std::max)(maximum,std::abs(height)); }
    const float heightScale=(std::max)(maximum/32766.0f,1e-4f);
    std::vector<PxHeightFieldSample> samples(heights.size());
    for (size_t i=0;i<heights.size();++i)
    {
        samples[i].height=static_cast<PxI16>(std::lround(heights[i]/heightScale));
        samples[i].materialIndex0=0; samples[i].materialIndex1=0; samples[i].clearTessFlag();
    }
    PxHeightFieldDesc desc;
    desc.nbRows=rows; desc.nbColumns=columns; desc.samples.data=samples.data(); desc.samples.stride=sizeof(PxHeightFieldSample);
    auto* field=PxCreateHeightField(desc,m_state->sdk->physics->getPhysicsInsertionCallback());
    if (!field) return {};
    const auto handle=m_state->Attach(body,PxHeightFieldGeometry(field,PxMeshGeometryFlags(),heightScale,rowSpacing,columnSpacing),settings);
    field->release(); return handle;
}
ShapeHandle PhysicsWorld::AttachTriangleMesh(BodyHandle body, std::span<const Maths::Vector3> vertices,
    std::span<const uint32_t> indices, const ShapeSettings& settings)
{
    {
        std::shared_lock lock(m_state->mutex);
        const auto* record = m_state->Find(body);
        if (!record || record->settings.dynamic) return {};
    }
    if (vertices.size() < 3 || indices.empty() || indices.size()%3 || vertices.size() > UINT32_MAX || indices.size()/3 > UINT32_MAX ||
        !std::all_of(vertices.begin(),vertices.end(),Finite)) return {};
    for (const auto index : indices) if (index >= vertices.size()) return {};
    std::vector<PxVec3> points; points.reserve(vertices.size());
    for (const auto vertex : vertices) points.push_back(ToNative(vertex));
    PxTriangleMeshDesc desc;
    desc.points.count = static_cast<PxU32>(points.size()); desc.points.stride = sizeof(PxVec3); desc.points.data = points.data();
    desc.triangles.count = static_cast<PxU32>(indices.size()/3); desc.triangles.stride = sizeof(uint32_t)*3; desc.triangles.data = indices.data();
    PxCookingParams params(m_state->sdk->physics->getTolerancesScale()); params.suppressTriangleMeshRemapTable = false;
    auto* mesh = PxCreateTriangleMesh(params,desc,m_state->sdk->physics->getPhysicsInsertionCallback());
    if (!mesh) return {};
    const auto* remap = mesh->getTrianglesRemap();
    const auto handle = m_state->Attach(body, PxTriangleMeshGeometry(mesh), settings,
        remap ? std::span<const uint32_t>(remap, mesh->getNbTriangles()) : std::span<const uint32_t>{});
    mesh->release(); return handle;
}
namespace
{
ShapeSettings LegacyShape(const BodySettings& settings)
{
    ShapeSettings shape;
    shape.localRotation = settings.localRotation; shape.layer = settings.layer;
    shape.material = settings.material; shape.trigger = settings.trigger;
    return shape;
}
}
BodyHandle PhysicsWorld::AddBox(Maths::Vector3 halfExtents, const BodySettings& settings)
{
    auto batch = BeginMutationBatch();
    const auto body = CreateBody(settings);
    if (body.IsValid() && !AttachBox(body, halfExtents, LegacyShape(settings)).IsValid()) { Remove(body); return {}; }
    return body;
}
BodyHandle PhysicsWorld::AddSphere(float radius, const BodySettings& settings)
{
    auto batch = BeginMutationBatch();
    const auto body = CreateBody(settings);
    if (body.IsValid() && !AttachSphere(body, radius, LegacyShape(settings)).IsValid()) { Remove(body); return {}; }
    return body;
}
BodyHandle PhysicsWorld::AddCapsule(float radius, float halfHeight, const BodySettings& settings)
{
    auto batch = BeginMutationBatch();
    const auto body = CreateBody(settings);
    if (body.IsValid() && !AttachCapsule(body, radius, halfHeight, LegacyShape(settings)).IsValid()) { Remove(body); return {}; }
    return body;
}
BodyHandle PhysicsWorld::AddTriangleMesh(std::span<const Maths::Vector3> vertices, std::span<const uint32_t> indices, const BodySettings& settings)
{
    if (settings.dynamic) return {};
    auto batch = BeginMutationBatch();
    const auto body = CreateBody(settings);
    if (body.IsValid() && !AttachTriangleMesh(body, vertices, indices, LegacyShape(settings)).IsValid()) { Remove(body); return {}; }
    return body;
}
bool PhysicsWorld::Remove(BodyHandle handle)
{
    std::unique_lock lock(m_state->mutex);
    auto* body = m_state->Find(handle); if (!body) return false;
    m_state->scene->removeActor(*body->actor);
    body->actor->release(); body->actor = nullptr;
    for (const auto shape : body->shapes)
    {
        m_state->shapes[shape.index].reset();
        if (++m_state->shapeGenerations[shape.index] == 0) ++m_state->shapeGenerations[shape.index];
        m_state->freeShapes.push_back(shape.index); --m_state->statistics.shapeCount;
    }
    m_state->bodies[handle.index].reset();
    if (++m_state->generations[handle.index] == 0) ++m_state->generations[handle.index];
    m_state->freeBodies.push_back(handle.index); --m_state->statistics.bodyCount; ++m_state->topologyRevision;
    m_state->scene->flushQueryUpdates();
    return true;
}
bool PhysicsWorld::Remove(ShapeHandle handle)
{
    std::unique_lock lock(m_state->mutex);
    if (!m_state->RemoveShape(handle)) return false;
    m_state->scene->flushQueryUpdates(); return true;
}
bool PhysicsWorld::IsValid(BodyHandle handle) const { std::shared_lock lock(m_state->mutex); return m_state->Find(handle) != nullptr; }
bool PhysicsWorld::IsValid(ShapeHandle handle) const { std::shared_lock lock(m_state->mutex); return m_state->Find(handle) != nullptr; }
BodyHandle PhysicsWorld::GetShapeBody(ShapeHandle handle) const
{ std::shared_lock lock(m_state->mutex); const auto* shape=m_state->Find(handle); return shape?shape->body:BodyHandle{}; }

void PhysicsWorld::Step(float delta)
{
    if (!std::isfinite(delta) || delta<=0) return;
    std::unique_lock lock(m_state->mutex);
    ++m_state->statistics.stepIndex;
    const auto begin = std::chrono::steady_clock::now();
    m_state->scene->simulate(delta);
    m_state->scene->fetchResults(true);
    m_state->dispatcher.Wait();
    m_state->statistics.stepMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    m_state->CollectEvents();
}
std::vector<ContactEvent> PhysicsWorld::TakeContactEvents()
{
    std::unique_lock lock(m_state->mutex);
    auto events=std::move(m_state->events); m_state->events.clear(); return events;
}
bool PhysicsWorld::GetTransform(BodyHandle handle, Maths::Vector3& position, Maths::Quaternion& rotation) const
{
    std::shared_lock lock(m_state->mutex);
    const auto* body=m_state->Find(handle); if (!body) return false;
    const auto pose=body->actor->getGlobalPose();
    position=FromNative(pose.p); rotation={pose.q.x,pose.q.y,pose.q.z,pose.q.w}; return true;
}
bool PhysicsWorld::SetVelocity(BodyHandle handle, Maths::Vector3 velocity)
{
    if (!Finite(velocity)) return false;
    std::unique_lock lock(m_state->mutex);
    const auto* body=m_state->Find(handle);
    auto* dynamic=body ? body->actor->is<PxRigidDynamic>() : nullptr;
    if (!dynamic || dynamic->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC)) return false;
    dynamic->setLinearVelocity(ToNative(velocity)); return true;
}
Maths::Vector3 PhysicsWorld::GetVelocity(BodyHandle handle) const
{
    std::shared_lock lock(m_state->mutex);
    const auto* body=m_state->Find(handle);
    const auto* dynamic=body ? body->actor->is<PxRigidDynamic>() : nullptr;
    return dynamic ? FromNative(dynamic->getLinearVelocity()) : Maths::Vector3{};
}
bool PhysicsWorld::AddForce(BodyHandle handle, Maths::Vector3 force)
{ return ApplyForce(handle, force, ForceMode::Force); }
namespace
{
PxForceMode::Enum NativeForceMode(ForceMode mode)
{
    switch (mode)
    {
    case ForceMode::Acceleration: return PxForceMode::eACCELERATION;
    case ForceMode::Impulse: return PxForceMode::eIMPULSE;
    case ForceMode::VelocityChange: return PxForceMode::eVELOCITY_CHANGE;
    default: return PxForceMode::eFORCE;
    }
}
BodyState ReadBodyState(const BodyRecord& body, uint64_t stepIndex)
{
    BodyState state; state.body = body.handle;
    state.stepIndex = stepIndex; state.poseRevision = body.poseRevision;
    const auto pose = body.actor->getGlobalPose();
    state.position = FromNative(pose.p); state.rotation = {pose.q.x,pose.q.y,pose.q.z,pose.q.w};
    if (const auto* dynamic = body.actor->is<PxRigidDynamic>())
    {
        state.velocity = FromNative(dynamic->getLinearVelocity()); state.angularVelocity = FromNative(dynamic->getAngularVelocity());
        state.mass = dynamic->getMass(); state.inertia = FromNative(dynamic->getMassSpaceInertiaTensor()); state.awake = !dynamic->isSleeping();
        const auto massPose=dynamic->getCMassLocalPose(); state.centerOfMass=FromNative(massPose.p);
        state.massFrameRotation={massPose.q.x,massPose.q.y,massPose.q.z,massPose.q.w};
    }
    return state;
}
}
bool PhysicsWorld::ApplyForce(BodyHandle handle, Maths::Vector3 force, ForceMode mode)
{
    if (!Finite(force) || mode > ForceMode::VelocityChange) return false;
    std::unique_lock lock(m_state->mutex);
    const auto* body=m_state->Find(handle);
    auto* dynamic=body ? body->actor->is<PxRigidDynamic>() : nullptr;
    if (!dynamic || dynamic->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC)) return false;
    dynamic->addForce(ToNative(force), NativeForceMode(mode)); return true;
}
bool PhysicsWorld::AddForceAtPosition(BodyHandle handle, Maths::Vector3 force, Maths::Vector3 position, ForceMode mode)
{
    if (!Finite(force) || !Finite(position) || mode > ForceMode::VelocityChange) return false;
    std::unique_lock lock(m_state->mutex);
    const auto* body = m_state->Find(handle);
    auto* dynamic = body ? body->actor->is<PxRigidDynamic>() : nullptr;
    if (!dynamic || body->settings.kinematic) return false;
    auto native = ToNative(force);
    if (mode == ForceMode::Acceleration || mode == ForceMode::VelocityChange) native *= dynamic->getMass();
    const auto center=dynamic->getGlobalPose().transform(dynamic->getCMassLocalPose().p);
    const auto lever=ToNative(position)-center;
    if (!native.isFinite() || !lever.isFinite() || !lever.cross(native).isFinite()) return false;
    PxRigidBodyExt::addForceAtPos(*dynamic, native, ToNative(position),
        mode == ForceMode::Impulse || mode == ForceMode::VelocityChange ? PxForceMode::eIMPULSE : PxForceMode::eFORCE);
    return true;
}
bool PhysicsWorld::AddTorque(BodyHandle handle, Maths::Vector3 torque, ForceMode mode)
{
    if (!Finite(torque) || mode > ForceMode::VelocityChange) return false;
    std::unique_lock lock(m_state->mutex);
    const auto* body = m_state->Find(handle);
    auto* dynamic = body ? body->actor->is<PxRigidDynamic>() : nullptr;
    if (!dynamic || body->settings.kinematic) return false;
    dynamic->addTorque(ToNative(torque), NativeForceMode(mode)); return true;
}
bool PhysicsWorld::SetAngularVelocity(BodyHandle handle, Maths::Vector3 velocity)
{
    if (!Finite(velocity)) return false;
    std::unique_lock lock(m_state->mutex);
    const auto* body = m_state->Find(handle);
    auto* dynamic = body ? body->actor->is<PxRigidDynamic>() : nullptr;
    if (!dynamic || body->settings.kinematic) return false;
    dynamic->setAngularVelocity(ToNative(velocity)); return true;
}
bool PhysicsWorld::SetKinematicTarget(BodyHandle handle, Maths::Vector3 position, Maths::Quaternion rotation)
{
    if (!Finite(position) || !ValidRotation(rotation)) return false;
    std::unique_lock lock(m_state->mutex);
    const auto* body = m_state->Find(handle);
    auto* dynamic = body ? body->actor->is<PxRigidDynamic>() : nullptr;
    if (!dynamic || !body->settings.kinematic) return false;
    dynamic->setKinematicTarget(Pose(position,rotation)); return true;
}
bool PhysicsWorld::SetAwake(BodyHandle handle, bool awake)
{
    std::unique_lock lock(m_state->mutex);
    const auto* body = m_state->Find(handle);
    auto* dynamic = body ? body->actor->is<PxRigidDynamic>() : nullptr;
    if (!dynamic || body->settings.kinematic) return false;
    if (awake) dynamic->wakeUp(); else dynamic->putToSleep();
    return true;
}
bool PhysicsWorld::SetGravity(Maths::Vector3 gravity)
{
    if (!Finite(gravity)) return false;
    std::unique_lock lock(m_state->mutex); m_state->scene->setGravity(ToNative(gravity)); return true;
}
bool PhysicsWorld::GetState(BodyHandle handle, BodyState& state) const
{
    std::shared_lock lock(m_state->mutex);
    const auto* body = m_state->Find(handle); if (!body) return false;
    state = ReadBodyState(*body, m_state->statistics.stepIndex); return true;
}
PhysicsStatistics PhysicsWorld::GetStatistics() const
{
    std::shared_lock lock(m_state->mutex);
    auto stats = m_state->statistics;
    for (const auto& body : m_state->bodies)
        if (body) if (const auto* dynamic = body->actor->is<PxRigidDynamic>())
        { if (dynamic->isSleeping()) ++stats.sleepingBodyCount; else ++stats.activeBodyCount; }
    return stats;
}
uint64_t PhysicsWorld::GetRevision() const noexcept { return m_state->mutex.Revision(); }

PhysicsState PhysicsWorld::CaptureState() const
{
    std::shared_lock lock(m_state->mutex);
    PhysicsState result; result.world = m_state->world; result.stepIndex = m_state->statistics.stepIndex;
    result.topologyRevision = m_state->topologyRevision; result.bodies.reserve(m_state->statistics.bodyCount);
    for (const auto& body : m_state->bodies) if (body) result.bodies.push_back(ReadBodyState(*body, m_state->statistics.stepIndex));
    return result;
}
bool PhysicsWorld::RestoreState(const PhysicsState& state, std::string& error)
{
    std::unique_lock lock(m_state->mutex);
    if (state.world != m_state->world || state.topologyRevision != m_state->topologyRevision || state.bodies.size() != m_state->statistics.bodyCount)
    { error = "Physics snapshot belongs to a different world or topology."; return false; }
    std::vector<bool> seen(m_state->bodies.size());
    for (const auto& saved : state.bodies)
    {
        const auto* body = m_state->Find(saved.body);
        if (!body || seen[saved.body.index] || !Finite(saved.position) || !ValidRotation(saved.rotation) ||
            !Finite(saved.velocity) || !Finite(saved.angularVelocity))
        { error = "Physics snapshot contains an invalid or duplicate body."; return false; }
        seen[saved.body.index] = true;
    }
    for (const auto& saved : state.bodies)
    {
        auto* body = m_state->Find(saved.body); body->actor->setGlobalPose(Pose(saved.position, saved.rotation));
        if (++body->poseRevision == 0) ++body->poseRevision;
        if (auto* dynamic = body->actor->is<PxRigidDynamic>(); dynamic && !body->settings.kinematic)
        {
            dynamic->clearForce(); dynamic->clearTorque();
            dynamic->setLinearVelocity(ToNative(saved.velocity)); dynamic->setAngularVelocity(ToNative(saved.angularVelocity));
            if (saved.awake) dynamic->wakeUp(); else dynamic->putToSleep();
        }
    }
    m_state->statistics.stepIndex = state.stepIndex; m_state->statistics.stepMilliseconds = 0;
    m_state->events.clear();
    { std::lock_guard callbackLock(m_state->callbackMutex); m_state->callbackEvents.clear(); }
    m_state->shapeContacts.clear(); m_state->ownerContacts.clear();
    m_state->scene->flushQueryUpdates(); error.clear(); return true;
}
bool PhysicsWorld::SetTransform(BodyHandle handle, Maths::Vector3 position, Maths::Quaternion rotation)
{
    if (!Finite(position) || !ValidRotation(rotation)) return false;
    std::unique_lock lock(m_state->mutex);
    auto* body = m_state->Find(handle);
    if (!body) return false;
    body->actor->setGlobalPose(Pose(position,rotation));
    if (++body->poseRevision == 0) ++body->poseRevision;
    m_state->scene->flushQueryUpdates();
    return true;
}
std::unique_ptr<PhysicsWorld::ReadAccess> PhysicsWorld::Read(const QueryFilter& filter) const
{
    return std::unique_ptr<ReadAccess>(new ReadAccess(std::make_unique<ReadAccess::Impl>(m_state,filter)));
}
PhysicsWorld::ReadAccess::ReadAccess(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
PhysicsWorld::ReadAccess::~ReadAccess() = default;

bool PhysicsWorld::ReadAccess::Raycast(Maths::Vector3 origin, Maths::Vector3 direction, float distance, QueryHit& hit) const
{
    if (!ValidRay(origin,direction,distance) || (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return false;
    Filter filter(m_impl->filter);
    PxRaycastBuffer buffer;
    if (!m_impl->state->scene->raycast(ToNative(origin),ToNative(direction.Normalised()),distance,buffer,
        PxHitFlag::eDEFAULT,QueryFlags(m_impl->filter),&filter) || !buffer.hasBlock) return false;
    hit = Convert(buffer.block); return true;
}

std::vector<QueryHit> PhysicsWorld::ReadAccess::RaycastAll(Maths::Vector3 origin, Maths::Vector3 direction, float distance) const
{
    std::vector<QueryHit> hits;
    if (!ValidRay(origin,direction,distance) || (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return hits;
    struct Collector : PxRaycastCallback
    {
        PxRaycastHit storage[64];
        std::vector<QueryHit>& results;
        explicit Collector(std::vector<QueryHit>& output) : PxRaycastCallback(storage,64),results(output) {}
        PxAgain processTouches(const PxRaycastHit* values, PxU32 count) override
        { for (PxU32 index = 0; index < count; ++index) results.push_back(Convert(values[index])); return true; }
    } collector(hits);
    Filter filter(m_impl->filter,true);
    m_impl->state->scene->raycast(ToNative(origin),ToNative(direction.Normalised()),distance,collector,
        PxHitFlag::eDEFAULT,QueryFlags(m_impl->filter),&filter);
    for (PxU32 index = 0; index < collector.nbTouches; ++index) hits.push_back(Convert(collector.touches[index]));
    std::sort(hits.begin(),hits.end(),[](const QueryHit& left, const QueryHit& right) { return left.distance < right.distance; });
    return hits;
}

QueryBufferResult PhysicsWorld::ReadAccess::RaycastNonAlloc(Maths::Vector3 origin, Maths::Vector3 direction, float distance, std::span<QueryHit> output) const
{
    if (!ValidRay(origin,direction,distance) || (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return {};
    Detail::BoundedQueryResults<QueryHit> results(output);
    struct Collector : PxRaycastCallback
    {
        PxRaycastHit storage[64];
        Detail::BoundedQueryResults<QueryHit>& results;
        explicit Collector(Detail::BoundedQueryResults<QueryHit>& output) : PxRaycastCallback(storage,64), results(output) {}
        PxAgain processTouches(const PxRaycastHit* hits, PxU32 count) override
        { for (PxU32 i=0;i<count;++i) results.Add(Convert(hits[i])); return true; }
    } collector(results);
    Filter filter(m_impl->filter,true);
    m_impl->state->scene->raycast(ToNative(origin),ToNative(direction.Normalised()),distance,collector,
        PxHitFlag::eDEFAULT,QueryFlags(m_impl->filter),&filter);
    for (PxU32 i=0;i<collector.nbTouches;++i) results.Add(Convert(collector.touches[i]));
    return results.Finish();
}

bool PhysicsWorld::ReadAccess::SweepSphere(float radius, Maths::Vector3 origin, Maths::Vector3 direction, float distance, QueryHit& hit) const
{
    if (!std::isfinite(radius) || radius <= 0 || !ValidRay(origin,direction,distance) ||
        (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return false;
    Filter filter(m_impl->filter);
    PxSweepBuffer buffer;
    if (!m_impl->state->scene->sweep(PxSphereGeometry(radius),PxTransform(ToNative(origin)),ToNative(direction.Normalised()),distance,buffer,
        PxHitFlag::eDEFAULT,QueryFlags(m_impl->filter),&filter) || !buffer.hasBlock) return false;
    hit = Convert(buffer.block); return true;
}

std::vector<BodyHandle> PhysicsWorld::ReadAccess::OverlapSphere(float radius, Maths::Vector3 center) const
{
    std::vector<BodyHandle> hits;
    if (!std::isfinite(radius) || radius <= 0 || !Finite(center) || (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return hits;
    struct Collector : PxOverlapCallback
    {
        PxOverlapHit storage[64];
        std::vector<BodyHandle>& results;
        explicit Collector(std::vector<BodyHandle>& output) : PxOverlapCallback(storage,64),results(output) {}
        PxAgain processTouches(const PxOverlapHit* values, PxU32 count) override
        { for (PxU32 i = 0; i < count; ++i) results.push_back(static_cast<BodyRecord*>(values[i].actor->userData)->handle); return true; }
    } collector(hits);
    Filter filter(m_impl->filter,true);
    m_impl->state->scene->overlap(PxSphereGeometry(radius),PxTransform(ToNative(center)),collector,QueryFlags(m_impl->filter),&filter);
    for (PxU32 index = 0; index < collector.nbTouches; ++index)
        hits.push_back(static_cast<BodyRecord*>(collector.touches[index].actor->userData)->handle);
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    return hits;
}

QueryBufferResult PhysicsWorld::ReadAccess::OverlapSphereNonAlloc(float radius, Maths::Vector3 center, std::span<QueryHit> output) const
{
    if (!std::isfinite(radius) || radius<=0 || !Finite(center) || (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return {};
    Detail::BoundedQueryResults<QueryHit> results(output);
    struct Collector : PxOverlapCallback
    {
        PxOverlapHit storage[64];
        Detail::BoundedQueryResults<QueryHit>& results;
        explicit Collector(Detail::BoundedQueryResults<QueryHit>& output) : PxOverlapCallback(storage,64), results(output) {}
        void Add(const PxOverlapHit& native)
        {
            const auto* shape=static_cast<const ShapeRecord*>(native.shape->userData);
            if (!shape) return;
            QueryHit hit; hit.body=shape->body; hit.shape=shape->handle;
            hit.owner=shape->settings.owner; hit.material=shape->settings.material; hit.subShapeId=shape->settings.subShapeId; results.Add(hit);
        }
        PxAgain processTouches(const PxOverlapHit* hits, PxU32 count) override
        { for(PxU32 i=0;i<count;++i) Add(hits[i]); return true; }
    } collector(results);
    Filter filter(m_impl->filter,true);
    m_impl->state->scene->overlap(PxSphereGeometry(radius),PxTransform(ToNative(center)),collector,QueryFlags(m_impl->filter),&filter);
    for(PxU32 i=0;i<collector.nbTouches;++i) collector.Add(collector.touches[i]);
    return results.Finish();
}

bool PhysicsWorld::ReadAccess::GetShapeWorldPose(ShapeHandle handle, Maths::Vector3& position, Maths::Quaternion& rotation) const
{
    const auto* shape=m_impl->state->Find(handle); if (!shape) return false;
    const auto* body=m_impl->state->Find(shape->body); if (!body) return false;
    const auto pose=body->actor->getGlobalPose()*shape->shape->getLocalPose();
    position=FromNative(pose.p); rotation={pose.q.x,pose.q.y,pose.q.z,pose.q.w}; return true;
}

bool PhysicsWorld::ReadAccess::SweepShape(const QueryGeometry3D& geometry, Maths::Vector3 origin, Maths::Quaternion rotation,
    Maths::Vector3 direction, float distance, QueryHit& hit, ShapeQueryStatus* status) const
{
    const auto report=[&](ShapeQueryStatus value) { if (status) *status=value; return value==ShapeQueryStatus::Success; };
    if (!ValidRay(origin,direction,distance) || distance>PX_MAX_SWEEP_DISTANCE || !ValidRotation(rotation)) return report(ShapeQueryStatus::InvalidInput);
    PxGeometryHolder native; PxQuat alignment; BodyHandle source;
    if (!report(m_impl->Geometry(geometry,native,alignment,source))) return false;
    auto pose=Pose(origin,rotation); pose.q=pose.q*alignment;
    if (!ValidQueryBounds(native.any(),pose)) return report(ShapeQueryStatus::InvalidInput);
    if (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic) return false;
    Filter filter(m_impl->filter,false,source); PxSweepBuffer buffer;
    if (!m_impl->state->scene->sweep(native.any(),pose,ToNative(direction.Normalised()),distance,buffer,
        SweepFlags(native.any(),geometry.preferPreciseSweep),QueryFlags(m_impl->filter),&filter) || !buffer.hasBlock) return false;
    hit=Convert(buffer.block); return true;
}

std::vector<QueryHit> PhysicsWorld::ReadAccess::SweepShapeAll(const QueryGeometry3D& geometry, Maths::Vector3 origin,
    Maths::Quaternion rotation, Maths::Vector3 direction, float distance, ShapeQueryStatus* status) const
{
    if (status) *status=ShapeQueryStatus::InvalidInput;
    if (!ValidRay(origin,direction,distance) || distance>PX_MAX_SWEEP_DISTANCE || !ValidRotation(rotation)) return {};
    PxGeometryHolder native; PxQuat alignment; BodyHandle source;
    const auto validity=m_impl->Geometry(geometry,native,alignment,source); if (status) *status=validity;
    if (validity!=ShapeQueryStatus::Success) return {};
    auto pose=Pose(origin,rotation); pose.q=pose.q*alignment;
    if (!ValidQueryBounds(native.any(),pose)) { if (status) *status=ShapeQueryStatus::InvalidInput; return {}; }
    if (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic) return {};
    VectorQueryResults results; QueryCollector<PxSweepHit,VectorQueryResults> collector(results); Filter filter(m_impl->filter,true,source);
    m_impl->state->scene->sweep(native.any(),pose,ToNative(direction.Normalised()),distance,collector,
        SweepFlags(native.any(),geometry.preferPreciseSweep),QueryFlags(m_impl->filter),&filter);
    collector.Finish(); results.Sort(); return std::move(results.hits);
}

ShapeQueryBufferResult PhysicsWorld::ReadAccess::SweepShapeNonAlloc(const QueryGeometry3D& geometry, Maths::Vector3 origin,
    Maths::Quaternion rotation, Maths::Vector3 direction, float distance, std::span<QueryHit> output) const
{
    if (!ValidRay(origin,direction,distance) || distance>PX_MAX_SWEEP_DISTANCE || !ValidRotation(rotation)) return {{},ShapeQueryStatus::InvalidInput};
    PxGeometryHolder native; PxQuat alignment; BodyHandle source;
    const auto validity=m_impl->Geometry(geometry,native,alignment,source);
    if (validity!=ShapeQueryStatus::Success) return {{},validity};
    auto pose=Pose(origin,rotation); pose.q=pose.q*alignment;
    if (!ValidQueryBounds(native.any(),pose)) return {{},ShapeQueryStatus::InvalidInput};
    if (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic) return {};
    Detail::BoundedQueryResults<QueryHit> results(output); QueryCollector<PxSweepHit,Detail::BoundedQueryResults<QueryHit>> collector(results);
    Filter filter(m_impl->filter,true,source);
    m_impl->state->scene->sweep(native.any(),pose,ToNative(direction.Normalised()),distance,collector,
        SweepFlags(native.any(),geometry.preferPreciseSweep),QueryFlags(m_impl->filter),&filter);
    collector.Finish(); return {results.Finish(),ShapeQueryStatus::Success};
}

std::vector<QueryHit> PhysicsWorld::ReadAccess::OverlapShape(const QueryGeometry3D& geometry, Maths::Vector3 position,
    Maths::Quaternion rotation, ShapeQueryStatus* status) const
{
    if (status) *status=ShapeQueryStatus::InvalidInput;
    if (!Finite(position) || !ValidRotation(rotation)) return {};
    PxGeometryHolder native; PxQuat alignment; BodyHandle source;
    const auto validity=m_impl->Geometry(geometry,native,alignment,source); if (status) *status=validity;
    if (validity!=ShapeQueryStatus::Success) return {};
    auto pose=Pose(position,rotation); pose.q=pose.q*alignment;
    if (!ValidQueryBounds(native.any(),pose)) { if (status) *status=ShapeQueryStatus::InvalidInput; return {}; }
    if (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic) return {};
    VectorQueryResults results; QueryCollector<PxOverlapHit,VectorQueryResults> collector(results); Filter filter(m_impl->filter,true,source);
    m_impl->state->scene->overlap(native.any(),pose,collector,QueryFlags(m_impl->filter),&filter);
    collector.Finish(); results.Sort(); return std::move(results.hits);
}

ShapeQueryBufferResult PhysicsWorld::ReadAccess::OverlapShapeNonAlloc(const QueryGeometry3D& geometry, Maths::Vector3 position,
    Maths::Quaternion rotation, std::span<QueryHit> output) const
{
    if (!Finite(position) || !ValidRotation(rotation)) return {{},ShapeQueryStatus::InvalidInput};
    PxGeometryHolder native; PxQuat alignment; BodyHandle source;
    const auto validity=m_impl->Geometry(geometry,native,alignment,source);
    if (validity!=ShapeQueryStatus::Success) return {{},validity};
    auto pose=Pose(position,rotation); pose.q=pose.q*alignment;
    if (!ValidQueryBounds(native.any(),pose)) return {{},ShapeQueryStatus::InvalidInput};
    if (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic) return {};
    Detail::BoundedQueryResults<QueryHit> results(output); QueryCollector<PxOverlapHit,Detail::BoundedQueryResults<QueryHit>> collector(results);
    Filter filter(m_impl->filter,true,source);
    m_impl->state->scene->overlap(native.any(),pose,collector,QueryFlags(m_impl->filter),&filter);
    collector.Finish(); return {results.Finish(),ShapeQueryStatus::Success};
}
}
