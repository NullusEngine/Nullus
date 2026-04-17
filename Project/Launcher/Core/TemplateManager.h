#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace NLS
{

struct ProjectTemplate
{
    std::string id;                     // Directory name
    std::string name;                   // Display name from template.json
    std::string description;            // Description from template.json
    std::string previewImagePath;       // Relative path to preview image (may be empty)
    std::string category = "Core";      // Template category (Core, Sample, Learning, etc.)
    std::filesystem::path templateDirectory; // Absolute path to template directory
    int sortOrder = 0;

    /**
     * Check if this template has initial content to copy
     */
    bool HasContent() const;

    /**
     * Get the absolute path to the content directory (may not exist)
     */
    std::filesystem::path GetContentPath() const;
};

class TemplateManager
{
public:
    /**
     * Scan a template root directory and load all valid templates
     * @param templateRoot Absolute path to the templates root directory
     */
    void LoadTemplates(const std::filesystem::path& templateRoot);

    /**
     * Get all loaded templates, sorted by sortOrder
     */
    const std::vector<ProjectTemplate>& GetTemplates() const { return m_templates; }

    /**
     * Find a template by its id (directory name)
     */
    const ProjectTemplate* GetTemplateById(const std::string& id) const;

private:
    std::vector<ProjectTemplate> m_templates;
};

} // namespace NLS
