#include "PhysicsShapeBuilder.h"
#include "Assets/CollisionMeshAsset.h"
#include "Assets/PhysicsMaterial.h"
#include <algorithm>

namespace NLS::Engine::Components::Physics3DDetail
{
bool BuildShapeResource(const Assets::PhysicsShapeData& data,const Frame& frame,const Frame& root,
    Maths::Vector3 center,const Physics::ShapeSettings& defaults,bool moving,ShapeBuild& output,std::string& error)
{
    ShapeBuild result; result.shapes.reserve(data.parts.size()); const auto inverse=Maths::Quaternion::Inverse(root.rotation);
    const auto transform=[&](Maths::Vector3 v){return frame.xAxis*v.x+frame.yAxis*v.y+frame.zAxis*v.z;};
    const auto dot=[](Maths::Vector3 a,Maths::Vector3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    for(const auto& part:data.parts)
    {
        const auto fail=[&](const std::string& detail){error="Shape part "+std::to_string(part.id)+": "+detail; return false;};
        auto x=transform(part.rotation*Maths::Vector3{1,0,0})*part.scale.x;
        auto y=transform(part.rotation*Maths::Vector3{0,1,0})*part.scale.y;
        auto z=transform(part.rotation*Maths::Vector3{0,0,1})*part.scale.z;
        const Maths::Vector3 scale{x.Length(),y.Length(),z.Length()};
        const auto position=frame.position+transform(center+part.position);
        if(!Finite(scale) || !Finite(position) || scale.x<1e-6f || scale.y<1e-6f || scale.z<1e-6f)
            return fail("nonfinite or degenerate transformed geometry.");
        x/=scale.x; y/=scale.y; z/=scale.z;
        if(std::abs(dot(x,y))>1e-4f || std::abs(dot(x,z))>1e-4f || std::abs(dot(y,z))>1e-4f)
            return fail("rotation under nonuniform scale produces unsupported shear.");
        const bool reflected=x.x*(y.y*z.z-y.z*z.y)-x.y*(y.x*z.z-y.z*z.x)+x.z*(y.x*z.y-y.y*z.x)<0;
        if(reflected) x*= -1;
        Physics::PhysicsShapeDescription3D description; description.geometry=part.geometry; description.settings=defaults;
        auto& geometry=description.geometry; auto& settings=description.settings;
        settings.subShapeId=part.id; settings.localPosition=inverse*(position-root.position);
        settings.localRotation=inverse*Maths::Quaternion(Maths::Matrix3(x.x,y.x,z.x,x.y,y.y,z.y,x.z,y.z,z.z));
        const auto equal=[](float a,float b){return std::abs(a-b)<=1e-5f*(std::max)(a,b);};
        switch(geometry.type)
        {
        case Physics::CollisionGeometryType3D::Box:
            geometry.halfExtents={geometry.halfExtents.x*scale.x,geometry.halfExtents.y*scale.y,geometry.halfExtents.z*scale.z}; break;
        case Physics::CollisionGeometryType3D::Sphere:
            if(!equal(scale.x,scale.y) || !equal(scale.x,scale.z)) return fail("sphere requires uniform scale.");
            geometry.radius*=scale.x; break;
        case Physics::CollisionGeometryType3D::Capsule:
        case Physics::CollisionGeometryType3D::Cylinder:
            if(!equal(scale.x,scale.z)) return fail("capsule and cylinder require equal radial X/Z scales.");
            geometry.radius*=scale.x; geometry.halfHeight*=scale.y;
            if(geometry.type==Physics::CollisionGeometryType3D::Capsule)
                settings.localRotation=settings.localRotation*Maths::Quaternion{0,0,0.70710678f,0.70710678f};
            break;
        case Physics::CollisionGeometryType3D::CookedMesh:
        {
            if(reflected) return fail("mesh reflection must be baked into the collision asset's bakeScale.");
            auto mesh=Assets::LoadCollisionMeshAsset(part.collisionMesh,error); if(!mesh) return fail(error);
            geometry.cookedMesh=mesh->Capture(); geometry.meshScale=scale;
            if(!geometry.cookedMesh) return fail("collision mesh is unresolved.");
            if(geometry.cookedMesh->GetKind()!=Physics::CollisionMeshKind::Convex && (moving || settings.trigger))
                return fail("triangle mesh and heightfield require a static body and cannot be triggers.");
            result.snapshots.push_back(geometry.cookedMesh); result.residents.push_back(std::move(mesh)); break;
        }
        default: return fail("unsupported resource geometry.");
        }
        if(part.material.IsAsset())
        {
            auto material=Assets::LoadPhysicsMaterial(part.material,error); if(!material) return fail(error);
            const auto snapshot=material->Capture(); if(!snapshot) return fail("material is unresolved.");
            settings.staticFriction=snapshot->staticFriction; settings.dynamicFriction=snapshot->dynamicFriction;
            settings.restitution=snapshot->restitution; settings.frictionCombine=snapshot->frictionCombine;
            settings.restitutionCombine=snapshot->restitutionCombine;
            result.snapshots.push_back(snapshot); result.residents.push_back(std::move(material));
        }
        result.shapes.push_back(std::move(description));
    }
    output=std::move(result); error.clear(); return true;
}
}
