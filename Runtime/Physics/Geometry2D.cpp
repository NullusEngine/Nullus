#include "Geometry2D.h"
#include <box2d/box2d.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace NLS::Physics
{
namespace Detail
{
bool ValidGeometryPoint2D(Maths::Vector2 p)
{ return std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x)<=MaxGeometryCoordinate2D && std::abs(p.y)<=MaxGeometryCoordinate2D; }
bool ValidGeometryEdge2D(Maths::Vector2 a,Maths::Vector2 b)
{ return ValidGeometryPoint2D(a) && ValidGeometryPoint2D(b) && std::hypot(double(a.x)-b.x,double(a.y)-b.y)>=MinEdgeLength2D; }
}
namespace
{
using Point=Maths::Vector2;
constexpr double Epsilon=1e-8;
double Cross(Point a,Point b,Point c)
{ return (double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x); }
bool OnSegment(Point a,Point b,Point p)
{ return std::abs(Cross(a,b,p))<=Epsilon && p.x>=(std::min)(a.x,b.x) && p.x<=(std::max)(a.x,b.x) && p.y>=(std::min)(a.y,b.y) && p.y<=(std::max)(a.y,b.y); }
bool Intersects(Point a,Point b,Point c,Point d)
{
    const auto x=Cross(a,b,c),y=Cross(a,b,d),z=Cross(c,d,a),w=Cross(c,d,b);
    return ((x>Epsilon && y<-Epsilon || x<-Epsilon && y>Epsilon) && (z>Epsilon && w<-Epsilon || z<-Epsilon && w>Epsilon)) ||
        OnSegment(a,b,c) || OnSegment(a,b,d) || OnSegment(c,d,a) || OnSegment(c,d,b);
}
bool Validate(std::span<const Point> points,bool closed,std::string& error)
{
    for(size_t i=0;i<points.size();++i)
    {
        if(!Detail::ValidGeometryPoint2D(points[i])) { error="2D contour has a nonfinite or out-of-range vertex."; return false; }
        for(size_t j=0;j<i;++j) if(!Detail::ValidGeometryEdge2D(points[i],points[j]))
        { error="2D contour vertices must be at least 0.01 meters apart."; return false; }
    }
    const auto edges=closed?points.size():points.size()-1;
    for(size_t i=0;i<edges;++i)
    {
        const auto a=points[i],b=points[(i+1)%points.size()];
        for(size_t j=i+1;j<edges;++j)
        {
            if(j==i+1 || (closed && i==0 && j+1==edges)) continue;
            if(Intersects(a,b,points[j],points[(j+1)%points.size()]))
            { error="2D contour self-intersects or has touching nonadjacent edges."; return false; }
        }
    }
    // Adjacent overlapping edges are excluded from the nonadjacent test above.
    for(size_t i=closed?0:1;i<(closed?points.size():points.size()-1);++i)
    {
        const auto a=points[(i+points.size()-1)%points.size()],b=points[i],c=points[(i+1)%points.size()];
        if(std::abs(Cross(a,b,c))<=Epsilon && (double(a.x)-b.x)*(double(c.x)-b.x)+(double(a.y)-b.y)*(double(c.y)-b.y)>0)
        { error="2D contour backtracks along an adjacent edge."; return false; }
    }
    return true;
}
void RemoveCollinear(std::vector<Point>& points)
{
    bool removed=true;
    while(removed && points.size()>3)
    {
        removed=false;
        for(size_t i=0;i<points.size();++i) if(std::abs(Cross(points[(i+points.size()-1)%points.size()],points[i],points[(i+1)%points.size()]))<=Epsilon)
        { points.erase(points.begin()+i); removed=true; break; }
    }
}
bool Convex(const std::vector<Point>& points)
{
    if(points.size()<3 || points.size()>B2_MAX_POLYGON_VERTICES) return false;
    for(size_t i=0;i<points.size();++i) if(Cross(points[i],points[(i+1)%points.size()],points[(i+2)%points.size()])<=Epsilon) return false;
    b2Vec2 native[B2_MAX_POLYGON_VERTICES];
    for(size_t i=0;i<points.size();++i) native[i]={points[i].x,points[i].y};
    const auto hull=b2ComputeHull(native,static_cast<int>(points.size()));
    return hull.count==static_cast<int>(points.size()) && b2ValidateHull(&hull);
}
bool Merge(std::vector<Point>& a,const std::vector<Point>& b)
{
    for(size_t i=0;i<a.size();++i) for(size_t j=0;j<b.size();++j)
        if(a[i]==b[(j+1)%b.size()] && a[(i+1)%a.size()]==b[j])
        {
            std::vector<Point> joined;
            for(size_t k=0;k<a.size();++k) joined.push_back(a[(i+1+k)%a.size()]);
            for(size_t k=0;k+2<b.size();++k) joined.push_back(b[(j+2+k)%b.size()]);
            RemoveCollinear(joined);
            if(Convex(joined)) { a=std::move(joined); return true; }
        }
    return false;
}
}
bool BuildPolygon2D(std::span<const Point> vertices,const ShapeSettings2D& settings,
    std::vector<PhysicsShapeDescription2D>& output,std::string& error)
{
    if(vertices.size()<3 || vertices.size()>MaxPolygonVertices2D) { error="2D polygon requires 3..256 vertices."; return false; }
    if(!Validate(vertices,true,error)) return false;
    double area=0; for(size_t i=0;i<vertices.size();++i) area+=Cross(vertices[0],vertices[i],vertices[(i+1)%vertices.size()]);
    if(std::abs(area)<=Epsilon) { error="2D polygon area is degenerate."; return false; }
    std::vector<Point> points(vertices.begin(),vertices.end());
    if(area<0) std::reverse(points.begin()+1,points.end());
    RemoveCollinear(points); std::vector<std::vector<Point>> parts;
    if(Convex(points)) parts.push_back(points);
    else
    {
        std::vector<size_t> indices(points.size()); std::iota(indices.begin(),indices.end(),0);
        while(indices.size()>3)
        {
            bool found=false;
            for(size_t i=0;i<indices.size();++i)
            {
                const auto a=points[indices[(i+indices.size()-1)%indices.size()]],b=points[indices[i]],c=points[indices[(i+1)%indices.size()]];
                if(Cross(a,b,c)<=Epsilon || !Convex({a,b,c})) continue;
                bool inside=false;
                for(size_t k=0;k<indices.size();++k) if(k!=i && k!=(i+1)%indices.size() && k!=(i+indices.size()-1)%indices.size())
                {
                    const auto p=points[indices[k]];
                    if(Cross(a,b,p)>=-Epsilon && Cross(b,c,p)>=-Epsilon && Cross(c,a,p)>=-Epsilon) { inside=true; break; }
                }
                if(inside) continue;
                parts.push_back({a,b,c}); indices.erase(indices.begin()+i); found=true; break;
            }
            if(!found) { error="2D polygon cannot be decomposed into valid Box2D hulls at meter tolerance."; return false; }
        }
        std::vector<Point> last{points[indices[0]],points[indices[1]],points[indices[2]]};
        if(!Convex(last)) { error="2D polygon contains a sliver below Box2D hull tolerance."; return false; }
        parts.push_back(std::move(last));
        for(size_t i=0;i<parts.size();++i)
            for(size_t j=i+1;j<parts.size();) { if(Merge(parts[i],parts[j])) { parts.erase(parts.begin()+j); j=i+1; } else ++j; }
    }
    std::vector<PhysicsShapeDescription2D> result; result.reserve(parts.size());
    for(auto& part:parts)
    {
        PhysicsShapeDescription2D description; description.settings=settings; description.settings.subShapeId=static_cast<uint32_t>(result.size()+1);
        description.geometry.type=CollisionGeometryType2D::ConvexPolygon; description.geometry.vertices=std::move(part); result.push_back(std::move(description));
    }
    output=std::move(result); error.clear(); return true;
}
bool BuildChain2D(std::span<const Point> vertices,const ChainGeometrySettings2D& chain,const ShapeSettings2D& settings,
    std::vector<PhysicsShapeDescription2D>& output,std::string& error)
{
    if(vertices.size()<(chain.loop?3u:2u) || vertices.size()>(chain.loop?MaxCompoundShapes2D:MaxCompoundShapes2D+1))
    { error="2D chain requires 2..257 open vertices or 3..256 loop vertices."; return false; }
    if(!Validate(vertices,chain.loop,error)) return false;
    if(settings.trigger) { error="2D chains cannot be triggers."; return false; }
    const auto firstGhost=chain.loop?vertices.back():chain.hasPreviousGhost?chain.previousGhost:vertices[0]*2-vertices[1];
    const auto lastGhost=chain.loop?vertices.front():chain.hasNextGhost?chain.nextGhost:vertices.back()*2-vertices[vertices.size()-2];
    if(!Detail::ValidGeometryEdge2D(firstGhost,vertices.front()) || !Detail::ValidGeometryEdge2D(vertices.back(),lastGhost))
    { error="2D chain has invalid or too-close ghost vertices."; return false; }
    const auto count=chain.loop?vertices.size():vertices.size()-1;
    std::vector<PhysicsShapeDescription2D> result; result.reserve(count);
    for(size_t i=0;i<count;++i)
    {
        PhysicsShapeDescription2D part; part.settings=settings; part.settings.subShapeId=static_cast<uint32_t>(i+1);
        part.geometry.type=CollisionGeometryType2D::ChainSegment;
        const auto previous=i?vertices[i-1]:firstGhost;
        const auto next=i+2<vertices.size()?vertices[i+2]:chain.loop?vertices[(i+2)%vertices.size()]:lastGhost;
        part.geometry.vertices={previous,vertices[i],vertices[(i+1)%vertices.size()],next}; result.push_back(std::move(part));
    }
    output=std::move(result); error.clear(); return true;
}
}
