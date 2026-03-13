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
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(NLS_META_PARSER_RUNTIME_ID "win-arm64")
    else()
        set(NLS_META_PARSER_RUNTIME_ID "win-x64")
    endif()
    set(NLS_META_PARSER_EXECUTABLE "${NLS_META_PARSER_OUTPUT_DIR}/MetaParser.exe" CACHE INTERNAL "MetaParser executable path" FORCE)
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(NLS_META_PARSER_RUNTIME_ID "osx-arm64")
    else()
        set(NLS_META_PARSER_RUNTIME_ID "osx-x64")
    endif()
    set(NLS_META_PARSER_EXECUTABLE "${NLS_META_PARSER_OUTPUT_DIR}/MetaParser" CACHE INTERNAL "MetaParser executable path" FORCE)
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(NLS_META_PARSER_RUNTIME_ID "linux-arm64")
    else()
        set(NLS_META_PARSER_RUNTIME_ID "linux-x64")
    endif()
    set(NLS_META_PARSER_EXECUTABLE "${NLS_META_PARSER_OUTPUT_DIR}/MetaParser" CACHE INTERNAL "MetaParser executable path" FORCE)
endif()

add_custom_target(MetaParser ALL
    COMMAND "${DOTNET_EXECUTABLE}" publish "${NLS_META_PARSER_PROJECT}" --configuration "$<CONFIG>" --runtime "${NLS_META_PARSER_RUNTIME_ID}" --self-contained false --nologo -o "${NLS_META_PARSER_OUTPUT_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building C# MetaParser (CppAst/libclang)"
    VERBATIM
)

set_target_properties(MetaParser PROPERTIES FOLDER Tools)
