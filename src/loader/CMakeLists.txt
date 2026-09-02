cmake_minimum_required(VERSION 3.20)
project(hl2_loader C)

# default.xbe -- the disc's installer, recompiled.
#
# Built as a separate executable from the game because it is a separate image:
# 1,415 functions against the game's 48,335, its own entry point, and a job
# that runs once. See regen_loader.sh and src/loader/main.c.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB LOADER_GEN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/gen/recomp_*.c"
)

add_executable(hl2_loader main.c recomp/recomp_manual.c ${LOADER_GEN_SOURCES})

target_link_libraries(hl2_loader PRIVATE
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_video
    xbox_apu
    kernel32
    user32
    gdi32
    dbghelp
)

target_include_directories(hl2_loader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/recomp/gen
    ${CMAKE_SOURCE_DIR}/src
    ${XBOXRECOMP_DIR}/templates/runtime
)

if(MSVC)
    target_compile_options(hl2_loader PRIVATE /W4 /WX- /O2 /MP)
    set_source_files_properties(${LOADER_GEN_SOURCES} PROPERTIES
        COMPILE_FLAGS "/w"
    )
    target_compile_definitions(hl2_loader PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(hl2_loader PRIVATE
        /DYNAMICBASE:NO
        /FIXED:NO
        /MAP
        /DEBUG
        /STACK:16777216
    )
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(hl2_loader PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
    set_source_files_properties(${LOADER_GEN_SOURCES} PROPERTIES COMPILE_FLAGS "-w")
endif()

set_target_properties(hl2_loader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
)
