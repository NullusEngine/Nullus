#include "RenderObject.h"
#include "RHI/MeshGeometry.h"

using namespace NLS::Engine;
using namespace NLS;

RenderObject::RenderObject(Transform* parentTransform, MeshGeometry* mesh, TextureBase* tex, ShaderBase* shader)
{
    this->transform = parentTransform;
    this->mesh = mesh;
    this->texture = tex;
    this->shader = shader;
    this->colour = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
}

RenderObject::~RenderObject()
{
}