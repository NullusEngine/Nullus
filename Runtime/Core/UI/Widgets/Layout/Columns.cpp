#include "Columns.h"

NLS::UI::Widgets::Layout::Columns::Columns(size_t size)
    : m_size(size)
{
    widths.resize(m_size, -1.f);
}

void NLS::UI::Widgets::Layout::Columns::_Draw_Impl()
{
    ImGui::Columns(static_cast<int>(m_size), ("##" + m_widgetID).c_str(), false);

    int counter = 0;

    CollectGarbages();

    for (auto it = m_widgets.begin(); it != m_widgets.end();)
    {
        it->first->Draw();

        ++it;

        if (it != m_widgets.end())
        {
            if (widths[counter] != -1.f)
                ImGui::SetColumnWidth(counter, widths[counter]);

            ImGui::NextColumn();
        }

        ++counter;

        if (counter == m_size)
            counter = 0;
    }

    ImGui::Columns(1); // Necessary to not break the layout for following widget
}
