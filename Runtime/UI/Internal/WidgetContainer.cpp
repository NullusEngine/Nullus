
#include <algorithm>

#include "UI/Internal/WidgetContainer.h"

namespace NLS::UI::Internal
{
namespace
{
void BindWidgetToContainer(Widgets::AWidget& widget, WidgetContainer& container)
{
    widget.SetParent(&container);
    if (widget.IsDestroyed())
        container.MarkGarbageCollectionDirty();
}
}

WidgetContainer::~WidgetContainer()
{
    RemoveAllWidgets();
}

void WidgetContainer::RemoveWidget(Widgets::AWidget& p_widget)
{
    auto found = std::find_if(m_widgets.begin(), m_widgets.end(), [&p_widget](std::pair<UI::Widgets::AWidget*, Internal::EMemoryMode>& p_pair)
                              { return p_pair.first == &p_widget; });

    if (found != m_widgets.end())
    {
        found->first->SetParent(nullptr);
        if (found->second == Internal::EMemoryMode::INTERNAL_MANAGMENT)
            delete found->first;

        m_widgets.erase(found);
    }
}

void WidgetContainer::RemoveAllWidgets()
{
    std::for_each(m_widgets.begin(), m_widgets.end(), [](auto& pair)
                  {
				if (pair.first)
					pair.first->SetParent(nullptr);
				if (pair.second == Internal::EMemoryMode::INTERNAL_MANAGMENT)
					delete pair.first; });

    m_widgets.clear();
    m_garbageCollectionDirty = false;
}

void WidgetContainer::ConsiderWidget(Widgets::AWidget& p_widget, bool p_manageMemory)
{
    if (p_widget.GetParent() == this)
        return;

    auto memoryMode = p_manageMemory ? EMemoryMode::INTERNAL_MANAGMENT : EMemoryMode::EXTERNAL_MANAGMENT;
    if (auto* parent = p_widget.GetParent())
    {
        if (const auto previousMemoryMode = parent->ExtractWidget(p_widget))
            memoryMode = *previousMemoryMode;
    }

    m_widgets.emplace_back(std::make_pair(&p_widget, memoryMode));
    BindWidgetToContainer(p_widget, *this);
}

void WidgetContainer::UnconsiderWidget(Widgets::AWidget& p_widget)
{
    auto found = std::find_if(m_widgets.begin(), m_widgets.end(), [&p_widget](std::pair<UI::Widgets::AWidget*, Internal::EMemoryMode>& p_pair)
                              { return p_pair.first == &p_widget; });

    if (found != m_widgets.end())
    {
        p_widget.SetParent(nullptr);
        m_widgets.erase(found);
    }
}

void WidgetContainer::CollectGarbages()
{
    if (!m_garbageCollectionDirty)
        return;

    m_widgets.erase(std::remove_if(m_widgets.begin(), m_widgets.end(), [](std::pair<UI::Widgets::AWidget*, Internal::EMemoryMode>& p_item)
                                   {
				bool toDestroy = p_item.first && p_item.first->IsDestroyed();

				if (toDestroy)
				{
					p_item.first->SetParent(nullptr);
					if (p_item.second == Internal::EMemoryMode::INTERNAL_MANAGMENT)
						delete p_item.first;
				}

				return toDestroy; }),
                    m_widgets.end());
    m_garbageCollectionDirty = false;
}

void WidgetContainer::MarkGarbageCollectionDirty()
{
    m_garbageCollectionDirty = true;
}

void WidgetContainer::DrawWidgets()
{
    CollectGarbages();

    if (m_reversedDrawOrder)
    {
        for (auto it = m_widgets.crbegin(); it != m_widgets.crend(); ++it)
            it->first->Draw();
    }
    else
    {
        for (const auto& widget : m_widgets)
            widget.first->Draw();
    }
}

void WidgetContainer::ReverseDrawOrder(const bool reversed)
{
    m_reversedDrawOrder = reversed;
}

const std::vector<std::pair<UI::Widgets::AWidget*, EMemoryMode>>& WidgetContainer::GetWidgets() const
{
    return m_widgets;
}

std::optional<EMemoryMode> WidgetContainer::ExtractWidget(Widgets::AWidget& p_widget)
{
    auto found = std::find_if(m_widgets.begin(), m_widgets.end(), [&p_widget](const auto& pair)
                              { return pair.first == &p_widget; });

    if (found == m_widgets.end())
        return std::nullopt;

    const auto memoryMode = found->second;
    p_widget.SetParent(nullptr);
    m_widgets.erase(found);
    return memoryMode;
}

} // namespace NLS::UI::Internal
