include("${CMAKE_CURRENT_LIST_DIR}/../../../Runtime/Plugins/SDK/NullusPluginSDK.cmake")
nullus_add_binary_plugin(NullusRuntimeSample RuntimeSample.cpp)
target_compile_features(NullusRuntimeSample PRIVATE cxx_std_20)
set_target_properties(NullusRuntimeSample PROPERTIES PREFIX "" DEBUG_POSTFIX "" FOLDER "Plugins/Examples")
add_custom_target(NullusRuntimeSamplePackage
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/plugin-examples/RuntimeSample/$<CONFIG>/Binaries"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${CMAKE_BINARY_DIR}/plugin-examples/RuntimeSample/$<CONFIG>/plugin.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NullusRuntimeSample>" "${CMAKE_BINARY_DIR}/plugin-examples/RuntimeSample/$<CONFIG>/Binaries/$<TARGET_FILE_NAME:NullusRuntimeSample>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Content" "${CMAKE_BINARY_DIR}/plugin-examples/RuntimeSample/$<CONFIG>/Content"
    DEPENDS NullusRuntimeSample VERBATIM)
set_target_properties(NullusRuntimeSamplePackage PROPERTIES FOLDER "Plugins/Examples")
