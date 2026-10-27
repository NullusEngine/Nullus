#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "Inspector/ReflectedEditHost.h"

namespace
{
    using NLS::Editor::Inspector::ApplyReflectedEdit;
    using NLS::Editor::Inspector::IReflectedEditHost;
    using NLS::Editor::Inspector::ReflectedPropertyEdit;

    class RecordingEditHost final : public IReflectedEditHost
    {
    public:
        [[nodiscard]] bool BeginPropertyEdit(std::string_view) override
        {
            ++m_begin;
            m_open = m_beginResult;
            return m_beginResult;
        }

        [[nodiscard]] bool PreviewPropertyEdit() override
        {
            ++m_preview;
            return m_previewResult;
        }

        [[nodiscard]] bool CommitPropertyEdit() override
        {
            ++m_commit;
            if (m_commitResult)
                m_open = false;
            return m_commitResult;
        }

        [[nodiscard]] bool CancelPropertyEdit() override
        {
            ++m_cancel;
            m_open = false;
            return true;
        }

        [[nodiscard]] bool IsPropertyEditOpen() const override { return m_open; }

        bool m_beginResult = true;
        bool m_previewResult = true;
        bool m_commitResult = true;
        bool m_open = false;
        int m_begin = 0;
        int m_preview = 0;
        int m_commit = 0;
        int m_cancel = 0;
    };

    ReflectedPropertyEdit ChangedEdit()
    {
        ReflectedPropertyEdit edit;
        edit.changed = true;
        edit.paths = { "Value.field" };
        return edit;
    }
}

TEST(ReflectedEditHost, ChangedOpensAndPreviewsTheGesture)
{
    RecordingEditHost host;
    const auto edit = ChangedEdit();
    EXPECT_TRUE(ApplyReflectedEdit(host, edit, "Edit Field"));
    EXPECT_EQ(host.m_begin, 1);
    EXPECT_EQ(host.m_preview, 1);
    EXPECT_EQ(host.m_commit, 0);
    EXPECT_TRUE(host.m_open);
}

TEST(ReflectedEditHost, ChangedAndCompletedCommitsOnce)
{
    RecordingEditHost host;
    auto edit = ChangedEdit();
    edit.completed = true;
    EXPECT_TRUE(ApplyReflectedEdit(host, edit, "Edit Field"));
    EXPECT_EQ(host.m_begin, 1);
    EXPECT_EQ(host.m_preview, 1);
    EXPECT_EQ(host.m_commit, 1);
    EXPECT_FALSE(host.m_open);
}

TEST(ReflectedEditHost, ContinuousChangesReuseTheOpenGesture)
{
    RecordingEditHost host;
    EXPECT_TRUE(ApplyReflectedEdit(host, ChangedEdit(), "Edit Field"));
    EXPECT_TRUE(ApplyReflectedEdit(host, ChangedEdit(), "Edit Field"));
    EXPECT_EQ(host.m_begin, 1);
    EXPECT_EQ(host.m_preview, 2);

    auto completed = ChangedEdit();
    completed.completed = true;
    EXPECT_TRUE(ApplyReflectedEdit(host, completed, "Edit Field"));
    EXPECT_EQ(host.m_begin, 1);
    EXPECT_EQ(host.m_commit, 1);
    EXPECT_FALSE(host.m_open);
}

TEST(ReflectedEditHost, CanceledClosesAnOpenGesture)
{
    RecordingEditHost host;
    EXPECT_TRUE(ApplyReflectedEdit(host, ChangedEdit(), "Edit Field"));
    ReflectedPropertyEdit canceled;
    canceled.canceled = true;
    EXPECT_TRUE(ApplyReflectedEdit(host, canceled, "Edit Field"));
    EXPECT_EQ(host.m_cancel, 1);
    EXPECT_EQ(host.m_commit, 0);
    EXPECT_FALSE(host.m_open);
}

TEST(ReflectedEditHost, CanceledWithoutOpenGestureIsANoOp)
{
    RecordingEditHost host;
    ReflectedPropertyEdit canceled;
    canceled.canceled = true;
    EXPECT_TRUE(ApplyReflectedEdit(host, canceled, "Edit Field"));
    EXPECT_EQ(host.m_cancel, 0);
    EXPECT_EQ(host.m_commit, 0);
}

TEST(ReflectedEditHost, BeginFailureSkipsPreviewAndCommit)
{
    RecordingEditHost host;
    host.m_beginResult = false;
    auto edit = ChangedEdit();
    edit.completed = true;
    EXPECT_FALSE(ApplyReflectedEdit(host, edit, "Edit Field"));
    EXPECT_EQ(host.m_begin, 1);
    EXPECT_EQ(host.m_preview, 0);
    EXPECT_EQ(host.m_commit, 0);
}

TEST(ReflectedEditHost, PreviewFailureCancelsAndReportsFailure)
{
    RecordingEditHost host;
    host.m_previewResult = false;
    auto edit = ChangedEdit();
    edit.completed = true;
    EXPECT_FALSE(ApplyReflectedEdit(host, edit, "Edit Field"));
    EXPECT_EQ(host.m_preview, 1);
    EXPECT_EQ(host.m_cancel, 1);
    EXPECT_EQ(host.m_commit, 0);
    EXPECT_FALSE(host.m_open);
}

TEST(ReflectedEditHost, CompletedWithoutChangeStillCommitsAnOpenGesture)
{
    RecordingEditHost host;
    EXPECT_TRUE(ApplyReflectedEdit(host, ChangedEdit(), "Edit Field"));
    ReflectedPropertyEdit completed;
    completed.completed = true;
    EXPECT_TRUE(ApplyReflectedEdit(host, completed, "Edit Field"));
    EXPECT_EQ(host.m_preview, 1);
    EXPECT_EQ(host.m_commit, 1);
    EXPECT_FALSE(host.m_open);
}

TEST(ReflectedEditHost, NoSignalsDoNotTouchTheHost)
{
    RecordingEditHost host;
    EXPECT_TRUE(ApplyReflectedEdit(host, {}, "Edit Field"));
    EXPECT_EQ(host.m_begin, 0);
    EXPECT_EQ(host.m_preview, 0);
    EXPECT_EQ(host.m_commit, 0);
    EXPECT_EQ(host.m_cancel, 0);
}
