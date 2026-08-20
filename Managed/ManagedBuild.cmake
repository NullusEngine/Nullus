if(NOT NLS_DOTNET_EXECUTABLE)
    message(FATAL_ERROR "NLS_DOTNET_EXECUTABLE was not resolved; C# scripting requires the repository's .NET 8 SDK.")
endif()

set(NLS_MANAGED_CONFIGURATION "$<CONFIG>")
set(NLS_MANAGED_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Managed/$<CONFIG>")
set(NLS_MANAGED_PROJECT_ASSETS_ARG)
if(NLS_PROJECT_ASSETS_DIR)
    list(APPEND NLS_MANAGED_PROJECT_ASSETS_ARG
        "-p:NullusProjectAssets=${NLS_PROJECT_ASSETS_DIR}")
endif()

set(NLS_MANAGED_ROOT "${CMAKE_CURRENT_LIST_DIR}")
if(WIN32)
    set(NLS_DOTNET_RUNNER "${CMAKE_SOURCE_DIR}/Tools/RunRepositoryDotnet.cmd")
else()
    set(NLS_DOTNET_RUNNER "${CMAKE_SOURCE_DIR}/Tools/RunRepositoryDotnet.sh")
endif()
set(NLS_MANAGED_SCHEMA_JSON "${CMAKE_SOURCE_DIR}/Library/ScriptApi/ScriptApi.json")
set(NLS_MANAGED_SCHEMA_BIN "${CMAKE_SOURCE_DIR}/Library/ScriptApi/ScriptApi.bin")

file(GLOB_RECURSE NLS_MANAGED_RUNTIME_SOURCE_FILES CONFIGURE_DEPENDS
    "${NLS_MANAGED_ROOT}/Nullus.Managed/*.cs"
    "${NLS_MANAGED_ROOT}/Nullus.ScriptGenerator/*.cs")
file(GLOB_RECURSE NLS_GAMESCRIPTS_SOURCE_FILES CONFIGURE_DEPENDS
    "${NLS_MANAGED_ROOT}/Nullus.GameScripts/*.cs")
file(GLOB_RECURSE NLS_ENGINESCRIPTS_SOURCE_FILES CONFIGURE_DEPENDS
    "${NLS_MANAGED_ROOT}/Nullus.EngineScripts/*.cs")
file(GLOB_RECURSE NLS_SCRIPT_ANALYSIS_SOURCE_FILES CONFIGURE_DEPENDS
    "${NLS_MANAGED_ROOT}/Nullus.ScriptAnalysis/*.cs")
list(FILTER NLS_MANAGED_RUNTIME_SOURCE_FILES EXCLUDE REGEX [[/(bin|obj)/]])
list(FILTER NLS_GAMESCRIPTS_SOURCE_FILES EXCLUDE REGEX [[/(bin|obj)/]])
list(FILTER NLS_ENGINESCRIPTS_SOURCE_FILES EXCLUDE REGEX [[/(bin|obj)/]])
list(FILTER NLS_SCRIPT_ANALYSIS_SOURCE_FILES EXCLUDE REGEX [[/(bin|obj)/]])
if(NLS_PROJECT_ASSETS_DIR)
    file(GLOB_RECURSE NLS_PROJECT_SCRIPT_SOURCE_FILES CONFIGURE_DEPENDS
        "${NLS_PROJECT_ASSETS_DIR}/*.cs")
else()
    file(GLOB_RECURSE NLS_PROJECT_SCRIPT_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/TestProject/Assets/*.cs")
endif()
list(FILTER NLS_PROJECT_SCRIPT_SOURCE_FILES EXCLUDE REGEX [[/(bin|obj)/]])

set(NLS_MANAGED_GENERATOR_DLL "${NLS_MANAGED_OUTPUT_DIR}/Nullus.ScriptGenerator.dll")
set(NLS_MANAGED_RUNTIME_DLL "${NLS_MANAGED_OUTPUT_DIR}/Nullus.Managed.dll")
set(NLS_MANAGED_RUNTIME_PDB "${NLS_MANAGED_OUTPUT_DIR}/Nullus.Managed.pdb")
set(NLS_MANAGED_RUNTIME_DEPS "${NLS_MANAGED_OUTPUT_DIR}/Nullus.Managed.deps.json")
set(NLS_MANAGED_STAMP "${CMAKE_CURRENT_BINARY_DIR}/NullusManaged.stamp")
set(NLS_SCRIPT_ANALYSIS_DLL "${NLS_MANAGED_OUTPUT_DIR}/Nullus.ScriptAnalysis.dll")
set(NLS_SCRIPT_ANALYSIS_PDB "${NLS_MANAGED_OUTPUT_DIR}/Nullus.ScriptAnalysis.pdb")
set(NLS_SCRIPT_ANALYSIS_DEPS "${NLS_MANAGED_OUTPUT_DIR}/Nullus.ScriptAnalysis.deps.json")
set(NLS_SCRIPT_ANALYSIS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/NullusScriptAnalysis.stamp")

# ScriptAnalysis is consumed by the project workspace and the C# compiler.
# Build it from the repository SDK as part of the existing NLS_Scripting
# file-producing graph so a clean checkout never falls back to a stale bin/
# copy or a network restore.
add_custom_command(
    OUTPUT
        "${NLS_SCRIPT_ANALYSIS_STAMP}"
        "${NLS_SCRIPT_ANALYSIS_DLL}"
        "${NLS_SCRIPT_ANALYSIS_PDB}"
        "${NLS_SCRIPT_ANALYSIS_DEPS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NLS_MANAGED_OUTPUT_DIR}"
    COMMAND "${NLS_DOTNET_RUNNER}"
        "${NLS_DOTNET_EXECUTABLE}" build
        "${NLS_MANAGED_ROOT}/Nullus.ScriptAnalysis/Nullus.ScriptAnalysis.csproj"
        --configuration "${NLS_MANAGED_CONFIGURATION}"
        --output "${NLS_MANAGED_OUTPUT_DIR}"
        --no-restore
        --nologo
    COMMAND ${CMAKE_COMMAND} -E touch "${NLS_SCRIPT_ANALYSIS_STAMP}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS
        "${NLS_MANAGED_ROOT}/Nullus.ScriptAnalysis/Nullus.ScriptAnalysis.csproj"
        ${NLS_SCRIPT_ANALYSIS_SOURCE_FILES}
    COMMENT "Building Nullus Roslyn script analyzer"
    VERBATIM)

# Build the shared managed runtime as a file-producing rule owned by the
# existing NLS_Scripting target. This is incremental and creates no empty VS
# project.
add_custom_command(
    OUTPUT
        "${NLS_MANAGED_STAMP}"
        "${NLS_MANAGED_GENERATOR_DLL}"
        "${NLS_MANAGED_RUNTIME_DLL}"
        "${NLS_MANAGED_RUNTIME_PDB}"
        "${NLS_MANAGED_RUNTIME_DEPS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NLS_MANAGED_OUTPUT_DIR}"
    COMMAND "${NLS_DOTNET_RUNNER}"
        "${NLS_DOTNET_EXECUTABLE}" build
        "${NLS_MANAGED_ROOT}/Nullus.ScriptGenerator/Nullus.ScriptGenerator.csproj"
        --configuration "${NLS_MANAGED_CONFIGURATION}"
        --output "${NLS_MANAGED_OUTPUT_DIR}"
        --no-restore
        --nologo
    COMMAND "${NLS_DOTNET_RUNNER}"
        "${NLS_DOTNET_EXECUTABLE}" build
        "${NLS_MANAGED_ROOT}/Nullus.Managed/Nullus.Managed.csproj"
        --configuration "${NLS_MANAGED_CONFIGURATION}"
        --output "${NLS_MANAGED_OUTPUT_DIR}"
        -p:NullusUsePrebuiltScriptGenerator=true
        -p:NullusForceScriptGenerator=true
        "-p:NullusScriptGeneratorPath=${NLS_MANAGED_GENERATOR_DLL}"
        -p:NullusUseScriptAnalysis=false
        ${NLS_MANAGED_PROJECT_ASSETS_ARG}
        --no-restore
        --nologo
    COMMAND ${CMAKE_COMMAND} -E touch "${NLS_MANAGED_STAMP}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS
        NLS_Engine
        "${NLS_MANAGED_SCHEMA_JSON}"
        "${NLS_MANAGED_SCHEMA_BIN}"
        "${NLS_MANAGED_ROOT}/Nullus.ScriptGenerator/Nullus.ScriptGenerator.csproj"
        "${NLS_MANAGED_ROOT}/Nullus.Managed/Nullus.Managed.csproj"
        ${NLS_MANAGED_RUNTIME_SOURCE_FILES}
    COMMENT "Building Nullus managed runtime and generated script bindings"
    VERBATIM)

set(NLS_ENGINESCRIPTS_DLL "${NLS_MANAGED_OUTPUT_DIR}/EngineScripts.dll")
set(NLS_ENGINESCRIPTS_PDB "${NLS_MANAGED_OUTPUT_DIR}/EngineScripts.pdb")
set(NLS_ENGINESCRIPTS_DEPS "${NLS_MANAGED_OUTPUT_DIR}/EngineScripts.deps.json")
set(NLS_ENGINESCRIPTS_RUNTIME_CONFIG "${NLS_MANAGED_OUTPUT_DIR}/EngineScripts.runtimeconfig.json")
set(NLS_ENGINESCRIPTS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/EngineScripts.stamp")

add_custom_command(
    OUTPUT
        "${NLS_ENGINESCRIPTS_STAMP}"
        "${NLS_ENGINESCRIPTS_DLL}"
        "${NLS_ENGINESCRIPTS_PDB}"
        "${NLS_ENGINESCRIPTS_DEPS}"
        "${NLS_ENGINESCRIPTS_RUNTIME_CONFIG}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NLS_MANAGED_OUTPUT_DIR}"
    COMMAND "${NLS_DOTNET_RUNNER}"
        "${NLS_DOTNET_EXECUTABLE}" build
        "${NLS_MANAGED_ROOT}/Nullus.EngineScripts/Nullus.EngineScripts.csproj"
        --configuration "${NLS_MANAGED_CONFIGURATION}"
        --output "${NLS_MANAGED_OUTPUT_DIR}"
        -p:NullusUsePrebuiltScriptGenerator=true
        -p:NullusForceScriptGenerator=true
        "-p:NullusScriptGeneratorPath=${NLS_MANAGED_GENERATOR_DLL}"
        -p:NullusUseScriptAnalysis=false
        --no-restore
        --nologo
    COMMAND ${CMAKE_COMMAND} -E touch "${NLS_ENGINESCRIPTS_STAMP}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS
        "${NLS_MANAGED_STAMP}"
        "${NLS_MANAGED_SCHEMA_JSON}"
        "${NLS_MANAGED_SCHEMA_BIN}"
        "${NLS_MANAGED_ROOT}/Nullus.EngineScripts/Nullus.EngineScripts.csproj"
        ${NLS_MANAGED_RUNTIME_SOURCE_FILES}
        ${NLS_ENGINESCRIPTS_SOURCE_FILES}
    COMMENT "Building Nullus built-in EngineScripts assembly"
    VERBATIM)

set(NLS_GAMESCRIPTS_DLL "${NLS_MANAGED_OUTPUT_DIR}/GameScripts.dll")
set(NLS_GAMESCRIPTS_PDB "${NLS_MANAGED_OUTPUT_DIR}/GameScripts.pdb")
set(NLS_GAMESCRIPTS_DEPS "${NLS_MANAGED_OUTPUT_DIR}/GameScripts.deps.json")
set(NLS_GAMESCRIPTS_RUNTIME_CONFIG "${NLS_MANAGED_OUTPUT_DIR}/GameScripts.runtimeconfig.json")
set(NLS_GAMESCRIPTS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/GameScripts.stamp")

# Keep a deterministic bootstrap assembly for non-debug Editor startup and
# command-line smoke tests. It is an output rule of NLS_Scripting, not a
# separate NullusGameScripts project.
add_custom_command(
    OUTPUT
        "${NLS_GAMESCRIPTS_STAMP}"
        "${NLS_GAMESCRIPTS_DLL}"
        "${NLS_GAMESCRIPTS_PDB}"
        "${NLS_GAMESCRIPTS_DEPS}"
        "${NLS_GAMESCRIPTS_RUNTIME_CONFIG}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NLS_MANAGED_OUTPUT_DIR}"
    COMMAND "${NLS_DOTNET_RUNNER}"
        "${NLS_DOTNET_EXECUTABLE}" build
        "${NLS_MANAGED_ROOT}/Nullus.GameScripts/Nullus.GameScripts.csproj"
        --configuration "${NLS_MANAGED_CONFIGURATION}"
        --output "${NLS_MANAGED_OUTPUT_DIR}"
        -p:NullusUsePrebuiltScriptGenerator=true
        -p:NullusForceScriptGenerator=true
        "-p:NullusScriptGeneratorPath=${NLS_MANAGED_GENERATOR_DLL}"
        "-p:NullusEngineScriptsPath=${NLS_ENGINESCRIPTS_DLL}"
        -p:NullusUseScriptAnalysis=false
        ${NLS_MANAGED_PROJECT_ASSETS_ARG}
        --no-restore
        --nologo
    COMMAND ${CMAKE_COMMAND} -E touch "${NLS_GAMESCRIPTS_STAMP}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS
        "${NLS_MANAGED_STAMP}"
        "${NLS_ENGINESCRIPTS_STAMP}"
        "${NLS_MANAGED_SCHEMA_JSON}"
        "${NLS_MANAGED_SCHEMA_BIN}"
        "${NLS_MANAGED_ROOT}/Nullus.GameScripts/Nullus.GameScripts.csproj"
        ${NLS_MANAGED_RUNTIME_SOURCE_FILES}
        ${NLS_GAMESCRIPTS_SOURCE_FILES}
        ${NLS_PROJECT_SCRIPT_SOURCE_FILES}
    COMMENT "Building bootstrap GameScripts assembly"
    VERBATIM)

# Visual Studio's CMake generator requires a source placeholder for custom
# build items that do not exist during configure. The commands above own these
# files and replace them on the first build.
foreach(_nls_managed_stamp IN ITEMS
        "${NLS_SCRIPT_ANALYSIS_STAMP}"
        "${NLS_MANAGED_STAMP}"
        "${NLS_ENGINESCRIPTS_STAMP}"
        "${NLS_GAMESCRIPTS_STAMP}")
    if(NOT EXISTS "${_nls_managed_stamp}")
        file(WRITE "${_nls_managed_stamp}" "Nullus managed build stamp\n")
    endif()
endforeach()

set_source_files_properties(
    "${NLS_SCRIPT_ANALYSIS_STAMP}"
    "${NLS_SCRIPT_ANALYSIS_DLL}"
    "${NLS_SCRIPT_ANALYSIS_PDB}"
    "${NLS_SCRIPT_ANALYSIS_DEPS}"
    "${NLS_MANAGED_STAMP}"
    "${NLS_MANAGED_GENERATOR_DLL}"
    "${NLS_MANAGED_RUNTIME_DLL}"
    "${NLS_MANAGED_RUNTIME_PDB}"
    "${NLS_MANAGED_RUNTIME_DEPS}"
    "${NLS_ENGINESCRIPTS_STAMP}"
    "${NLS_ENGINESCRIPTS_DLL}"
    "${NLS_ENGINESCRIPTS_PDB}"
    "${NLS_ENGINESCRIPTS_DEPS}"
    "${NLS_ENGINESCRIPTS_RUNTIME_CONFIG}"
    "${NLS_GAMESCRIPTS_STAMP}"
    "${NLS_GAMESCRIPTS_DLL}"
    "${NLS_GAMESCRIPTS_PDB}"
    "${NLS_GAMESCRIPTS_DEPS}"
    "${NLS_GAMESCRIPTS_RUNTIME_CONFIG}"
    PROPERTIES GENERATED TRUE)

target_sources(NLS_Scripting PRIVATE
    "${NLS_SCRIPT_ANALYSIS_STAMP}"
    "${NLS_MANAGED_STAMP}"
    "${NLS_ENGINESCRIPTS_STAMP}"
    "${NLS_GAMESCRIPTS_STAMP}")
add_dependencies(NLS_Scripting NLS_Engine)
