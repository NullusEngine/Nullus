add_subdirectory(MetaParser)
add_subdirectory(ReflectionTest)

add_executable(NullusDebugBroker Debug/NullusDebugBroker.cpp)
target_include_directories(NullusDebugBroker PRIVATE
    "${CMAKE_SOURCE_DIR}/ThirdParty"
)
set_target_properties(NullusDebugBroker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${NLS_APP_OUTPUT_PATH}"
)
set_target_properties(NullusDebugBroker PROPERTIES FOLDER "Tools/Debug")
