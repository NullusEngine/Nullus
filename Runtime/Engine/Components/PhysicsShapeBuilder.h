#pragma once

#include "Physics3DTransform.h"
#include "Assets/PhysicsShapeAsset.h"

namespace NLS::Engine::Components::Physics3DDetail
{
struct ShapeBuild
{
    std::vector<Physics::PhysicsShapeDescription3D> shapes;
    std::vector<std::shared_ptr<Object>> residents;
    std::vector<std::shared_ptr<const void>> snapshots;
};
bool BuildShapeResource(const Assets::PhysicsShapeData& data,const Frame& frame,const Frame& root,
    Maths::Vector3 center,const Physics::ShapeSettings& defaults,bool moving,ShapeBuild& result,std::string& error);
}
