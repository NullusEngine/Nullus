#include "Panels/ProfilerPanel.h"

#include "Profiling/Profiler.h"

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
    m_timelineSink.SetRecordingEnabled(IsRecordingEnabled());
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
    m_timelineSink.SetRecordingEnabled(IsRecordingEnabled());
    if (IsRecordingEnabled())
        m_timelineSink.TickFrame();
}

bool ProfilerPanel::IsRecordingEnabled() const
{
    return enabled && IsOpened();
}

NLS::Base::Profiling::TimelineProfilerSink& ProfilerPanel::GetTimelineSink()
{
    return m_timelineSink;
}

void ProfilerPanel::OnBeforeDrawWidgets()
{
    m_timelineSink.SetRecordingEnabled(IsRecordingEnabled());
    RefreshStatus();
    if (m_timelineSink.GetState().availability == NLS::Base::Profiling::ProfilerAvailability::Available)
    {
        NLS_PROFILE_NAMED_SCOPE("ProfilerPanel::DrawTimeline");
        m_timelineSink.DrawTimeline();
    }
}

void ProfilerPanel::OnAfterDrawWidgets()
{
}
}
