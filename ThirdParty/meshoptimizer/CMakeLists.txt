add_library(meshoptimizer STATIC
    src/meshoptimizer.h
    src/simplifier.cpp
)

target_include_directories(meshoptimizer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_compile_features(meshoptimizer PUBLIC cxx_std_11)
set_target_properties(meshoptimizer PROPERTIES
    FOLDER "ThirdParty/meshoptimizer"
    POSITION_INDEPENDENT_CODE ON
)
