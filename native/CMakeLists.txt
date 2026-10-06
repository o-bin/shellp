cmake_minimum_required(VERSION 3.22.1)
project(shellp_main C)

set(CMAKE_C_STANDARD 11)

# Generate glyph.h using mkfont_bdf
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/../repos/yaft/glyph.h
    COMMAND gcc -std=c99 -O2 tools/mkfont_bdf.c -o tools/mkfont_bdf
    COMMAND ./tools/mkfont_bdf table/alias fonts/milkjf/milkjf_k16.bdf fonts/milkjf/milkjf_8x16r.bdf fonts/milkjf/milkjf_8x16.bdf fonts/terminus/ter-u16n.bdf > glyph.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../repos/yaft
    COMMENT "Generating glyph.h for yaft"
    VERBATIM
)

# Custom target so it runs before compilation
add_custom_target(generate_glyph_h DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../repos/yaft/glyph.h)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/app_glue
    ${CMAKE_CURRENT_SOURCE_DIR}/tar_extract
    ${CMAKE_CURRENT_SOURCE_DIR}/xz_embedded/linux/include/linux
    ${CMAKE_CURRENT_SOURCE_DIR}/xz_embedded/userspace
    ${CMAKE_CURRENT_SOURCE_DIR}/../repos/yaft
)

add_definitions(
    -DXZ_USE_CRC64
    -DXZ_USE_SHA256
    -DXZ_DEC_ANY_CHECK
    -DXZ_DEC_CONCATENATED
    -DXZ_DEC_ARM64
    -DXZ_DEC_X86
    -DXZ_DEC_ARM
    -DXZ_DEC_ARMTHUMB
    -DXZ_DEC_RISCV
    -DXZ_DEC_POWERPC
)

set(SHELLP_SRCS
    shellp_main.c
    app_glue/android_native_app_glue.c
    tar_extract/tar_xz.c
    xz_embedded/linux/lib/xz/xz_crc32.c
    xz_embedded/linux/lib/xz/xz_crc64.c
    xz_embedded/linux/lib/xz/xz_sha256.c
    xz_embedded/linux/lib/xz/xz_dec_stream.c
    xz_embedded/linux/lib/xz/xz_dec_lzma2.c
    xz_embedded/linux/lib/xz/xz_dec_bcj.c
)

add_library(shellp_main SHARED ${SHELLP_SRCS})

add_dependencies(shellp_main generate_glyph_h)

target_link_libraries(shellp_main
    android
    log
    m
)
