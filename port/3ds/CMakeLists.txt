# PSP2i for New 3DS (3DS_PLAN.md). Configure with devkitPro's toolchain:
#
#   cmake -S port/3ds -B port/3ds/build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
#
# (port/3ds/build.sh does this through MSYS2.) For now it builds the T0
# check app, psp2i_hello, as a .3dsx and a .cia; the game joins in T1.
cmake_minimum_required(VERSION 3.20)
project(psp2i_3ds C ASM)

set(MAKEROM    "D:/Tools/makerom/makerom.exe" CACHE FILEPATH "makerom (Project_CTR)")
set(BANNERTOOL "D:/Tools/bannertool/bannertool-1.2.3-windows/bannertool.exe" CACHE FILEPATH "bannertool")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(PSP2I_RSF ${CMAKE_CURRENT_SOURCE_DIR}/psp2i.rsf)
set(ASSETS ${CMAKE_CURRENT_BINARY_DIR}/assets)

# HOME menu icon, banner and banner sound, drawn from text (no game art)
add_custom_command(
    OUTPUT ${ASSETS}/icon.png ${ASSETS}/banner.png ${ASSETS}/banner.wav
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/make_assets.py ${ASSETS}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/make_assets.py
    VERBATIM)
add_custom_command(
    OUTPUT ${ASSETS}/banner.bnr
    COMMAND ${BANNERTOOL} makebanner -i ${ASSETS}/banner.png -a ${ASSETS}/banner.wav -o ${ASSETS}/banner.bnr
    DEPENDS ${ASSETS}/banner.png ${ASSETS}/banner.wav
    VERBATIM)

# A .3dsx and a .cia for `target`. The .cia carries the exheader from
# psp2i.rsf (124 MB mode, core 2); unique_id must differ per app.
function(psp2i_package target title description unique_id product_code)
    ctr_generate_smdh(${target}.smdh
        NAME "${title}" DESCRIPTION "${description}" AUTHOR "PSP2i Recomp"
        ICON ${ASSETS}/icon.png)
    ctr_create_3dsx(${target} SMDH ${target}.smdh)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target}.cia
        COMMAND ${MAKEROM} -f cia -o ${CMAKE_CURRENT_BINARY_DIR}/${target}.cia
                -target t -exefslogo
                -elf $<TARGET_FILE:${target}> -rsf ${PSP2I_RSF}
                -icon ${CMAKE_CURRENT_BINARY_DIR}/${target}.smdh
                -banner ${ASSETS}/banner.bnr
                "-DAPP_TITLE=${title}" -DAPP_PRODUCT_CODE=${product_code}
                -DAPP_UNIQUE_ID=${unique_id}
        DEPENDS ${target} ${PSP2I_RSF} ${CMAKE_CURRENT_BINARY_DIR}/${target}.smdh ${ASSETS}/banner.bnr
        VERBATIM)
    add_custom_target(${target}_cia ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target}.cia)
endfunction()

add_executable(psp2i_hello hello/main.c)
target_compile_options(psp2i_hello PRIVATE -O2 -Wall)
target_link_libraries(psp2i_hello PRIVATE ctru m)
psp2i_package(psp2i_hello "PSP2i 3DS check" "T0 bring-up check" 0xF5220 CTR-P-P2IH)

# ---- the game (T1) ------------------------------------------------------------
#
# The psprecomp runtime, built for the ARM11 with PSPRECOMP_3DS (ARM32 fibers,
# smaller stacks), plus the C that allegrexrecomp generated from the user's
# EBOOT into port/gen (git-ignored; see psp2i-build-run to make it).
set(PORT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(PSPRECOMP_DIR ${PORT}/../psprecomp)
set(PSP2I_GEN_DIR ${PORT}/gen CACHE PATH "allegrexrecomp output (recomp_funcs.c)")
# parts of the ~200 MB recomp_funcs.c: each must fit GCC's memory at -Os
# with a dozen compiling at once (32 GB machine)
set(PSP2I_GEN_PARTS 256 CACHE STRING "translation units recomp_funcs.c is split into")

add_subdirectory(${PSPRECOMP_DIR} psprecomp EXCLUDE_FROM_ALL)
target_compile_definitions(psprecomp PUBLIC PSPRECOMP_3DS)
# host stack per PSP thread (bytes); empty = the runtime's 3DS default
set(PSP2I_FIBER_STACK "" CACHE STRING "PSPRECOMP_FIBER_STACK for the 3DS build")
set(PSP2I_FIBER_STACK_SMALL "" CACHE STRING "PSPRECOMP_FIBER_STACK_SMALL for the 3DS build")
if(PSP2I_FIBER_STACK)
    target_compile_definitions(psprecomp PRIVATE PSPRECOMP_FIBER_STACK=${PSP2I_FIBER_STACK})
endif()
if(PSP2I_FIBER_STACK_SMALL)
    target_compile_definitions(psprecomp PRIVATE PSPRECOMP_FIBER_STACK_SMALL=${PSP2I_FIBER_STACK_SMALL})
endif()
# The runtime is small, and its GE vertex work (gpu.c) took 300-470 ms of
# every second in town at -Os (T4): it's built for speed. -Os only matters
# for the ~200 MB of game code below.
set(PSP2I_RUNTIME_OPT "-O2" CACHE STRING "optimization flag for the psprecomp runtime")
# newlib's uint32_t is `long` on ARM: every "%X" of one warns, harmlessly
# (both are 32 bits here), and would bury the real messages
target_compile_options(psprecomp PRIVATE ${PSP2I_RUNTIME_OPT} -Wno-format -Wno-comment
    -include ${CMAKE_CURRENT_SOURCE_DIR}/net_compat.h)

if(EXISTS ${PSP2I_GEN_DIR}/recomp_funcs.c)
    set(SPLIT_DIR ${CMAKE_BINARY_DIR}/gen_split)
    # the functions the game spends its time in (hot_functions.txt) go to
    # recomp_hot_*.c, built for speed below
    # (on by default since the stack fix, 3DS_PROGRESS.md T4; -DPSP2I_HOT_LIST=
    # with nothing after it builds all of the game's code at -Os again)
    set(PSP2I_HOT_LIST ${CMAKE_CURRENT_SOURCE_DIR}/hot_functions.txt CACHE FILEPATH
        "game functions built at -O2 with inline memory access (empty: none)")
    execute_process(COMMAND ${Python3_EXECUTABLE} ${PORT}/android/split_gen.py
                            ${PSP2I_GEN_DIR}/recomp_funcs.c ${SPLIT_DIR} ${PSP2I_GEN_PARTS} ${PSP2I_HOT_LIST}
                    RESULT_VARIABLE split_rc)
    if(NOT split_rc EQUAL 0)
        message(FATAL_ERROR "split_gen.py could not split ${PSP2I_GEN_DIR}/recomp_funcs.c")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PSP2I_GEN_DIR}/recomp_funcs.c ${PSP2I_HOT_LIST})
    file(GLOB GEN_PARTS ${SPLIT_DIR}/recomp_part_*.c)
    file(GLOB GEN_HOT ${SPLIT_DIR}/recomp_hot_*.c)

    # the game's code as a library of its own, so its size can be read alone;
    # -Os and no debug info (P3P3DS: -Os took its code from 77 to 44 MB)
    add_library(psp2i_game STATIC ${GEN_PARTS} ${GEN_HOT} ${PSP2I_GEN_DIR}/recomp_imports.c)
    target_include_directories(psp2i_game PRIVATE ${PSP2I_GEN_DIR} ${SPLIT_DIR})
    target_compile_options(psp2i_game PRIVATE -Os -g0 -w)
    # The hot functions: -O2, and main-RAM loads and stores inline (psprecomp
    # mem.h; otherwise each is a call that spills and reloads the MIPS
    # registers kept in psp_cpu). Not everywhere: inline memory in all the
    # game's code took .text from 28 to 52 MB, more than the heap can give.
    if(GEN_HOT)
        set_source_files_properties(${GEN_HOT} PROPERTIES
            COMPILE_OPTIONS "-O2" COMPILE_DEFINITIONS PSPRECOMP_INLINE_MEM)
    endif()
    target_link_libraries(psp2i_game PRIVATE psprecomp)

    # the GE backend's vertex shader, assembled by picasso and linked in
    ctr_add_shader_library(c3d_ge_shader ${PORT}/host/c3d_ge.v.pica OUTPUT c3d_ge.shbin)
    dkp_add_embedded_binary_library(c3d_ge_shader_bin c3d_ge_shader)

    add_executable(psp2i ${PORT}/host/3ds_main.c ${PORT}/host/c3d_ge.c ${PORT}/host/camera.c
                         ${PORT}/host/savecrypt.c)
    target_compile_options(psp2i PRIVATE -O2 -Wall -Wno-format)
    target_compile_definitions(psp2i PRIVATE PSP2I_3DS)
    target_include_directories(psp2i PRIVATE ${PORT}/host)
    target_link_libraries(psp2i PRIVATE c3d_ge_shader_bin psp2i_game psprecomp citro3d ctru m)
    target_link_options(psp2i PRIVATE -Wl,--gc-sections -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/psp2i.map)
    psp2i_package(psp2i "PSP2i Recomp" "Phantasy Star Portable 2 Infinity" 0xF5221 CTR-P-P2I3)
    message(STATUS "PSP2i 3DS: building the game from ${PSP2I_GEN_DIR}")
else()
    message(STATUS "PSP2i 3DS: no ${PSP2I_GEN_DIR}/recomp_funcs.c; only the check app is built")
endif()
