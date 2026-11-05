#include "SpriteGeometry.h"

#include <algorithm>
#include <cmath>

namespace NLS::Core::Assets
{
std::optional<SpritePatchGeometry> BuildSlicedSpriteGeometry(
    const SpriteArtifact& sprite,float width,float height,std::string& error)
{
    if(!sprite.Validate(error))return {};
    if(!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0||width>1000000||height>1000000)
    {error="Sliced Sprite size must be positive, finite and at most one million world units";return {};}
    float left=sprite.border[0]/sprite.pixelsPerUnit,right=sprite.border[2]/sprite.pixelsPerUnit;
    float bottom=sprite.border[1]/sprite.pixelsPerUnit,top=sprite.border[3]/sprite.pixelsPerUnit;
    const bool collapsedX=left+right>=width,collapsedY=bottom+top>=height;
    if(left+right>width){const auto factor=width/(left+right);left*=factor;right=width-left;}
    if(bottom+top>height){const auto factor=height/(bottom+top);bottom*=factor;top=height-bottom;}
    const float px=sprite.pivot[0]*width,py=sprite.pivot[1]*height;
    const std::array<float,4> x{-px,left-px,collapsedX?left-px:width-right-px,width-px};
    const std::array<float,4> y{-py,bottom-py,collapsedY?bottom-py:height-top-py,height-py};
    const std::array<float,4> u{float(sprite.x)/sprite.sourceWidth,float(uint64_t(sprite.x)+sprite.border[0])/sprite.sourceWidth,
        float(uint64_t(sprite.x)+sprite.width-sprite.border[2])/sprite.sourceWidth,float(uint64_t(sprite.x)+sprite.width)/sprite.sourceWidth};
    const std::array<float,4> v{float(uint64_t(sprite.y)+sprite.height)/sprite.sourceHeight,
        float(uint64_t(sprite.y)+sprite.height-sprite.border[1])/sprite.sourceHeight,
        float(uint64_t(sprite.y)+sprite.border[3])/sprite.sourceHeight,float(sprite.y)/sprite.sourceHeight};
    SpritePatchGeometry result;result.bounds={x.front(),y.front(),x.back(),y.back()};
    for(size_t row=0;row<3;++row)for(size_t column=0;column<3;++column)
    {
        if(x[column+1]<=x[column]||y[row+1]<=y[row])continue;
        result.patches[result.count++]={{x[column],y[row],x[column+1],y[row+1]},
            {u[column],v[row+1],u[column+1],v[row]},column==1,row==1};
    }
    error.clear();return result;
}
std::optional<SpriteTiledGeometry> BuildTiledSpriteGeometry(
    const SpriteArtifact& sprite,float width,float height,std::string& error)
{
    const auto sliced=BuildSlicedSpriteGeometry(sprite,width,height,error);if(!sliced)return {};
    SpriteTiledGeometry result;result.bounds=sliced->bounds;
    const double tileWidth=double(sprite.width-sprite.border[0]-sprite.border[2])/sprite.pixelsPerUnit;
    const double tileHeight=double(sprite.height-sprite.border[1]-sprite.border[3])/sprite.pixelsPerUnit;
    for(uint32_t index=0;index<sliced->count;++index)
    {
        const auto& patch=sliced->patches[index];const auto& rect=patch.localRect;const auto& uv=patch.uvRect;
        const double w=double(rect[2])-rect[0],h=double(rect[3])-rect[1];
        const bool repeatX=patch.repeatX&&tileWidth>0,repeatY=patch.repeatY&&tileHeight>0;
        const double countX=repeatX?std::max(1.0,std::ceil(w/tileWidth)):1;
        const double countY=repeatY?std::max(1.0,std::ceil(h/tileHeight)):1;
        if(countX>MaxTiledSpritePatches||countY>MaxTiledSpritePatches||
            countX*countY>MaxTiledSpritePatches-result.patches.size())
        {error="Tiled Sprite exceeds the 65,536-quad limit. Reduce Size or increase the tile's world size.";return {};}
        const double stepX=repeatX?tileWidth:w,stepY=repeatY?tileHeight:h;
        for(uint32_t row=0;row<uint32_t(countY);++row)for(uint32_t column=0;column<uint32_t(countX);++column)
        {
            const double x=double(rect[0])+column*stepX,y=double(rect[1])+row*stepY;
            const double right=std::min(double(rect[2]),x+stepX),top=std::min(double(rect[3]),y+stepY);
            if(right<=x||top<=y)continue;
            const float fx=float(std::clamp((right-x)/stepX,0.0,1.0)),fy=float(std::clamp((top-y)/stepY,0.0,1.0));
            SpritePatch value{{float(x),float(y),float(right),float(top)},
                {uv[0],uv[3]+(uv[1]-uv[3])*fy,uv[0]+(uv[2]-uv[0])*fx,uv[3]}};
            if(value.localRect[2]>value.localRect[0]&&value.localRect[3]>value.localRect[1])result.patches.push_back(value);
        }
    }
    error.clear();return result;
}
}
