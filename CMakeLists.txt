cmake_minimum_required(VERSION 3.20)
project(hl2_recomp C)

# Half-Life 2 (Xbox, 2005) - Static Recompilation
# Builds all compatibility layers and the recompiled game executable

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Record every indirect-branch target the game reaches, so the next codegen
# pass can seed function detection with measurements instead of inference.
# Costs one byte store per indirect call; off by default.
# See xboxrecomp docs/technical/indirect-calls.md ("Target Feedback").
option(HL2_ICALL_FEEDBACK "Record indirect-branch targets for codegen feedback" OFF)
if(HL2_ICALL_FEEDBACK)
    add_compile_definitions(RECOMP_ICALL_FEEDBACK)
    message(STATUS "ICALL target feedback: ON")
endif()

# ebx/esi/edi are callee-saved on x86 and live in globals here, so a lifted
# function that skips its epilogue corrupts its *caller* and nothing crashes.
# This makes every indirect call check them and name the offender. Costs three
# compares per call; off by default.
option(HL2_ABI_CHECK "Report functions that clobber callee-saved registers" OFF)
if(HL2_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK)
    message(STATUS "Callee-saved register check: ON")
endif()

# ── xboxrecomp runtime libraries ──────────────────────────
# Point this to your xboxrecomp checkout
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH "Path to xboxrecomp toolkit")
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ─────────────────────────────
add_subdirectory(src/game)

# The disc's installer, recompiled: it decompresses the .xz_ archives the
# game expects to find already decompressed. Separate image, separate
# executable -- see src/loader/main.c.
add_subdirectory(src/loader)
