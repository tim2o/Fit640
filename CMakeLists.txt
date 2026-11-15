cmake_minimum_required(VERSION 3.10)
project(Fit640 C CXX ASM)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall --param=min-pagesize=0")

# --- The extension (INIT): built as a flat code resource ---
add_executable(Fit640 src/Fit640.c src/Blit.s src/Fit640.h)
set_target_properties(Fit640 PROPERTIES
    OUTPUT_NAME Fit640.flt
    LINK_FLAGS -Wl,--mac-flat)

if(REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

add_custom_command(
    OUTPUT Fit640.bin
    COMMAND ${REZ} ${REZ_INCLUDE_FLAG}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/Fit640.r
            -o Fit640.bin
            -t INIT -c F640
    DEPENDS Fit640 src/Fit640.r)
add_custom_target(Fit640_INIT ALL DEPENDS Fit640.bin)

# --- The info app ---
add_application(Fit640Info info/Fit640Info.c CONSOLE CREATOR F64i)
