# ECS behavior is part of the same ordinary test executable. Keep the examples
# as runnable applications, without duplicating the tests in per-feature EXEs.
set(_ecs_test_files
    CoreTests.cpp QueryTests.cpp CommandTests.cpp SystemTests.cpp ParallelTests.cpp
    SerializationTests.cpp StreamingTests.cpp HybridTests.cpp TransformTests.cpp
    BakingTests.cpp AuthoringIdentityTests.cpp AssetPipelineTests.cpp)
if(TARGET GameProject)
    list(APPEND _ecs_test_files PlayerStartupTests.cpp)
endif()
if(TARGET EditorProject)
    list(APPEND _ecs_test_files EditorBakingTests.cpp)
    target_compile_definitions(NullusUnitTests PRIVATE NLS_ECS_ASSET_EDITOR_TESTS=1)
endif()
list(TRANSFORM _ecs_test_files PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
target_sources(NullusUnitTests PRIVATE ${_ecs_test_files})
target_link_libraries(NullusUnitTests PRIVATE NLS_ECS NLS_Cook NLS_Build)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX "ECS Tests" FILES ${_ecs_test_files})

add_executable(NullusECSAssetPipelineExample "${CMAKE_SOURCE_DIR}/Samples/ECS/AssetPipeline.cpp")
target_compile_features(NullusECSAssetPipelineExample PRIVATE cxx_std_20)
target_compile_options(NullusECSAssetPipelineExample PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
target_link_libraries(NullusECSAssetPipelineExample PRIVATE NLS_Cook NLS_Build NLS_Engine NLS_ECS)
set_target_properties(NullusECSAssetPipelineExample PROPERTIES FOLDER Samples/ECS RUNTIME_OUTPUT_DIRECTORY "${NLS_RUNTIME_OUTPUT_PATH}")

add_executable(NullusECSStreamingExample "${CMAKE_SOURCE_DIR}/Samples/ECS/SceneStreaming.cpp")
target_compile_features(NullusECSStreamingExample PRIVATE cxx_std_20)
target_compile_options(NullusECSStreamingExample PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
target_link_libraries(NullusECSStreamingExample PRIVATE NLS_ECS)
set_target_properties(NullusECSStreamingExample PROPERTIES FOLDER Samples/ECS RUNTIME_OUTPUT_DIRECTORY "${NLS_RUNTIME_OUTPUT_PATH}")

add_executable(NullusECSHybridExample "${CMAKE_SOURCE_DIR}/Samples/ECS/Hybrid.cpp")
target_compile_features(NullusECSHybridExample PRIVATE cxx_std_20)
target_compile_options(NullusECSHybridExample PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
target_link_libraries(NullusECSHybridExample PRIVATE NLS_Scripting NLS_Engine NLS_ECS)
set_target_properties(NullusECSHybridExample PROPERTIES FOLDER Samples/ECS RUNTIME_OUTPUT_DIRECTORY "${NLS_RUNTIME_OUTPUT_PATH}")

add_executable(NullusECSBakingExample "${CMAKE_SOURCE_DIR}/Samples/ECS/Baking.cpp")
target_compile_features(NullusECSBakingExample PRIVATE cxx_std_20)
target_compile_options(NullusECSBakingExample PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)
target_link_libraries(NullusECSBakingExample PRIVATE NLS_Engine NLS_ECS)
set_target_properties(NullusECSBakingExample PROPERTIES FOLDER Samples/ECS RUNTIME_OUTPUT_DIRECTORY "${NLS_RUNTIME_OUTPUT_PATH}")
