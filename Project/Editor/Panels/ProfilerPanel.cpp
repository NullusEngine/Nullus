#include "Panels/ProfilerPanel.h"

namespace NLS::Editor::Panels
{
ProfilerPanel::ProfilerPanel(
    const std::string& p_title,
    const bool p_opened,
    const UI::PanelWindowSettings& p_windowSettings)
    : PanelWindow(p_title, p_opened, p_windowSettings),
      m_statusText(CreateWidget<UI::Widgets::Text>("TimelineProfiler: Unknown")),
      m_detailText(CreateWidget<UI::Widgets::Text>("No timeline data has been recorded yet."))
{
    RefreshStatus();
    m_timelineSink.PrepareTimelineUI();
}

void ProfilerPanel::RefreshStatus()
{
    const auto state = m_timelineSink.GetState();
    if (state.availability == NLS::Base::Profiling::ProfilerAvailability::Available)
    {
        m_statusText.content.clear();
        m_detailText.content.clear();
    }
    else if (!state.lastError.empty())
    {
        m_statusText.content =
            std::string("TimelineProfiler: ") +
            NLS::Base::Profiling::TimelineProfilerSink::FormatAvailability(state.availability);
        m_detailText.content = state.lastError;
    }
    else
    {
        m_statusText.content =
            std::string("TimelineProfiler: ") +
            NLS::Base::Profiling::TimelineProfilerSink::FormatAvailability(state.availability);
        m_detailText.content = "Timeline data is not available.";
    }
}

void ProfilerPanel::BeginProfilerFrame()
{
    m_timelineSink.TickFrame();
}

NLS::Base::Profiling::TimelineProfilerSink& ProfilerPanel::GetTimelineSink()
{
    return m_timelineSink;
}

void ProfilerPanel::OnBeforeDrawWidgets()
{
    RefreshStatus();
    if (m_timelineSink.GetState().availability == NLS::Base::Profiling::ProfilerAvailability::Available)
        m_timelineSink.DrawTimeline();
}

void ProfilerPanel::OnAfterDrawWidgets()
{
}
}
