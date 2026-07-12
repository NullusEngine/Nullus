cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED NLS_ROOT_DIR)
    message(FATAL_ERROR "NLS_ROOT_DIR is required")
endif()

set(TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/deploy-runtime-test")
set(STAGE_DIR "${TEST_ROOT}/stage")
set(DEST_DIR "${TEST_ROOT}/destination")
set(EXPECTED_MANIFEST "${TEST_ROOT}/expected-runtime.txt")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${STAGE_DIR}/nested" "${DEST_DIR}/Assets")
file(WRITE "${STAGE_DIR}/runtime-a.bin" "runtime-a-v1")
file(WRITE "${STAGE_DIR}/nested/runtime-b.bin" "runtime-b")
file(WRITE "${STAGE_DIR}/libfbxsdk.dll" "external-fbx-runtime")
file(WRITE "${EXPECTED_MANIFEST}" "runtime-a.bin\nnested/runtime-b.bin\nlibfbxsdk.dll\n")
file(WRITE "${DEST_DIR}/Editor.exe" "editor")
file(WRITE "${DEST_DIR}/Assets/project.asset" "asset")

function(run_deployment)
    if(ARGC GREATER 0)
        set(deployment_config "${ARGV0}")
    else()
        set(deployment_config "Debug")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DSTAGE_DIR=${STAGE_DIR}"
            "-DDEST_DIR=${DEST_DIR}"
            "-DCONFIG=${deployment_config}"
            "-DEXPECTED_MANIFEST=${EXPECTED_MANIFEST}"
            -P "${NLS_ROOT_DIR}/Tools/CMake/DeployRuntime.cmake"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Deployment failed (${result}):\n${output}\n${error}")
    endif()
endfunction()

run_deployment()
foreach(expected IN ITEMS "runtime-a.bin" "nested/runtime-b.bin" "libfbxsdk.dll" "Editor.exe" "Assets/project.asset")
    if(NOT EXISTS "${DEST_DIR}/${expected}")
        message(FATAL_ERROR "Expected file is missing after initial deployment: ${expected}")
    endif()
endforeach()

run_deployment("")
if(NOT EXISTS "${DEST_DIR}/.nullus-runtime-NoConfig.manifest")
    message(FATAL_ERROR "Empty configuration did not use the stable NoConfig manifest name")
endif()

file(TIMESTAMP "${DEST_DIR}/runtime-a.bin" first_timestamp "%s")
run_deployment()
file(TIMESTAMP "${DEST_DIR}/runtime-a.bin" second_timestamp "%s")
if(NOT first_timestamp STREQUAL second_timestamp)
    message(FATAL_ERROR "Idempotent deployment rewrote an unchanged runtime file")
endif()

# Multiple COMMAND entries form a concurrent pipeline. Neither deployment reads
# stdin, so this exercises two processes contending for the destination lock.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSTAGE_DIR=${STAGE_DIR}"
        "-DDEST_DIR=${DEST_DIR}"
        "-DCONFIG=Debug"
        "-DEXPECTED_MANIFEST=${EXPECTED_MANIFEST}"
        -P "${NLS_ROOT_DIR}/Tools/CMake/DeployRuntime.cmake"
    COMMAND "${CMAKE_COMMAND}"
        "-DSTAGE_DIR=${STAGE_DIR}"
        "-DDEST_DIR=${DEST_DIR}"
        "-DCONFIG=Debug"
        "-DEXPECTED_MANIFEST=${EXPECTED_MANIFEST}"
        -P "${NLS_ROOT_DIR}/Tools/CMake/DeployRuntime.cmake"
    RESULTS_VARIABLE concurrent_results)
foreach(result IN LISTS concurrent_results)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Concurrent deployment failed: ${concurrent_results}")
    endif()
endforeach()

file(WRITE "${EXPECTED_MANIFEST}" "runtime-a.bin\nlibfbxsdk.dll\n")
file(WRITE "${STAGE_DIR}/runtime-a.bin" "runtime-a-v2")
if(NOT EXISTS "${STAGE_DIR}/nested/runtime-b.bin")
    message(FATAL_ERROR "The stale-runtime test requires the obsolete file to remain in staging")
endif()
run_deployment()
if(EXISTS "${DEST_DIR}/nested/runtime-b.bin")
    message(FATAL_ERROR "Stale previously deployed runtime file was not removed")
endif()
file(READ "${DEST_DIR}/runtime-a.bin" deployed_runtime)
if(NOT deployed_runtime STREQUAL "runtime-a-v2")
    message(FATAL_ERROR "Updated runtime file was not deployed")
endif()
foreach(unrelated IN ITEMS "Editor.exe" "Assets/project.asset")
    if(NOT EXISTS "${DEST_DIR}/${unrelated}")
        message(FATAL_ERROR "Unrelated destination content was removed: ${unrelated}")
    endif()
endforeach()
if(NOT EXISTS "${DEST_DIR}/libfbxsdk.dll")
    message(FATAL_ERROR "Registered external runtime was removed during stale cleanup")
endif()

message(STATUS "DeployRuntime integration tests passed")
