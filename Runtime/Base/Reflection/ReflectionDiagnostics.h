#pragma once

#include "BaseDef.h"
#include "TypeKey.h"

#include <string>
#include <vector>

namespace NLS::meta
{
    enum class ReflectionDiagnosticSeverity
    {
        Warning,
        Error
    };

    struct ReflectionDiagnostic
    {
        ReflectionDiagnosticSeverity severity = ReflectionDiagnosticSeverity::Warning;
        TypeKey moduleKey = InvalidTypeKey;
        std::string moduleName;
        std::string typeName;
        std::string subject;
        std::string message;
    };

    struct ReflectionDiagnosticCounts
    {
        std::size_t warnings = 0;
        std::size_t errors = 0;
        std::size_t total = 0;
    };

    class NLS_BASE_API ReflectionDiagnostics
    {
    public:
        static void Report(
            ReflectionDiagnosticSeverity severity,
            TypeKey moduleKey,
            const char* moduleName,
            const char* typeName,
            const char* subject,
            const char* message
        );

        static const std::vector<ReflectionDiagnostic>& Get();
        static std::vector<ReflectionDiagnostic> Snapshot();
        static ReflectionDiagnosticCounts Count();
        static std::string Format(const ReflectionDiagnostic& diagnostic);
        static void Clear();
        static bool HasErrors();
    };
}
