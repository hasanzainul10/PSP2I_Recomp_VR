# PSP2i for New 3DS (3DS_PLAN.md). Configure with devkitPro's toolchain:
#
#   cmake -S port/3ds -B port/3ds/build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
#
# (port/3ds/build.sh does this through MSYS2.) For now it builds the T0
# check app, psp2i_hello, as a .3dsx and a .cia; the game joins in T1.
cmake_minimum_required(VERSION 3.20)
project(psp2i_3ds C)

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
