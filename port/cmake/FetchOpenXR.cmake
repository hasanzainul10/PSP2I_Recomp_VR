# OpenXR for VR (vr/): the Khronos loader, fetched at configure time.
#
# vr/vr_xr_d3d11.c loads openxr_loader.dll at run time (LoadLibrary +
# xrGetInstanceProcAddr), so it is not a link dependency: without it, or without
# an OpenXR runtime, the game runs flat as before. The build needs only the
# headers; the DLL is copied next to the exe.
#
# psp2i_fetch_openxr(<inc-var> <dll-var>) sets <inc-var> to the include folder
# (with openxr/openxr.h) and <dll-var> to the x64 openxr_loader.dll, or empty
# when offline and not fetched before.

set(PSP2I_OPENXR_VERSION "1.1.63" CACHE STRING "OpenXR SDK release (KhronosGroup/OpenXR-SDK-Source)")

function(psp2i_fetch_openxr inc_out dll_out)
    set(dir "${CMAKE_BINARY_DIR}/_deps/openxr-${PSP2I_OPENXR_VERSION}")
    set(inc "${dir}/x/include")
    set(dll "${dir}/x/x64/bin/openxr_loader.dll")
    if(NOT EXISTS "${inc}/openxr/openxr.h")
        file(MAKE_DIRECTORY "${dir}")
        set(url "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-${PSP2I_OPENXR_VERSION}/openxr_loader_windows-${PSP2I_OPENXR_VERSION}.zip")
        message(STATUS "OpenXR: downloading the loader ${PSP2I_OPENXR_VERSION}")
        file(DOWNLOAD "${url}" "${dir}/openxr.zip" STATUS st TIMEOUT 120)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            message(WARNING "OpenXR: download failed (${st}); building without VR")
            set(${inc_out} "" PARENT_SCOPE)
            set(${dll_out} "" PARENT_SCOPE)
            return()
        endif()
        file(REMOVE_RECURSE "${dir}/x")
        file(ARCHIVE_EXTRACT INPUT "${dir}/openxr.zip" DESTINATION "${dir}/x" TOUCH)
    endif()
    set(${inc_out} "${inc}" PARENT_SCOPE)
    if(EXISTS "${dll}")
        set(${dll_out} "${dll}" PARENT_SCOPE)
    else()
        set(${dll_out} "" PARENT_SCOPE)
    endif()
endfunction()
