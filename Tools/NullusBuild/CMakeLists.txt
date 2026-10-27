add_executable(NullusBuild
    Main.cpp)

target_compile_features(NullusBuild PRIVATE cxx_std_20)
target_include_directories(NullusBuild PRIVATE
    "${CMAKE_SOURCE_DIR}/Runtime"
    "${CMAKE_SOURCE_DIR}/Runtime/Core"
    "${CMAKE_SOURCE_DIR}/Runtime/Engine"
    "${CMAKE_SOURCE_DIR}/ThirdParty"
)
target_link_libraries(NullusBuild PRIVATE NLS_Build)
set_target_properties(NullusBuild PROPERTIES
    FOLDER "Tools/Build"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
nls_copy_shared_runtime_to_target(NullusBuild)

install(TARGETS NullusBuild RUNTIME DESTINATION .)
