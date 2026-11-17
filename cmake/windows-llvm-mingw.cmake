# Cross-compile the Windows client on Linux (bd remora-7oih).
#
#   tools/windows-sdk.sh                      # fetch the pinned Windows Qt/FFmpeg/toolchain once
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake
#   cmake --build build-win
#
# llvm-mingw, not the GCC mingw a distribution ships: Qt's official Windows builds come in MSVC and
# llvm-mingw flavours, and this links against them, so the C++ runtime has to be the same libc++
# Qt itself uses. Two runtimes in one process is an ABI break waiting for the first std:: type to
# cross a DLL boundary. The SDK pins the llvm-mingw release Qt 6.11.3 was built with.
#
# moc, rcc and uic run on THIS machine, so they come from a Linux Qt (QT_HOST_PATH) of exactly the
# Windows Qt's version: Qt refuses a mismatch at configure time. The SDK carries one (host-qt/), so
# the distribution's Qt version does not matter; without it, the system Qt is tried.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(DEFINED ENV{REMORA_WINDOWS_SDK})
    set(REMORA_WINDOWS_SDK "$ENV{REMORA_WINDOWS_SDK}")
elseif(NOT REMORA_WINDOWS_SDK)
    set(REMORA_WINDOWS_SDK "$ENV{HOME}/.cache/remora-windows-sdk")
endif()
if(NOT EXISTS "${REMORA_WINDOWS_SDK}/llvm-mingw/bin/x86_64-w64-mingw32-clang++")
    message(FATAL_ERROR "No Windows SDK at ${REMORA_WINDOWS_SDK} — run tools/windows-sdk.sh first "
        "(or point REMORA_WINDOWS_SDK at one)")
endif()

set(_tc "${REMORA_WINDOWS_SDK}/llvm-mingw/bin/x86_64-w64-mingw32")
set(CMAKE_C_COMPILER "${_tc}-clang")
set(CMAKE_CXX_COMPILER "${_tc}-clang++")
set(CMAKE_RC_COMPILER "${_tc}-windres")
set(CMAKE_AR "${_tc}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_tc}-ranlib" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH
    "${REMORA_WINDOWS_SDK}/qt"
    "${REMORA_WINDOWS_SDK}/ffmpeg"
    "${REMORA_WINDOWS_SDK}/llvm-mingw/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH "${REMORA_WINDOWS_SDK}/qt")
if(EXISTS "${REMORA_WINDOWS_SDK}/host-qt/lib/cmake/Qt6CoreTools")
    set(QT_HOST_PATH "${REMORA_WINDOWS_SDK}/host-qt" CACHE PATH "Host Qt for moc/rcc/uic")
    set(QT_HOST_PATH_CMAKE_DIR "${REMORA_WINDOWS_SDK}/host-qt/lib/cmake" CACHE PATH "")
else()
    set(QT_HOST_PATH "/usr" CACHE PATH "Host Qt for moc/rcc/uic (same version as the target Qt)")
endif()
# Named, not derived: Qt guesses ${QT_HOST_PATH}/lib/cmake, which is not where Gentoo, Fedora or
# Debian keep it, and when the guess misses Qt silently takes the Windows package's own tools —
# moc.exe, which this machine cannot run — and the failure only surfaces at generate time.
if(NOT QT_HOST_PATH_CMAKE_DIR)
    foreach(_d lib64/cmake lib/cmake lib/x86_64-linux-gnu/cmake)
        if(EXISTS "${QT_HOST_PATH}/${_d}/Qt6CoreTools")
            set(QT_HOST_PATH_CMAKE_DIR "${QT_HOST_PATH}/${_d}" CACHE PATH "")
            break()
        endif()
    endforeach()
endif()

# FFmpeg's .pc files are relocatable (prefix=${pcfiledir}/../..). LIBDIR, not PATH, so nothing of
# the host's own pkg-config tree can leak into a Windows link.
set(ENV{PKG_CONFIG_LIBDIR} "${REMORA_WINDOWS_SDK}/ffmpeg/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
