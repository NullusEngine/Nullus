# The SDK-style projects are the IDE/source surfaces. Native builds use the
# output-producing rules included by Runtime/Scripting/CMakeLists.txt so these
# projects do not become duplicate CMake wrapper projects.
set(NLS_MANAGED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    include_external_msproject(
        Nullus.ScriptGenerator
        "${NLS_MANAGED_ROOT}/Nullus.ScriptGenerator/Nullus.ScriptGenerator.csproj"
        PLATFORM AnyCPU)
    include_external_msproject(
        Nullus.Managed
        "${NLS_MANAGED_ROOT}/Nullus.Managed/Nullus.Managed.csproj"
        PLATFORM AnyCPU)
    include_external_msproject(
        Nullus.EngineScripts
        "${NLS_MANAGED_ROOT}/Nullus.EngineScripts/Nullus.EngineScripts.csproj"
        PLATFORM AnyCPU)
    foreach(_nls_managed_ide_target
            Nullus.ScriptGenerator
            Nullus.Managed
            Nullus.EngineScripts)
        set_property(TARGET ${_nls_managed_ide_target} PROPERTY FOLDER Managed)
        # The C# projects are source/debug surfaces only. Runtime artifacts are
        # produced by ManagedBuild.cmake as part of NLS_Scripting, so do not
        # let the Visual Studio solution schedule these external projects in
        # the default ALL_BUILD graph as well.
        set_property(TARGET ${_nls_managed_ide_target}
            PROPERTY EXCLUDE_FROM_ALL TRUE)
        set_property(TARGET ${_nls_managed_ide_target}
            PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
        # SDK-style projects intentionally expose only Debug/Release. Map the
        # extra CMake configurations instead of emitting invalid AnyCPU
        # configurations into Nullus.sln.
        set_property(TARGET ${_nls_managed_ide_target}
            PROPERTY MAP_IMPORTED_CONFIG_MINSIZEREL Release)
        set_property(TARGET ${_nls_managed_ide_target}
            PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Debug)
        foreach(_nls_managed_configuration DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
            set_property(TARGET ${_nls_managed_ide_target}
                PROPERTY EXCLUDE_FROM_DEFAULT_BUILD_${_nls_managed_configuration} TRUE)
        endforeach()
    endforeach()
endif()
