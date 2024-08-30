
#include "UI/Widgets/Plots/APlot.h"
namespace NLS::UI::Widgets
{
APlot::APlot(
    const std::vector<float>& p_data,
    float p_minScale,
    float p_maxScale,
    const Maths::Vector2& p_size,
    const std::string& p_overlay,
    const std::string& p_label)
    : DataWidget(data), data(p_data), minScale(p_minScale), maxScale(p_maxScale), size(p_size), overlay(p_overlay), label(p_label)
{
}
} // namespace NLS::UI::Widgets
