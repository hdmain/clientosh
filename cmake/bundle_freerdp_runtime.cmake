# Copy FreeRDP runtime DLLs (and their MinGW dependencies) next to clientosh.exe.
cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED FREERDP_BIN_DIR OR NOT DEFINED DEST_DIR)
    message(FATAL_ERROR "bundle_freerdp_runtime.cmake requires FREERDP_BIN_DIR and DEST_DIR")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")

set(_bash_candidates
    "C:/msys64/usr/bin/bash.exe"
    "C:/msys64/msys2.exe"
)
set(_bash "")
foreach(_candidate IN LISTS _bash_candidates)
    if(EXISTS "${_candidate}")
        set(_bash "${_candidate}")
        break()
    endif()
endforeach()

set(_copied 0)
if(_bash)
    file(TO_CMAKE_PATH "${FREERDP_BIN_DIR}" _bin_cmake)
    file(TO_CMAKE_PATH "${DEST_DIR}" _dest_cmake)
    string(REPLACE "\\" "/" _bin_unix "${_bin_cmake}")
    string(REPLACE "\\" "/" _dest_unix "${_dest_cmake}")
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_LIST_DIR}/bundle_freerdp_runtime.sh" _script_cmake)
    string(REPLACE "\\" "/" _script_unix "${_script_cmake}")

    execute_process(
        COMMAND "${_bash}" -lc "bash '${_script_unix}' '${_bin_unix}' '${_dest_unix}'"
        OUTPUT_VARIABLE _count
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc
    )
    if(_rc EQUAL 0)
        string(STRIP "${_count}" _count)
        if(_count MATCHES "^[0-9]+$")
            set(_copied "${_count}")
        endif()
    else()
        message(WARNING "FreeRDP runtime bundling via MSYS2 failed: ${_err}")
    endif()
endif()

if(_copied EQUAL 0)
    set(_fallback_dlls
        libfreerdp3.dll libfreerdp-client3.dll libwinpr3.dll
        libfreerdp2.dll libfreerdp-client2.dll libwinpr2.dll
        liburiparser-1.dll libcjson-1.dll
        avutil-60.dll avcodec-62.dll swscale-9.dll swresample-6.dll
        zlib1.dll libusb-1.0.dll
        libcrypto-3-x64.dll libssl-3-x64.dll
        libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll
    )
    foreach(_dll IN LISTS _fallback_dlls)
        set(_src "${FREERDP_BIN_DIR}/${_dll}")
        if(EXISTS "${_src}")
            file(COPY "${_src}" DESTINATION "${DEST_DIR}")
            math(EXPR _copied "${_copied} + 1")
        endif()
    endforeach()
endif()

message(STATUS "Bundled ${_copied} FreeRDP runtime DLL(s) -> ${DEST_DIR}")
