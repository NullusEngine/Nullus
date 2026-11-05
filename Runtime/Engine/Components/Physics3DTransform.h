#pragma once

#include "Components/TransformComponent.h"
#include <Math/Matrix3.h>
#include <Math/Vector4.h>
#include <cmath>
#include <string>

namespace NLS::Engine::Components::Physics3DDetail
{
inline bool Finite(Maths::Vector3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool Same(Maths::Vector3 a, Maths::Vector3 b)
{ return std::abs(a.x-b.x)<1e-6f && std::abs(a.y-b.y)<1e-6f && std::abs(a.z-b.z)<1e-6f; }
inline bool Same(Maths::Quaternion a, Maths::Quaternion b)
{
    const double lengths = double(a.x*a.x+a.y*a.y+a.z*a.z+a.w*a.w) * double(b.x*b.x+b.y*b.y+b.z*b.z+b.w*b.w);
    const double dot = double(a.x)*b.x + double(a.y)*b.y + double(a.z)*b.z + double(a.w)*b.w;
    return lengths>1e-12 && dot*dot >= (1-1e-6)*lengths;
}
struct Frame
{
    Maths::Vector3 position, xAxis, yAxis, zAxis, scale;
    Maths::Quaternion rotation;
    bool reflected = false;
};
inline bool Capture(const TransformComponent& transform, Frame& frame, std::string& error)
{
    const auto& matrix = transform.GetWorldMatrix();
    const auto a=matrix.GetRow0(), b=matrix.GetRow1(), c=matrix.GetRow2();
    frame.position=transform.GetWorldPosition();
    frame.xAxis={a.x,b.x,c.x}; frame.yAxis={a.y,b.y,c.y}; frame.zAxis={a.z,b.z,c.z};
    frame.scale={frame.xAxis.Length(),frame.yAxis.Length(),frame.zAxis.Length()};
    if (!Finite(frame.position) || !Finite(frame.xAxis) || !Finite(frame.yAxis) || !Finite(frame.zAxis) ||
        !Finite(frame.scale) || frame.scale.x<1e-6f || frame.scale.y<1e-6f || frame.scale.z<1e-6f)
    { error="3D physics requires finite, nondegenerate transforms."; return false; }
    auto x=frame.xAxis/frame.scale.x, y=frame.yAxis/frame.scale.y, z=frame.zAxis/frame.scale.z;
    const auto dot=[](Maths::Vector3 u, Maths::Vector3 v) { return u.x*v.x+u.y*v.y+u.z*v.z; };
    if (std::abs(dot(x,y))>1e-4f || std::abs(dot(x,z))>1e-4f || std::abs(dot(y,z))>1e-4f)
    { error="3D collider transforms containing shear cannot be represented by primitive geometry."; return false; }
    const float determinant=x.x*(y.y*z.z-y.z*z.y)-x.y*(y.x*z.z-y.z*z.x)+x.z*(y.x*z.y-y.y*z.x);
    frame.reflected=determinant<0;
    if (frame.reflected) x*= -1; // Symmetric primitive geometry can bake this reflection.
    frame.rotation=Maths::Quaternion(Maths::Matrix3(x.x,y.x,z.x,x.y,y.y,z.y,x.z,y.z,z.z));
    error.clear(); return true;
}
}
