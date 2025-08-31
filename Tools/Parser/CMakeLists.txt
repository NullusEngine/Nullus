cmake_minimum_required(VERSION 3.12)
set(TARGET_NAME MetaParser)
project(MetaParser CXX)

set (CMAKE_CXX_STANDARD 17)

set(HEADER_FILES
    Cursor.h
    CursorType.h
    MetaDataConfig.h
    MetaDataManager.h
    MetaUtils.h
    MetaUtils.hpp
    Namespace.h
    Precompiled.h
    ReflectionOptions.h
    ReflectionParser.h
    ReflectionParserUtils.h
    ReservedTypes.h
    Switches.h
    Templates.h
    Version.h
    LanguageTypes/Class.h
    LanguageTypes/Constructor.h
    LanguageTypes/Enum.h
    LanguageTypes/External.h
    LanguageTypes/Field.h
    LanguageTypes/Function.h
    LanguageTypes/Global.h
    LanguageTypes/Invokable.h
    LanguageTypes/LanguageType.h
    LanguageTypes/Method.h
    Module/ModuleFile.h
    Tokenizer/TokenizerResult.h
    Tokenizer/TokenizerState.h
    Tokenizer/TokenType.h
    Tokenizer/Token.h
    Tokenizer/Tokenizer.h
    Tokenizer/ConstructorTokenSymbols.inl
    Tokenizer/Impl/TokenizerResult.hpp
    Tokenizer/Impl/TokenizerState.hpp
    Tokenizer/Impl/Token.hpp
    Tokenizer/Impl/Tokenizer.hpp
)

set(SOURCE_FILES
    Cursor.cpp
    CursorType.cpp
    Main.cpp
    MetaDataManager.cpp
    MetaUtils.cpp
    Precompiled.cpp
    ReflectionParser.cpp
    ReflectionParserUtils.cpp
    LanguageTypes/Class.cpp
    LanguageTypes/Constructor.cpp
    LanguageTypes/Enum.cpp
    LanguageTypes/External.cpp
    LanguageTypes/Field.cpp
    LanguageTypes/Function.cpp
    LanguageTypes/Global.cpp
    LanguageTypes/Invokable.cpp
    LanguageTypes/LanguageType.cpp
    LanguageTypes/Method.cpp
)

# header files are superflous, but some IDEs (Visual Studio) don't include
# them in the solution explorer without them being added to the list of sources
add_executable(MetaParser ${HEADER_FILES} ${SOURCE_FILES})

if (CMAKE_HOST_WIN32)
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/x64)
    set(LLVM_SHARED_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/bin/x64)
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} /O2 /Ob2")
    target_link_libraries(${TARGET_NAME}  ${LLVM_LIBRARY_DIR}/libclang.lib)
elseif(${CMAKE_HOST_SYSTEM_NAME} STREQUAL "Linux")
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/Linux)
    set(LLVM_SHARED_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/bin/Linux)
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O3")
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.so.12)
elseif(${CMAKE_HOST_SYSTEM_PROCESSOR} STREQUAL "arm64") 
    # link to XCode Toolchains' universal binary libclang.dylib 
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/macOS)
    set(LLVM_SHARED_LIBRARY_DIR  ${OSX_DEVELOPER_ROOT}/Toolchains/XcodeDefault.xctoolchain/usr/lib)
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O3")
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.dylib)    
else()
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/macOS)
    set(LLVM_SHARED_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/bin/macOS)
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O3")
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.dylib)
endif()


set(LLVM_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/include ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mustache ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${ENGINE_ROOT_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${ENGINE_ROOT_DIR}/bin)
# add LLVM includes
include_directories(${LLVM_INCLUDE_DIRS})

link_directories(${LLVM_LIBRARY_DIR})

# statically link with Boost & LibClang
target_link_libraries(MetaParser ${LIBCLANG_LIBRARY})


if (MSVC)
    # disable security warnings
    add_definitions(-D_CRT_SECURE_NO_WARNINGS=1)

    target_compile_options(MetaParser
        PUBLIC
        # treat warnings as errors
        /WX 
        # multi process compilation
        /MP
    )

    # detect version of Visual Studio
    if (MSVC10)
        set(VS_VERSION "10")
    elseif (MSVC11)
        set(VS_VERSION "11")
    elseif (MSVC12)
        set(VS_VERSION "12")
    elseif (MSVC14)
        set(VS_VERSION "14")
    endif()

    # cmake is gross sometimes
    set(_PROGRAM_FILES_x86 "ProgramFiles(x86)")
    set(PROGRAM_FILES_x86 "$ENV{${_PROGRAM_FILES_x86}}")
    set(SYSTEM_INCLUDES "${PROGRAM_FILES_x86}/Microsoft Visual Studio ${VS_VERSION}.0/VC/include")

    # normalize slashes
    string(REPLACE "\\" "/" SYSTEM_INCLUDES "${SYSTEM_INCLUDES}")
else ()
    target_compile_options(MetaParser
        PUBLIC -std=c++11
    )

    set(SYSTEM_INCLUDES "/usr/include/c++/${CMAKE_CXX_COMPILER_VERSION}")
endif ()

if (SYSTEM_INCLUDES)
    # visual studio seems to have issues with escape characters in post build commands
    set_property(TARGET MetaParser APPEND PROPERTY COMPILE_DEFINITIONS SYSTEM_INCLUDE_DIRECTORY="${SYSTEM_INCLUDES}")
else ()
    message(FATAL_ERROR "System include directories not implemented for this compiler.")
endif ()


# copy resources on post build
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
	# mustache templates directory
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${LLVM_SHARED_LIBRARY_DIR}"
		$<TARGET_FILE_DIR:${TARGET_NAME}>
)