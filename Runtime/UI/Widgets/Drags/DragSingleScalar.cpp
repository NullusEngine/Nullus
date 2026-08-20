#include <cstdint>

#include "UI/Widgets/DataWidget.h"
#include "UI/Widgets/Drags/DragSingleScalar.h"

namespace NLS::UI::Widgets
{
// DragSingleScalar is part of the public UI ABI.  GUIDrawer is header-only and
// can instantiate it for every portable scalar type, so keep the concrete
// instances in NLS_UI instead of leaving shared-library consumers with an
// imported template that has no definition.
#define NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(type) \
    template class DataWidget<type>; \
    template class DragSingleScalar<type>;

NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(int8_t)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(uint8_t)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(int16_t)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(uint16_t)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(int32_t)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(uint32_t)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(int64_t)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(uint64_t)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(float)
NLS_INSTANTIATE_DRAG_SINGLE_SCALAR(double)

#undef NLS_INSTANTIATE_DRAG_SINGLE_SCALAR
} // namespace NLS::UI::Widgets
