add_executable(MetaParser main.cpp)

target_compile_features(MetaParser PRIVATE cxx_std_20)

set_target_properties(MetaParser PROPERTIES
    FOLDER Tools
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
