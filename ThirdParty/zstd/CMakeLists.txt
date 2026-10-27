# Nullus vendored Zstandard (see README.nullus.md).
#
# The upstream build system is not imported on purpose: only the library
# sources are vendored, and the options below are pinned so every Nullus
# platform produces the same compression behaviour for .npak content.
set(NLS_ZSTD_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(NLS_ZSTD_COMMON_SOURCES
    ${NLS_ZSTD_DIR}/lib/common/debug.c
    ${NLS_ZSTD_DIR}/lib/common/entropy_common.c
    ${NLS_ZSTD_DIR}/lib/common/error_private.c
    ${NLS_ZSTD_DIR}/lib/common/fse_decompress.c
    ${NLS_ZSTD_DIR}/lib/common/pool.c
    ${NLS_ZSTD_DIR}/lib/common/threading.c
    ${NLS_ZSTD_DIR}/lib/common/xxhash.c
    ${NLS_ZSTD_DIR}/lib/common/zstd_common.c)

set(NLS_ZSTD_COMPRESS_SOURCES
    ${NLS_ZSTD_DIR}/lib/compress/fse_compress.c
    ${NLS_ZSTD_DIR}/lib/compress/hist.c
    ${NLS_ZSTD_DIR}/lib/compress/huf_compress.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_compress.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_compress_literals.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_compress_sequences.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_compress_superblock.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_double_fast.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_fast.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_lazy.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_ldm.c
    ${NLS_ZSTD_DIR}/lib/compress/zstd_opt.c
    ${NLS_ZSTD_DIR}/lib/compress/zstdmt_compress.c)

set(NLS_ZSTD_DECOMPRESS_SOURCES
    ${NLS_ZSTD_DIR}/lib/decompress/huf_decompress.c
    ${NLS_ZSTD_DIR}/lib/decompress/zstd_ddict.c
    ${NLS_ZSTD_DIR}/lib/decompress/zstd_decompress.c
    ${NLS_ZSTD_DIR}/lib/decompress/zstd_decompress_block.c)

add_library(zstd STATIC
    ${NLS_ZSTD_COMMON_SOURCES}
    ${NLS_ZSTD_COMPRESS_SOURCES}
    ${NLS_ZSTD_DECOMPRESS_SOURCES})

target_include_directories(zstd PUBLIC
    ${NLS_ZSTD_DIR}/lib)

# The .npak writer must be deterministic across hosts. A single-context
# encoder with no worker threads is used, so the multithreaded variants stay
# compiled but cannot change output bytes. Legacy frame support is not needed
# because every package is written by this same library version.
target_compile_definitions(zstd PUBLIC
    ZSTD_DISABLE_ASM=1
    ZSTD_LEGACY_SUPPORT=0)

if(WIN32 AND NOT MINGW)
    target_compile_definitions(zstd PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(zstd PROPERTIES
    FOLDER "ThirdParty/zstd"
    POSITION_INDEPENDENT_CODE ON)
