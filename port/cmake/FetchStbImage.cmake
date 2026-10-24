# PNG decoding for texture packs (host/texreplace.c): stb_image (public domain
# / MIT, Sean Barrett), one header, fetched at configure time at a fixed
# commit and checked against its SHA-256.
#
# psp2i_fetch_stb_image(<inc-out>) sets the directory holding stb_image.h, or
# empties it when offline (texture packs are then not loaded; the game is
# otherwise unchanged).

set(PSP2I_STB_COMMIT 013ac3beddff3dbffafd5177e7972067cd2b5083)   # stb_image 2.30

function(psp2i_fetch_stb_image inc_out)
    set(${inc_out} "" PARENT_SCOPE)
    set(dir "${CMAKE_BINARY_DIR}/_deps/stb")
    set(dest "${dir}/stb_image.h")
    if(NOT EXISTS "${dest}")
        message(STATUS "stb_image: fetching stb_image.h")
        file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/${PSP2I_STB_COMMIT}/stb_image.h" "${dest}"
             EXPECTED_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
             STATUS st TIMEOUT 60)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${dest}")
            message(WARNING "stb_image: could not fetch stb_image.h; texture packs will not load")
            return()
        endif()
    endif()
    set(${inc_out} "${dir}" PARENT_SCOPE)
endfunction()
