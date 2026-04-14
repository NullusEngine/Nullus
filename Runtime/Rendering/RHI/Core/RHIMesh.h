#pragma once

#include <memory>
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Settings/EPrimitiveMode.h"

namespace NLS::Render::RHI
{
class NLS_RENDER_API RHIMesh
{
public:
    virtual ~RHIMesh() = default;

    virtual std::shared_ptr<RHIBuffer> GetVertexBuffer() const = 0;
    virtual std::shared_ptr<RHIBuffer> GetIndexBuffer() const = 0;
    virtual uint32_t GetVertexCount() const = 0;
    virtual uint32_t GetIndexCount() const = 0;
    virtual Settings::EPrimitiveMode GetPrimitiveMode() const = 0;
    virtual uint32_t GetVertexStride() const = 0;
    virtual IndexType GetIndexType() const = 0;
};
}