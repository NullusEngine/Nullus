cmake_minimum_required(VERSION 3.18)

project(MetaParser LANGUAGES NONE)

find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
if(NOT DOTNET_EXECUTABLE)
    message(FATAL_ERROR "dotnet SDK is required for C# MetaParser. Please install .NET 8 SDK.")
endif()

set(NLS_META_PARSER_DOTNET "${DOTNET_EXECUTABLE}" CACHE INTERNAL "dotnet executable")
set(NLS_META_PARSER_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/src/MetaParser.csproj" CACHE INTERNAL "MetaParser csproj")

add_custom_target(MetaParser
    COMMAND "${DOTNET_EXECUTABLE}" build "${NLS_META_PARSER_PROJECT}" --configuration "$<CONFIG>" --nologo
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building C# MetaParser (CppAst + T4 template renderer)"
    VERBATIM
)

set_target_properties(MetaParser PROPERTIES FOLDER Tools)
