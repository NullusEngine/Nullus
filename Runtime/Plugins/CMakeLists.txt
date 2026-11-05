nls_add_runtime_target(NAME NLS_Plugins SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    NO_REFLECTION LIBS NLS_Core NLS_Platform)
if(UNIX)
    target_link_libraries(NLS_Plugins PRIVATE ${CMAKE_DL_LIBS})
endif()
add_library(NullusPluginSDK INTERFACE)
target_include_directories(NullusPluginSDK INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/SDK")
add_custom_target(NullusPluginSDKPackage
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/plugin-sdk"
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${CMAKE_BINARY_DIR}/plugin-sdk/NullusPluginSDK.zip" --format=zip
        NullusPlugin.h NullusPluginSDK.cmake README.md
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/SDK" VERBATIM)
set_target_properties(NullusPluginSDKPackage PROPERTIES FOLDER "Plugins/Packaging")
