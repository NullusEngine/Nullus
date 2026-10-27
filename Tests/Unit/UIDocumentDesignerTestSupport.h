#pragma once

// Shared fixtures for the UI Designer tests. Kept in a header so the focused
// canvas suite and the panel suite use the same ImGui, undo and temp-directory
// setup without pulling editor panels into the canvas-only binary.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <Guid.h>
#include <ServiceLocator.h>
#include <imgui.h>

#include "Undo/EditorUndoService.h"

namespace NLS::Editor::UIToolsTest
{
    class ScopedDocumentDirectory final
    {
    public:
        ScopedDocumentDirectory()
            : m_path(std::filesystem::temp_directory_path() /
                ("NullusUIDesignerPanel_" + NLS::Guid::New().ToString()))
        {
            std::error_code error;
            std::filesystem::create_directories(m_path, error);
            EXPECT_FALSE(error);
        }

        ~ScopedDocumentDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        [[nodiscard]] std::filesystem::path Write(
            const std::filesystem::path& relativePath,
            const std::string& contents) const
        {
            const auto path = m_path / relativePath;
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << contents;
            return path;
        }

        [[nodiscard]] const std::filesystem::path& GetPath() const { return m_path; }

    private:
        std::filesystem::path m_path;
    };

    class ScopedUndoService final
    {
    public:
        ScopedUndoService()
        {
            NLS::Core::ServiceLocator::Remove<NLS::Editor::Undo::EditorUndoService>();
            NLS::Core::ServiceLocator::Provide(m_service);
        }

        ~ScopedUndoService()
        {
            NLS::Core::ServiceLocator::Remove<NLS::Editor::Undo::EditorUndoService>();
        }

        [[nodiscard]] NLS::Editor::Undo::EditorUndoService& Get() { return m_service; }

    private:
        NLS::Editor::Undo::EditorUndoService m_service;
    };

    class ScopedImGuiContext final
    {
    public:
        ScopedImGuiContext()
        {
            m_previous = ImGui::GetCurrentContext();
            m_context = ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = { 1280.0f, 900.0f };
            io.DeltaTime = 1.0f / 60.0f;
            io.ConfigInputTrickleEventQueue = false;
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }

        ~ScopedImGuiContext()
        {
            ImGui::DestroyContext(m_context);
            ImGui::SetCurrentContext(m_previous);
        }

        ScopedImGuiContext(const ScopedImGuiContext&) = delete;
        ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

    private:
        ImGuiContext* m_previous = nullptr;
        ImGuiContext* m_context = nullptr;
    };

    inline void RenderFrame(const std::function<void()>& draw)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0.0f, 0.0f });
        ImGui::SetNextWindowSize({ 1200.0f, 840.0f });
        ImGui::Begin("UIDesignerTestWindow", nullptr, ImGuiWindowFlags_NoDecoration);
        draw();
        ImGui::End();
        ImGui::Render();
    }

    inline std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }
}
