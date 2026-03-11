cmake_minimum_required(VERSION 3.18)

project(MetaParser LANGUAGES NONE)

find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
if(NOT DOTNET_EXECUTABLE)
    message(FATAL_ERROR "dotnet SDK is required for C# MetaParser. Please install .NET 8 SDK.")
endif()

set(NLS_META_PARSER_DOTNET "${DOTNET_EXECUTABLE}" CACHE INTERNAL "dotnet executable" FORCE)
set(NLS_META_PARSER_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/src/MetaParser.csproj" CACHE INTERNAL "MetaParser csproj" FORCE)
set(NLS_META_PARSER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/publish/$<CONFIG>" CACHE INTERNAL "MetaParser output directory" FORCE)

if(WIN32)
    set(NLS_META_PARSER_EXECUTABLE "${NLS_META_PARSER_OUTPUT_DIR}/MetaParser.exe" CACHE INTERNAL "MetaParser executable path" FORCE)
else()
    set(NLS_META_PARSER_EXECUTABLE "${NLS_META_PARSER_OUTPUT_DIR}/MetaParser" CACHE INTERNAL "MetaParser executable path" FORCE)
endif()

add_custom_target(MetaParser ALL
    COMMAND "${DOTNET_EXECUTABLE}" publish "${NLS_META_PARSER_PROJECT}" --configuration "$<CONFIG>" --nologo -o "${NLS_META_PARSER_OUTPUT_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building C# MetaParser (CppAst/libclang)"
    VERBATIM
)

set_target_properties(MetaParser PROPERTIES FOLDER Tools)
