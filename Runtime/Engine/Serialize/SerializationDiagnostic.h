#pragma once

#include <string>
#include <vector>

namespace NLS::Engine::Serialize
{
    enum class SerializationDiagnosticCode
    {
        UnsupportedFormat,
        UnsupportedVersion,
        UnknownType,
        MissingObject,
        DuplicateObjectId,
        InvalidGuid,
        InvalidPropertyType,
        MissingAsset,
        DanglingReference,
        InvalidPrefabOverride,
        OwnershipCycle,
        OrphanedOwnedObject
    };

    enum class SerializationDiagnosticSeverity
    {
        Info,
        Warning,
        Error
    };

    class SerializationDiagnostic
    {
    public:
        SerializationDiagnostic(
            SerializationDiagnosticCode code,
            SerializationDiagnosticSeverity severity,
            std::string message)
            : m_code(code)
            , m_severity(severity)
            , m_message(std::move(message))
        {
        }

        SerializationDiagnosticCode GetCode() const
        {
            return m_code;
        }

        SerializationDiagnosticSeverity GetSeverity() const
        {
            return m_severity;
        }

        const std::string& GetMessage() const
        {
            return m_message;
        }

        bool IsError() const
        {
            return m_severity == SerializationDiagnosticSeverity::Error;
        }

    private:
        SerializationDiagnosticCode m_code;
        SerializationDiagnosticSeverity m_severity;
        std::string m_message;
    };

    class SerializationDiagnosticList
    {
    public:
        void Add(SerializationDiagnostic diagnostic)
        {
            m_items.push_back(std::move(diagnostic));
        }

        bool HasErrors() const
        {
            for (const auto& diagnostic : m_items)
            {
                if (diagnostic.IsError())
                    return true;
            }
            return false;
        }

        const std::vector<SerializationDiagnostic>& GetItems() const
        {
            return m_items;
        }

    private:
        std::vector<SerializationDiagnostic> m_items;
    };
}
