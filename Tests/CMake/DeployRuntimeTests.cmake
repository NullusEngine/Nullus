cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED NLS_ROOT_DIR)
    message(FATAL_ERROR "NLS_ROOT_DIR is required")
endif()

set(TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/deploy-runtime-test")
set(STAGE_DIR "${TEST_ROOT}/stage")
set(DEST_DIR "${TEST_ROOT}/destination")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${STAGE_DIR}/nested" "${DEST_DIR}/Assets")
file(WRITE "${STAGE_DIR}/runtime-a.bin" "runtime-a-v1")
file(WRITE "${STAGE_DIR}/nested/runtime-b.bin" "runtime-b")
file(WRITE "${DEST_DIR}/Editor.exe" "editor")
file(WRITE "${DEST_DIR}/Assets/project.asset" "asset")

function(run_deployment)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DSTAGE_DIR=${STAGE_DIR}"
            "-DDEST_DIR=${DEST_DIR}"
            "-DCONFIG=Debug"
            -P "${NLS_ROOT_DIR}/Tools/CMake/DeployRuntime.cmake"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Deployment failed (${result}):\n${output}\n${error}")
    endif()
endfunction()

run_deployment()
foreach(expected IN ITEMS "runtime-a.bin" "nested/runtime-b.bin" "Editor.exe" "Assets/project.asset")
    if(NOT EXISTS "${DEST_DIR}/${expected}")
        message(FATAL_ERROR "Expected file is missing after initial deployment: ${expected}")
    endif()
endforeach()

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
        -P "${NLS_ROOT_DIR}/Tools/CMake/DeployRuntime.cmake"
    COMMAND "${CMAKE_COMMAND}"
        "-DSTAGE_DIR=${STAGE_DIR}"
        "-DDEST_DIR=${DEST_DIR}"
        "-DCONFIG=Debug"
        -P "${NLS_ROOT_DIR}/Tools/CMake/DeployRuntime.cmake"
    RESULTS_VARIABLE concurrent_results)
foreach(result IN LISTS concurrent_results)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Concurrent deployment failed: ${concurrent_results}")
    endif()
endforeach()

file(REMOVE "${STAGE_DIR}/nested/runtime-b.bin")
file(WRITE "${STAGE_DIR}/runtime-a.bin" "runtime-a-v2")
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

message(STATUS "DeployRuntime integration tests passed")
