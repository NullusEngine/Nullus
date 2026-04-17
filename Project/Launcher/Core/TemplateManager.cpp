#include "TemplateManager.h"

#include <algorithm>
#include <fstream>

#include <json11.hpp>

#include <Debug/Logger.h>

namespace NLS
{

bool ProjectTemplate::HasContent() const
{
    return std::filesystem::exists(GetContentPath());
}

std::filesystem::path ProjectTemplate::GetContentPath() const
{
    return templateDirectory / "content";
}

void TemplateManager::LoadTemplates(const std::filesystem::path& templateRoot)
{
    m_templates.clear();

    if (!std::filesystem::exists(templateRoot) || !std::filesystem::is_directory(templateRoot))
    {
        NLS_LOG_WARNING("TemplateManager: Template root does not exist: " + templateRoot.string());
        return;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(templateRoot, ec))
    {
        if (!entry.is_directory())
            continue;

        const auto templateDir = entry.path();
        const std::string templateId = templateDir.filename().string();

        // Validate id format: [a-zA-Z0-9_-]
        bool validId = !templateId.empty();
        for (char c : templateId)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            {
                validId = false;
                break;
            }
        }
        if (!validId)
            continue;

        // Read template.json
        auto jsonPath = templateDir / "template.json";
        if (!std::filesystem::exists(jsonPath))
            continue;

        std::ifstream ifs(jsonPath);
        if (!ifs.is_open())
            continue;

        std::string jsonContent((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
        ifs.close();

        std::string parseError;
        auto json = json11::Json::parse(jsonContent, parseError);
        if (!parseError.empty())
        {
            NLS_LOG_WARNING("TemplateManager: Failed to parse " + jsonPath.string() + ": " + parseError);
            continue;
        }

        // Validate required fields
        std::string name = json["name"].string_value();
        std::string description = json["description"].string_value();

        if (name.empty() || name.size() > 64)
            continue;
        if (description.empty() || description.size() > 256)
            continue;

        ProjectTemplate tmpl;
        tmpl.id = templateId;
        tmpl.name = name;
        tmpl.description = description;
        tmpl.templateDirectory = templateDir;
        tmpl.sortOrder = json["sortOrder"].int_value();

        // Optional category (defaults to "Core")
        std::string category = json["category"].string_value();
        if (!category.empty())
            tmpl.category = category;

        // Optional preview path
        std::string preview = json["preview"].string_value();
        if (!preview.empty())
        {
            auto previewPath = templateDir / preview;
            if (std::filesystem::exists(previewPath))
                tmpl.previewImagePath = previewPath.string();
        }

        m_templates.push_back(std::move(tmpl));
    }

    // Sort by sortOrder ascending
    std::sort(m_templates.begin(), m_templates.end(),
        [](const ProjectTemplate& a, const ProjectTemplate& b)
        {
            return a.sortOrder < b.sortOrder;
        });

    NLS_LOG_INFO("TemplateManager: Loaded " + std::to_string(m_templates.size()) + " templates from " + templateRoot.string());
}

const ProjectTemplate* TemplateManager::GetTemplateById(const std::string& id) const
{
    for (const auto& tmpl : m_templates)
    {
        if (tmpl.id == id)
            return &tmpl;
    }
    return nullptr;
}

} // namespace NLS
