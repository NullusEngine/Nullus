add_library(lmdb STATIC
    mdb.c
    midl.c
    lmdb.h
    midl.h
)

target_include_directories(lmdb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_definitions(lmdb PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(lmdb PRIVATE /wd4172 /wd4333)
endif()

set_target_properties(lmdb PROPERTIES FOLDER ThirdParty/LMDB)
