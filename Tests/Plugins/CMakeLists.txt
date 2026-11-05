cmake_minimum_required(VERSION 3.22)
set(CMAKE_FOLDER "Tests/Plugins")
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NullusPluginValidation LANGUAGES C CXX)
    enable_testing()
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    get_filename_component(NLS_PLUGIN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory("${NLS_PLUGIN_ROOT}/ThirdParty/assimp/contrib/googletest" "${CMAKE_BINARY_DIR}/gtest" EXCLUDE_FROM_ALL)
else()
    set(NLS_PLUGIN_ROOT "${CMAKE_SOURCE_DIR}")
endif()
if(NOT TARGET NullusPluginSDK)
    add_library(NullusPluginSDK INTERFACE)
    target_include_directories(NullusPluginSDK INTERFACE "${NLS_PLUGIN_ROOT}/Runtime/Plugins/SDK")
endif()
if(NOT TARGET zstd)
    add_subdirectory("${NLS_PLUGIN_ROOT}/ThirdParty/zstd" "${CMAKE_CURRENT_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)
endif()
include("${NLS_PLUGIN_ROOT}/Tools/CMake/NullusPackageZip.cmake")
nls_add_package_zip("${NLS_PLUGIN_ROOT}")
include("${NLS_PLUGIN_ROOT}/Tools/CMake/NullusPackageTools.cmake")
add_library(NullusPluginCAbiSmoke OBJECT AbiCHeaderSmoke.c)
target_link_libraries(NullusPluginCAbiSmoke PRIVATE NullusPluginSDK)
set_target_properties(NullusPluginCAbiSmoke PROPERTIES C_STANDARD 11 FOLDER "Tests/Fixtures/Plugins")
if(NOT TARGET NullusRuntimeSample)
    add_subdirectory("${NLS_PLUGIN_ROOT}/Examples/Plugins/RuntimeSample" "${CMAKE_CURRENT_BINARY_DIR}/RuntimeSample")
endif()
include("${NLS_PLUGIN_ROOT}/Runtime/Plugins/SDK/NullusPluginSDK.cmake")
foreach(_fixture RANGE 1 4)
    nullus_add_binary_plugin(NullusPluginAbiFixture${_fixture} AbiFixtures.cpp)
    target_compile_definitions(NullusPluginAbiFixture${_fixture} PRIVATE NLS_FIXTURE_MODE=${_fixture})
    set_target_properties(NullusPluginAbiFixture${_fixture} PROPERTIES FOLDER "Tests/Fixtures/Plugins")
endforeach()

foreach(_mode Static Shared)
    string(TOUPPER "${_mode}" _library_mode)
    add_library(NullusPluginHost${_mode} ${_library_mode}
        "${NLS_PLUGIN_ROOT}/Runtime/Core/Modules/ModuleSystem.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Core/ServiceLocator.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Base/Guid.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Core/Assets/PluginAssetPath.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Core/Assets/ArtifactManifest.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Core/Assets/ContentHash.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Core/Assets/ContentSource.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Core/Assets/NpakArchive.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Platform/Modules/NativeLibrary.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Platform/Process/InterprocessFileLock.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Platform/Process/ProcessRunner.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Plugins/PluginHost.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Plugins/Manifest.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Plugins/PluginManager.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Plugins/ApplicationPlugins.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Plugins/ProjectPluginState.cpp")
    if(_mode STREQUAL "Static")
        # A shared parent build defines NLS_SHARED_LIB directory-wide. Keep
        # this static host and its consumers on the static API declarations.
        target_compile_options(NullusPluginHost${_mode} PUBLIC
            "$<$<CXX_COMPILER_ID:MSVC>:/UNLS_SHARED_LIB>"
            "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNLS_SHARED_LIB>")
    endif()
    target_sources(NullusPluginHost${_mode} PRIVATE "${NLS_PLUGIN_ROOT}/Runtime/Plugins/RuntimeDeployment.cpp")
    target_include_directories(NullusPluginHost${_mode} PUBLIC
        "${NLS_PLUGIN_ROOT}/Runtime" "${NLS_PLUGIN_ROOT}/Runtime/Base"
        "${NLS_PLUGIN_ROOT}/Runtime/Core" "${NLS_PLUGIN_ROOT}/Runtime/Platform"
        "${NLS_PLUGIN_ROOT}/Runtime/Plugins" "${NLS_PLUGIN_ROOT}/ThirdParty")
    target_compile_features(NullusPluginHost${_mode} PUBLIC cxx_std_20)
    target_compile_definitions(NullusPluginHost${_mode} PRIVATE NLS_ENGINE_VERSION_STRING="1.0.0")
    if(_mode STREQUAL "Shared")
        target_compile_definitions(NullusPluginHost${_mode} PUBLIC NLS_SHARED_LIB PRIVATE NLS_BASE_EXPORT NLS_CORE_EXPORT NLS_PLATFORM_EXPORT NLS_PLUGINS_EXPORT)
    endif()
    if(UNIX)
        target_link_libraries(NullusPluginHost${_mode} PRIVATE ${CMAKE_DL_LIBS})
    endif()
    target_link_libraries(NullusPluginHost${_mode} PRIVATE zstd)
    add_executable(NullusPluginProbe${_mode} PluginProbe.cpp)
    add_library(NullusPackageTestSupport${_mode} ${_library_mode}
        "${NLS_PLUGIN_ROOT}/Runtime/Packages/PackageManager.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Packages/RemotePackageSources.cpp"
        "${NLS_PLUGIN_ROOT}/Runtime/Packages/ZipPackageSource.cpp")
    nls_configure_package_tools(NullusPackageTestSupport${_mode})
    target_include_directories(NullusPackageTestSupport${_mode} PUBLIC "${NLS_PLUGIN_ROOT}/Runtime/Packages")
    target_link_libraries(NullusPackageTestSupport${_mode} PUBLIC NullusPluginHost${_mode} PRIVATE NullusPackageZip)
    if(_mode STREQUAL "Shared")
        target_compile_definitions(NullusPackageTestSupport${_mode} PRIVATE NLS_PACKAGES_EXPORT)
    endif()
    target_link_libraries(NullusPluginProbe${_mode} PRIVATE NullusPluginHost${_mode})
    add_executable(NullusPackageProbe${_mode} PackageProbe.cpp)
    target_link_libraries(NullusPackageProbe${_mode} PRIVATE NullusPackageTestSupport${_mode})
    add_executable(NullusPluginsTool${_mode} "${NLS_PLUGIN_ROOT}/Tools/Plugins/NullusPlugins.cpp")
    target_link_libraries(NullusPluginsTool${_mode} PRIVATE NullusPackageTestSupport${_mode})
    add_executable(NullusPluginTests${_mode} ModuleTests.cpp ManifestTests.cpp AssetPathTests.cpp DeploymentTests.cpp PackageTests.cpp)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(NLS_TEST_OPENSSL_EXECUTABLE NAMES openssl HINTS "C:/Program Files/Git/usr/bin")
    if(Python3_Interpreter_FOUND AND NLS_TEST_OPENSSL_EXECUTABLE AND NLS_PACKAGE_GIT_EXECUTABLE AND NLS_PACKAGE_CURL_EXECUTABLE)
        target_sources(NullusPluginTests${_mode} PRIVATE RemotePackageTests.cpp)
        target_compile_definitions(NullusPluginTests${_mode} PRIVATE
            NLS_TEST_PYTHON="${Python3_EXECUTABLE}" NLS_TEST_OPENSSL="${NLS_TEST_OPENSSL_EXECUTABLE}"
            NLS_REMOTE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/RemoteFixture.py")
    endif()
    target_link_libraries(NullusPluginTests${_mode} PRIVATE NullusPluginHost${_mode} NullusPackageTestSupport${_mode} gtest_main)
    target_compile_definitions(NullusPluginTests${_mode} PRIVATE NLS_PACKAGE_PROBE="$<TARGET_FILE:NullusPackageProbe${_mode}>")
    add_dependencies(NullusPluginTests${_mode} NullusPackageProbe${_mode} NullusPluginsTool${_mode})
    target_compile_definitions(NullusPluginTests${_mode} PRIVATE NLS_TEST_PLUGIN_PATH="$<TARGET_FILE:NullusRuntimeSample>")
    foreach(_fixture RANGE 1 4)
        target_compile_definitions(NullusPluginTests${_mode} PRIVATE NLS_TEST_ABI_FIXTURE_${_fixture}="$<TARGET_FILE:NullusPluginAbiFixture${_fixture}>")
        add_dependencies(NullusPluginTests${_mode} NullusPluginAbiFixture${_fixture})
    endforeach()
    target_compile_definitions(NullusPluginTests${_mode} PRIVATE NLS_PLUGIN_EXAMPLES="${NLS_PLUGIN_ROOT}/Examples/Plugins")
    target_compile_definitions(NullusPluginTests${_mode} PRIVATE NLS_TEST_PLUGIN_PROBE="$<TARGET_FILE:NullusPluginProbe${_mode}>")
    if(_mode STREQUAL "Shared")
        target_compile_definitions(NullusPluginTests${_mode} PRIVATE NLS_TEST_HOST_DLL="$<TARGET_FILE:NullusPluginHostShared>")
    endif()
    add_dependencies(NullusPluginTests${_mode} NullusPluginProbe${_mode})
    add_dependencies(NullusPluginTests${_mode} NullusRuntimeSample)
    add_dependencies(NullusPluginTests${_mode} NullusPluginCAbiSmoke)
    add_test(NAME NullusPluginTests${_mode} COMMAND NullusPluginTests${_mode})
    set_tests_properties(NullusPluginTests${_mode} PROPERTIES LABELS "plugins;abi;${_mode}")
    set_target_properties(NullusPluginHost${_mode} NullusPackageTestSupport${_mode}
        NullusPluginProbe${_mode} NullusPackageProbe${_mode} NullusPluginsTool${_mode}
        PROPERTIES FOLDER "Tests/Fixtures/Plugins")
    if(Python3_Interpreter_FOUND)
        add_test(NAME NullusPluginCMakeInputs${_mode}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/CMakeInputs.py"
                "${NLS_PLUGIN_ROOT}" "$<TARGET_FILE:NullusPluginsTool${_mode}>" "${CMAKE_COMMAND}" "${CMAKE_CURRENT_BINARY_DIR}/cmake-tests"
                "$<TARGET_LINKER_FILE:NullusPluginHost${_mode}>" "$<TARGET_LINKER_FILE:zstd>" "${_mode}")
        set_tests_properties(NullusPluginCMakeInputs${_mode} PROPERTIES TIMEOUT 240)
    endif()
endforeach()
if(TARGET NullusUnitTests AND TARGET NLS_Scripting AND NLS_ENABLE_CSHARP_SCRIPTING)
    get_filename_component(_plugin_dotnet_root "${NLS_DOTNET_EXECUTABLE}" DIRECTORY)
    set(_plugin_sample_output "${CMAKE_CURRENT_BINARY_DIR}/ManagedSample/$<CONFIG>")
    add_custom_target(NullusPluginManagedSample
        COMMAND "${NLS_DOTNET_EXECUTABLE}" build "${NLS_PLUGIN_ROOT}/Examples/Plugins/CSharpRuntime/Nullus.SamplePlugin.csproj"
            --configuration $<IF:$<CONFIG:Debug>,Debug,Release> --output "${_plugin_sample_output}"
            -p:DefineConstants=NULLUS_PLUGIN_ACCEPTANCE_FIXTURES --nologo
        DEPENDS NLS_Scripting VERBATIM)
    set_target_properties(NullusPluginManagedSample PROPERTIES FOLDER "Tests/Fixtures/Managed")
    target_sources(NullusUnitTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ScriptAdapterTests.cpp")
    target_compile_definitions(NullusUnitTests PRIVATE NLS_PLUGIN_REPO="${NLS_PLUGIN_ROOT}"
        NLS_PLUGIN_DOTNET_ROOT="${_plugin_dotnet_root}" NLS_PLUGIN_MANAGED_OUTPUT="${CMAKE_BINARY_DIR}/Managed/$<CONFIG>"
        NLS_PLUGIN_SAMPLE_OUTPUT="${_plugin_sample_output}")
    add_dependencies(NullusUnitTests NullusPluginManagedSample)
endif()
if(TARGET NullusUnitTests AND TARGET NLS_Build)
    target_sources(NullusUnitTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/AssetCookTests.cpp")
    target_link_libraries(NullusUnitTests PRIVATE NLS_Build NLS_Cook NLS_Plugins)
endif()
if(TARGET NullusUnitTests AND TARGET NLS_Engine)
    target_sources(NullusUnitTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/SerializationTests.cpp")
endif()
if(WIN32 AND TARGET NullusUnitTests AND TARGET EditorProject AND NLS_ENABLE_CSHARP_SCRIPTING)
    # The production build service is already provided by EditorProject.
    target_sources(NullusUnitTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ManagedBuildTests.cpp")
    target_compile_definitions(NullusUnitTests PRIVATE NLS_PLUGIN_DOTNET_EXECUTABLE="${NLS_DOTNET_EXECUTABLE}")
endif()
