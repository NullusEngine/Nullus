#include "Precompiled.h"

#include "ReflectionDiagnostics.h"

#include <mutex>
#include <sstream>

namespace NLS::meta
{
    namespace
    {
        std::vector<ReflectionDiagnostic>& Diagnostics()
        {
            static std::vector<ReflectionDiagnostic> diagnostics;
            return diagnostics;
        }

        std::mutex& DiagnosticsMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
    }

    void ReflectionDiagnostics::Report(
        ReflectionDiagnosticSeverity severity,
        TypeKey moduleKey,
        const char* moduleName,
        const char* typeName,
        const char* subject,
        const char* message
    )
    {
        std::scoped_lock lock(DiagnosticsMutex());
        Diagnostics().push_back({
            severity,
            moduleKey,
            moduleName != nullptr ? moduleName : "",
            typeName != nullptr ? typeName : "",
            subject != nullptr ? subject : "",
            message != nullptr ? message : ""
        });
    }

    const std::vector<ReflectionDiagnostic>& ReflectionDiagnostics::Get()
    {
        return Diagnostics();
    }

    std::vector<ReflectionDiagnostic> ReflectionDiagnostics::Snapshot()
    {
        std::scoped_lock lock(DiagnosticsMutex());
        return Diagnostics();
    }

    ReflectionDiagnosticCounts ReflectionDiagnostics::Count()
    {
        std::scoped_lock lock(DiagnosticsMutex());
        ReflectionDiagnosticCounts counts;
        for (const auto& diagnostic : Diagnostics())
        {
            if (diagnostic.severity == ReflectionDiagnosticSeverity::Error)
                ++counts.errors;
            else
                ++counts.warnings;
        }

        counts.total = counts.errors + counts.warnings;
        return counts;
    }

    std::string ReflectionDiagnostics::Format(const ReflectionDiagnostic& diagnostic)
    {
        std::ostringstream stream;
        stream
            << (diagnostic.severity == ReflectionDiagnosticSeverity::Error ? "Error" : "Warning")
            << " module=" << diagnostic.moduleName
            << " type=" << diagnostic.typeName
            << " subject=" << diagnostic.subject
            << " message=" << diagnostic.message;
        return stream.str();
    }

    void ReflectionDiagnostics::Clear()
    {
        std::scoped_lock lock(DiagnosticsMutex());
        Diagnostics().clear();
    }

    bool ReflectionDiagnostics::HasErrors()
    {
        std::scoped_lock lock(DiagnosticsMutex());
        for (const auto& diagnostic : Diagnostics())
        {
            if (diagnostic.severity == ReflectionDiagnosticSeverity::Error)
                return true;
        }

        return false;
    }
}
