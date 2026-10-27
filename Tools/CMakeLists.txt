add_subdirectory(MetaParser)
add_subdirectory(ReflectionTest)
add_subdirectory(NullusBuild)

add_executable(NullusVfxCompile VFX/VfxCompile.cpp)
add_executable(NullusMaterialCompile Material/MaterialCompile.cpp)
target_link_libraries(NullusMaterialCompile PRIVATE NLS_MaterialCompiler)
target_compile_features(NullusMaterialCompile PRIVATE cxx_std_20)
set_target_properties(NullusMaterialCompile PROPERTIES FOLDER "Tools/Material"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
nls_copy_shared_runtime_to_target(NullusMaterialCompile)
target_link_libraries(NullusVfxCompile PRIVATE NLS_VFXCompiler)
target_compile_features(NullusVfxCompile PRIVATE cxx_std_20)
set_target_properties(NullusVfxCompile PROPERTIES FOLDER "Tools/VFX"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
nls_copy_shared_runtime_to_target(NullusVfxCompile)

add_executable(NullusDebugBroker Debug/NullusDebugBroker.cpp)
target_include_directories(NullusDebugBroker PRIVATE
    "${CMAKE_SOURCE_DIR}/ThirdParty"
)
set_target_properties(NullusDebugBroker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
)
set_target_properties(NullusDebugBroker PROPERTIES FOLDER "Tools/Debug")
